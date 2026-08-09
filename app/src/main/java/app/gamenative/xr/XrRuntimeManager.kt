package app.gamenative.xr

import android.content.Context
import app.gamenative.BuildConfig
import app.gamenative.MainActivity
import com.winlator.container.Container
import com.winlator.core.WineRegistryEditor
import com.winlator.core.envvars.EnvVars
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonObject
import timber.log.Timber

object XrRuntimeManager {
    private const val ENABLED_EXTRA = "xrRuntimeEnabled"
    private const val WINDOWS_XR_DIR = "C:\\gamenative\\xr"
    private const val RUNTIME_JSON_COMMON = "gamenative_openxr.json"
    private const val RUNTIME_JSON_64 = "gamenative_openxr64.json"
    private const val RUNTIME_JSON_32 = "gamenative_openxr32.json"
    private const val RUNTIME_DLL_COMMON = "C:\\windows\\system32\\gamenative_openxr.dll"
    private const val RUNTIME_DLL_64 = "gamenative_openxr64.dll"
    private const val RUNTIME_DLL_32 = "gamenative_openxr32.dll"
    private const val UNIX_BRIDGE_MODULE = "gamenative_xr_unixbridge"
    private const val OPENXR_REGISTRY_KEY_64 = "Software\\Khronos\\OpenXR\\1"
    // Wine's registry files are case-sensitive to the file editor even though
    // the Windows registry API is not. Match Wine's canonical key casing so we
    // replace an existing 32-bit runtime instead of creating a duplicate key.
    internal const val OPENXR_REGISTRY_KEY_32 =
        "Software\\Wow6432Node\\Khronos\\OpenXR\\1"
    private const val OPENXR_REGISTRY_KEY_32_LEGACY =
        "Software\\WOW6432Node\\Khronos\\OpenXR\\1"

    fun isEnabled(context: Context, container: Container): Boolean =
        BuildConfig.XR_BUILD &&
            MainActivity.isHeadset(context) &&
            container.getExtra(ENABLED_EXTRA, "true").toBooleanStrictOrNull() != false

    fun isOpenCompositeEnabled(context: Context, container: Container): Boolean =
        XrContainerSettings.read(context, container.id, container).openCompositeEnabled

    fun setOpenCompositeEnabled(context: Context, container: Container, enabled: Boolean) {
        val updated = XrContainerSettings.read(context, container.id, container).copy(
            openCompositeEnabled = enabled,
        )
        check(XrContainerSettings.persist(context, container.id, updated)) {
            "Could not persist OpenComposite setting for ${container.id}"
        }
        XrContainerSettings.write(container, updated)
        container.saveData()
    }

    fun prepareRuntime(
        context: Context,
        container: Container,
        envVars: EnvVars,
        active: Boolean,
    ) {
        if (!active || !isEnabled(context, container)) {
            clearOpenXrRegistry(container)
            return
        }

        val runtimeDir = File(container.rootDir, ".wine/drive_c/gamenative/xr").apply { mkdirs() }
        if (!XrPayloadManager.hasBridgePayload(container)) {
            clearOpenXrRegistry(container)
            Timber.e(
                "GameNativeVR OpenXR runtime is missing in %s. Not advertising XR_RUNTIME_JSON.",
                runtimeDir.path,
            )
            return
        }

        val runtimeJson64 = File(runtimeDir, RUNTIME_JSON_64)
        val runtimeJson32 = File(runtimeDir, RUNTIME_JSON_32)
        val runtimeJsonCommon = File(runtimeDir, RUNTIME_JSON_COMMON)
        writeRuntimeManifest(runtimeJson64, RUNTIME_DLL_64)
        writeRuntimeManifest(runtimeJson32, RUNTIME_DLL_32)
        writeRuntimeManifest(runtimeJsonCommon, RUNTIME_DLL_COMMON)

        writeOpenXrRegistry(container, runtimeJsonCommon, runtimeJsonCommon)

        envVars.put("GAMENATIVE_XR", "1")
        // The bridge DLL appends to C:\gamenative\xr\bridge.log when this is set.
        envVars.put("GAMENATIVE_XR_LOG", "1")
        // Use a concrete app-private path: the Wine/proot /tmp view is not the
        // ImageFs root that the Android diagnostics process can inspect.
        val unixLog = File(runtimeDir, "unix.log").apply { delete() }
        envVars.put("GAMENATIVE_XR_UNIX_LOG", unixLog.absolutePath)
        // The Quest compositor listens in Linux's abstract AF_UNIX namespace.
        // A filesystem /tmp path lives inside the container/proot view and cannot
        // be opened by the Android app process.
        envVars.put("GAMENATIVE_XR_SOCKET", "@gamenative-xr")
        envVars.put("GAMENATIVE_XR_BRIDGE_HOST", XrBridgeServer.HOST)
        envVars.put("GAMENATIVE_XR_BRIDGE_PORT", XrBridgeServer.PORT.toString())
        envVars.put("GAMENATIVE_XR_RUNTIME_DIR", runtimeDir.absolutePath)
        // This architecture-neutral manifest points into system32. Wine redirects
        // that path to syswow64 for a 32-bit loader, selecting the matching DLL
        // without requiring architecture-specific environment variables.
        envVars.put(
            "XR_RUNTIME_JSON",
            "$WINDOWS_XR_DIR\\$RUNTIME_JSON_COMMON",
        )
        // The PE companion is a Wine builtin module with an associated Unix
        // library. If this override is absent, Wine tries to treat the staged
        // PE file as a regular native DLL and never attaches its unixlib.
        envVars.put(
            "WINEDLLOVERRIDES",
            runtimeDllOverrides(envVars.get("WINEDLLOVERRIDES")),
        )
        if (isOpenCompositeEnabled(context, container)) {
            val openCompositeDir = File(container.rootDir, ".wine/drive_c/gamenative/opencomposite")
            if (openCompositeDir.isDirectory) {
                envVars.put("VR_OVERRIDE", openCompositeDir.absolutePath)
                envVars.put("OPENCOMPOSITE_LOG", "1")
            } else {
                Timber.w("OpenComposite requested but payload directory is missing: %s", openCompositeDir.path)
            }
        }
    }

