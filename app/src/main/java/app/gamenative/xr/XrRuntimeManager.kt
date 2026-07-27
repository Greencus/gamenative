package app.gamenative.xr

import android.content.Context
import app.gamenative.BuildConfig
import app.gamenative.MainActivity
import com.winlator.container.Container
import com.winlator.core.WineRegistryEditor
import com.winlator.core.envvars.EnvVars
import java.io.File
import timber.log.Timber

object XrRuntimeManager {
    private const val ENABLED_EXTRA = "xrRuntimeEnabled"
    private const val OPENCOMPOSITE_EXTRA = "xrOpenCompositeEnabled"
    private const val WINDOWS_XR_DIR = "C:\\gamenative\\xr"
    private const val RUNTIME_JSON_64 = "gamenative_openxr64.json"
    private const val RUNTIME_JSON_32 = "gamenative_openxr32.json"
    private const val RUNTIME_DLL_64 = "gamenative_openxr64.dll"
    private const val RUNTIME_DLL_32 = "gamenative_openxr32.dll"

    fun isEnabled(context: Context, container: Container): Boolean =
        BuildConfig.XR_BUILD &&
            MainActivity.isHeadset(context) &&
            container.getExtra(ENABLED_EXTRA, "true").toBooleanStrictOrNull() != false

    fun isOpenCompositeEnabled(container: Container): Boolean =
        container.getExtra(OPENCOMPOSITE_EXTRA, "true").toBooleanStrictOrNull() != false

    fun setOpenCompositeEnabled(container: Container, enabled: Boolean) {
        container.putExtra(OPENCOMPOSITE_EXTRA, enabled)
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
        writeRuntimeManifest(runtimeJson64, RUNTIME_DLL_64)
        writeRuntimeManifest(runtimeJson32, RUNTIME_DLL_32)

        writeOpenXrRegistry(container, runtimeJson64, runtimeJson32)

        envVars.put("GAMENATIVE_XR", "1")
        // The bridge DLL appends to C:\gamenative\xr\bridge.log when this is set.
        envVars.put("GAMENATIVE_XR_LOG", "1")
        // The Quest compositor listens in Linux's abstract AF_UNIX namespace.
        // A filesystem /tmp path lives inside the container/proot view and cannot
        // be opened by the Android app process.
        envVars.put("GAMENATIVE_XR_SOCKET", "@gamenative-xr")
        envVars.put("GAMENATIVE_XR_BRIDGE_HOST", XrBridgeServer.HOST)
        envVars.put("GAMENATIVE_XR_BRIDGE_PORT", XrBridgeServer.PORT.toString())
        envVars.put("GAMENATIVE_XR_RUNTIME_DIR", runtimeDir.absolutePath)
        // Do not set XR_RUNTIME_JSON here. One environment variable is inherited by
        // both PE architectures and therefore cannot select the matching runtime DLL.
        // The Wine OpenXR loader selects the x64/x86 manifest through the corresponding
        // native/WOW6432 registry view below.

        if (isOpenCompositeEnabled(container)) {
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
        target.writeText(
            """
            {
              "file_format_version": "1.0.0",
              "runtime": {
                "library_path": "$WINDOWS_XR_DIR\\$runtimeDll"
              }
            }
            """.trimIndent(),
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
                registry.setStringValue(
                    "Software\\Khronos\\OpenXR\\1",
                    "ActiveRuntime",
                    winePath64,
                )
                registry.setStringValue(
                    "Software\\WOW6432Node\\Khronos\\OpenXR\\1",
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
                registry.removeValue("Software\\Khronos\\OpenXR\\1", "ActiveRuntime")
                registry.removeValue("Software\\WOW6432Node\\Khronos\\OpenXR\\1", "ActiveRuntime")
            }
        } catch (e: Exception) {
            Timber.w(e, "Failed to clear Wine OpenXR runtime registry entries")
        }
    }
}