    private fun writeRuntimeManifest(target: File, runtimeDll: String) {
        target.writeText(runtimeManifest(runtimeDll))
    }

    internal fun runtimeManifest(runtimeDll: String): String =
        buildJsonObject {
            put("file_format_version", "1.0.0")
            putJsonObject("runtime") {
                put("name", "GameNativeVR")
                put(
                    "library_path",
                    if (runtimeDll.length >= 3 &&
                        runtimeDll[1] == ':' &&
                        runtimeDll[2] == '\\'
                    ) {
                        runtimeDll
                    } else {
                        "$WINDOWS_XR_DIR\\$runtimeDll"
                    },
                )
            }
        }.toString()

    internal fun runtimeDllOverrides(existing: String): String {
        val xrOverride = "$UNIX_BRIDGE_MODULE=b"
        val withoutTrailingSeparators = existing.trim().trimEnd(';')
        return if (withoutTrailingSeparators.isEmpty()) {
            xrOverride
        } else {
            "$withoutTrailingSeparators;$xrOverride"
        }
    }

    fun diagnosticSummary(container: Container): String {
        val runtimeDir = File(container.rootDir, ".wine/drive_c/gamenative/xr")
        val manifest64 = File(runtimeDir, RUNTIME_JSON_64)
        val manifest32 = File(runtimeDir, RUNTIME_JSON_32)
        val manifestCommon = File(runtimeDir, RUNTIME_JSON_COMMON)
        val systemReg = File(container.rootDir, ".wine/system.reg")
        var registry64: String? = null
        var registry32: String? = null
        runCatching {
            WineRegistryEditor(systemReg).use { registry ->
                registry64 = registry.getStringValue(
                    OPENXR_REGISTRY_KEY_64,
                    "ActiveRuntime",
                )
                registry32 = registry.getStringValue(
                    OPENXR_REGISTRY_KEY_32,
                    "ActiveRuntime",
                )
            }
        }

        return buildString {
            appendLine("GameNativeVR OpenXR discovery state:")
            appendLine("  environmentRuntime=$WINDOWS_XR_DIR\\$RUNTIME_JSON_COMMON")
            appendLine("  registry64=${registry64 ?: "<missing>"}")
            appendLine("  registry32=${registry32 ?: "<missing>"}")
            appendManifestDiagnostic("manifestCommon", manifestCommon)
            appendManifestDiagnostic("manifest64", manifest64)
            appendManifestDiagnostic("manifest32", manifest32)
        }.trimEnd()
    }

    private fun StringBuilder.appendManifestDiagnostic(label: String, manifest: File) {
        val contents = runCatching { manifest.readText() }.getOrNull()
        val valid = contents != null && runCatching {
            Json.parseToJsonElement(contents)
        }.isSuccess
        appendLine(
            "  $label=${manifest.path} validJson=$valid " +
                "contents=${contents?.replace("\r", "")?.replace("\n", "") ?: "<unavailable>"}",
        )
    }

    private fun writeOpenXrRegistry(
        container: Container,
        runtimeJson64: File,
        runtimeJson32: File,
    ) {
        val systemReg = File(container.rootDir, ".wine/system.reg")
        val winePath64 = "C:\\gamenative\\xr\\${runtimeJson64.name}"
        val winePath32 = "C:\\gamenative\\xr\\${runtimeJson32.name}"
        try {
            WineRegistryEditor(systemReg).use { registry ->
                registry.setCreateKeyIfNotExist(true)
                registry.removeValue(OPENXR_REGISTRY_KEY_32_LEGACY, "ActiveRuntime")
                registry.setStringValue(
                    OPENXR_REGISTRY_KEY_64,
                    "ActiveRuntime",
                    winePath64,
                )
                registry.setStringValue(
                    OPENXR_REGISTRY_KEY_32,
                    "ActiveRuntime",
                    winePath32,
                )
            }
        } catch (e: Exception) {
            Timber.w(e, "Failed to write Wine OpenXR runtime registry entries")
        }
    }

    private fun clearOpenXrRegistry(container: Container) {
        val systemReg = File(container.rootDir, ".wine/system.reg")
        try {
            WineRegistryEditor(systemReg).use { registry ->
                registry.removeValue(OPENXR_REGISTRY_KEY_64, "ActiveRuntime")
                registry.removeValue(OPENXR_REGISTRY_KEY_32, "ActiveRuntime")
                registry.removeValue(OPENXR_REGISTRY_KEY_32_LEGACY, "ActiveRuntime")
            }
        } catch (e: Exception) {
            Timber.w(e, "Failed to clear Wine OpenXR runtime registry entries")
        }
    }
}
