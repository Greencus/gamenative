package app.gamenative.xr

import android.content.Context
import app.gamenative.data.GameSource
import app.gamenative.service.SteamService
import app.gamenative.utils.ContainerUtils
import app.gamenative.utils.Net
import com.winlator.container.Container
import com.winlator.contents.ContentsManager
import com.winlator.core.FileUtils
import com.winlator.core.WineInfo
import com.winlator.xenvironment.ImageFs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.Request
import timber.log.Timber
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.Properties

object XrPayloadManager {
    private const val OPENCOMPOSITE_X64_URL =
        "https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr"
    private const val OPENCOMPOSITE_X86_URL =
        "https://znix.xyz/OpenComposite/download.php?arch=x86&branch=openxr"

    private const val BRIDGE_DLL_64 = "gamenative_openxr64.dll"
    private const val BRIDGE_DLL_32 = "gamenative_openxr32.dll"
    private const val BRIDGE_UNIXLIB = "gamenative_openxr.so"
    private const val UNIX_BRIDGE_DLL_64 = "gamenative_xr_unixbridge64.dll"
    private const val UNIX_BRIDGE_DLL_32 = "gamenative_xr_unixbridge32.dll"
    private const val UNIX_BRIDGE_MODULE = "gamenative_xr_unixbridge"
    private const val PATCHED_WINE_IDENTIFIER = "wine-9.2-x86_64"
    private const val WINE_VULKAN_MODULE = "winevulkan.so"
    private const val WINE_VULKAN_BACKUP_SUFFIX = ".gamenative-stock"

    private const val OPENVR_BACKUP_SUFFIX = ".gamenative-valve.bak"
    private const val OPENCOMPOSITE_CONFIG_BACKUP = "opencomposite.ini.gamenative.bak"
    private const val OPENCOMPOSITE_CONFIG_MARKER = "; Written by GameNative."
    private const val OPENCOMPOSITE_CHECK_INTERVAL_MS = 24L * 60L * 60L * 1000L

    suspend fun prepare(context: Context, container: Container, appId: String) = withContext(Dispatchers.IO) {
        prepareBridgePayload(context, container)
        prepareUnixLib(context, container)
        if (XrRuntimeManager.isOpenCompositeEnabled(container)) {
            prepareOpenComposite(context, container, appId)
        } else {
            restorePerGameOpenComposite(container, appId)
        }
    }

    /**
     * Undo [installPerGameOpenComposite]: put every backed-up stock openvr_api.dll back in
     * place and drop the backup, so disabling the per-game toggle (or launching flat) runs
     * against Valve's original DLLs again.
     */
    fun restorePerGameOpenComposite(container: Container, appId: String) {
        val appDir = gameInstallDir(container, appId) ?: return
        appDir.walkTopDown()
            .filter { it.isFile && it.name.endsWith(OPENVR_BACKUP_SUFFIX, ignoreCase = true) }
            .forEach { backup ->
                val original = File(backup.parentFile, backup.name.removeSuffix(OPENVR_BACKUP_SUFFIX))
                runCatching {
                    FileUtils.copy(backup, original)
                    backup.delete()
                    original.parentFile?.let(::restoreOpenCompositeConfig)
                    Timber.i("Restored stock OpenVR DLL: ${original.path}")
                }.onFailure { error ->
                    Timber.w(error, "Failed to restore stock OpenVR DLL at ${original.path}")
                }
            }
    }

    private fun steamAppDir(appId: String): File? {
        if (ContainerUtils.extractGameSourceFromContainerId(appId) != GameSource.STEAM) return null
        val gameId = ContainerUtils.extractGameIdFromContainerId(appId)
        return File(SteamService.getAppDirPath(gameId)).takeIf { it.isDirectory }
    }

    fun bridgeDll64(container: Container): File =
        File(container.rootDir, ".wine/drive_c/gamenative/xr/$BRIDGE_DLL_64")

    fun bridgeDll32(container: Container): File =
        File(container.rootDir, ".wine/drive_c/gamenative/xr/$BRIDGE_DLL_32")

    fun hasBridgePayload(container: Container): Boolean =
        bridgeDll64(container).isFile && bridgeDll32(container).isFile

    private fun prepareBridgePayload(context: Context, container: Container) {
        val targetDir = File(container.rootDir, ".wine/drive_c/gamenative/xr").apply { mkdirs() }
        val assetDir = "xr/windows"
        copyAssetIfExists(context, "$assetDir/$BRIDGE_DLL_64", File(targetDir, BRIDGE_DLL_64))
        copyAssetIfExists(context, "$assetDir/$BRIDGE_DLL_32", File(targetDir, BRIDGE_DLL_32))

        if (!File(targetDir, BRIDGE_DLL_64).isFile || !File(targetDir, BRIDGE_DLL_32).isFile) {
            Timber.w(
                "GameNativeVR OpenXR payload is incomplete in %s. Both x64 and x86 DLLs are required.",
                targetDir.path,
            )
        }
    }

    private fun prepareUnixLib(context: Context, container: Container) {
        val unixArch = if (container.wineVersion.contains("arm64ec", ignoreCase = true)) {
            "aarch64"
        } else {
            "x86_64"
        }
        val wineDir = wineInstallDir(context, container)
        val unixTarget = File(wineDir, "lib/wine/$unixArch-unix/$UNIX_BRIDGE_MODULE.so")
        val x64Target = File(wineDir, "lib/wine/x86_64-windows/$UNIX_BRIDGE_MODULE.dll")
        val x86Target = File(wineDir, "lib/wine/i386-windows/$UNIX_BRIDGE_MODULE.dll")
        val prefixX64Target =
            File(container.rootDir, ".wine/drive_c/windows/system32/$UNIX_BRIDGE_MODULE.dll")
        val prefixX86Target =
            File(container.rootDir, ".wine/drive_c/windows/syswow64/$UNIX_BRIDGE_MODULE.dll")
        copyAssetIfExists(context, "xr/unix/$unixArch/$BRIDGE_UNIXLIB", unixTarget)
        copyAssetIfExists(context, "xr/windows/$UNIX_BRIDGE_DLL_64", x64Target)
        copyAssetIfExists(context, "xr/windows/$UNIX_BRIDGE_DLL_32", x86Target)
        copyAssetIfExists(context, "xr/windows/$UNIX_BRIDGE_DLL_64", prefixX64Target)
        copyAssetIfExists(context, "xr/windows/$UNIX_BRIDGE_DLL_32", prefixX86Target)
        if (!unixTarget.isFile || !unixTarget.hasElfMagic()) {
            throw IOException("Wine unixlib payload is missing or invalid: ${unixTarget.path}")
        }
        if (x64Target.peMachine() != PeMachine.X64 ||
            x86Target.peMachine() != PeMachine.X86 ||
            prefixX64Target.peMachine() != PeMachine.X64 ||
            prefixX86Target.peMachine() != PeMachine.X86
        ) {
            throw IOException(
                "Wine unix bridge PE payloads are missing or invalid: " +
                    "${x64Target.path}, ${x86Target.path}, " +
                    "${prefixX64Target.path}, ${prefixX86Target.path}",
            )
        }
        // Wine is spawned under the app UID, so owner-readable permissions are sufficient.
        unixTarget.setReadable(true, true)
        x64Target.setReadable(true, true)
        x86Target.setReadable(true, true)
        prefixX64Target.setReadable(true, true)
        prefixX86Target.setReadable(true, true)
        prepareWineVulkanInterop(context, container, wineDir)
        Timber.i(
            "Installed GameNativeVR Wine unix bridge: %s (%s), %s, %s",
            unixTarget.path,
            unixTarget.sha256(),
            x64Target.path,
            x86Target.path,
        )
    }

    /**
     * Wine intentionally hides Linux dma-buf and sync-file extensions from a
     * Windows Vulkan application. The version-pinned module keeps those
     * extensions hidden while enabling them on Wine's host VkDevice for the XR
     * unixlib. Its behavior is gated by GAMENATIVE_XR, so non-XR launches use
     * the normal Wine path even though the module is installed globally.
     */
    private fun prepareWineVulkanInterop(context: Context, container: Container, wineDir: File) {
        if (container.wineVersion != PATCHED_WINE_IDENTIFIER) {
            Timber.w(
                "GameNativeVR dma-buf producer interop is version-pinned to %s; current Wine is %s",
                PATCHED_WINE_IDENTIFIER,
                container.wineVersion,
            )
            return
        }

        val target = File(wineDir, "lib/wine/x86_64-unix/$WINE_VULKAN_MODULE")
        val backup = File(target.parentFile, "$WINE_VULKAN_MODULE$WINE_VULKAN_BACKUP_SUFFIX")
        val assetPath =
            "xr/wine/$PATCHED_WINE_IDENTIFIER/x86_64-unix/$WINE_VULKAN_MODULE"
        val expectedHash = assetSha256(context, assetPath)
        if (target.isFile && !backup.isFile && target.sha256() != expectedHash) {
            backup.parentFile?.mkdirs()
            if (!FileUtils.copy(target, backup) ||
                target.length() != backup.length() ||
                target.sha256() != backup.sha256()
            ) {
                throw IOException("Could not preserve stock Wine Vulkan module: ${target.path}")
            }
        }

        copyAssetIfExists(context, assetPath, target)
        if (!target.isFile || !target.hasElfMagic() ||
            target.sha256() != expectedHash
        ) {
            throw IOException("Patched Wine Vulkan module is missing or invalid: ${target.path}")
        }
        target.setReadable(true, true)
        target.setExecutable(true, true)
        Timber.i("Installed GameNativeVR Wine Vulkan interop: %s (%s)", target.path, target.sha256())
    }

    private fun wineInstallDir(context: Context, container: Container): File {
        val contentsManager = ContentsManager(context).also { it.syncContents() }
        val wineInfo = WineInfo.fromIdentifier(context, contentsManager, container.wineVersion)
        return wineInfo.path?.takeIf { it.isNotEmpty() }?.let(::File)
            ?: File(ImageFs.find(context).rootDir, "opt/wine")
    }

    private fun copyAssetIfExists(context: Context, assetPath: String, target: File) {
        runCatching {
            context.assets.open(assetPath).use { input ->
                target.parentFile?.mkdirs()
                target.outputStream().use { output -> input.copyTo(output) }
            }
        }.onFailure { error ->
            if (error !is IOException) {
            Timber.w(error, "Failed to copy GameNativeVR payload asset $assetPath")
            }
        }
    }

    private fun prepareOpenComposite(context: Context, container: Container, appId: String) {
        val cacheDir = File(context.filesDir, "xr/opencomposite").apply { mkdirs() }
        val x64Dll = downloadIfMissing(
            File(cacheDir, "openvr_api_x64.dll"),
            OPENCOMPOSITE_X64_URL,
            PeMachine.X64,
        )
        val x86Dll = downloadIfMissing(
            File(cacheDir, "openvr_api_x86.dll"),
            OPENCOMPOSITE_X86_URL,
            PeMachine.X86,
        )

        val openCompositeDir = File(container.rootDir, ".wine/drive_c/gamenative/opencomposite")
        val runtimeBin = File(openCompositeDir, "Runtime/bin").apply { mkdirs() }
        val x64Dir = File(openCompositeDir, "x64").apply { mkdirs() }
        val x86Dir = File(openCompositeDir, "Win32").apply { mkdirs() }

        copyAndVerify(x64Dll, File(runtimeBin, "vrclient_x64.dll"))
        copyAndVerify(x86Dll, File(runtimeBin, "vrclient.dll"))
        copyAndVerify(x64Dll, File(x64Dir, "openvr_api.dll"))
        copyAndVerify(x86Dll, File(x86Dir, "openvr_api.dll"))

        writeOpenCompositeConfig(File(openCompositeDir, "opencomposite.ini"))

        installPerGameOpenComposite(container, appId, x64Dll, x86Dll)
    }

    private fun writeOpenCompositeConfig(target: File) {
        target.parentFile?.mkdirs()
        target.writeText(
            """
            $OPENCOMPOSITE_CONFIG_MARKER
            ; Avoid a second Vulkan bootstrap before the game creates its OpenXR graphics binding.
            initUsingVulkan=false
            logAllOpenVRCalls=false
            """.trimIndent(),
        )
    }

    private fun installPerGameOpenCompositeConfig(directory: File) {
        val config = File(directory, "opencomposite.ini")
        val backup = File(directory, OPENCOMPOSITE_CONFIG_BACKUP)
        if (config.isFile &&
            !config.readText().startsWith(OPENCOMPOSITE_CONFIG_MARKER) &&
            !backup.isFile
        ) {
            if (!FileUtils.copy(config, backup)) {
                throw IOException("Could not back up OpenComposite config: ${config.path}")
            }
        }
        writeOpenCompositeConfig(config)
    }

    private fun restoreOpenCompositeConfig(directory: File) {
        val config = File(directory, "opencomposite.ini")
        val backup = File(directory, OPENCOMPOSITE_CONFIG_BACKUP)
        when {
            backup.isFile -> {
                FileUtils.copy(backup, config)
                backup.delete()
            }
            config.isFile &&
                runCatching { config.readText().startsWith(OPENCOMPOSITE_CONFIG_MARKER) }
                    .getOrDefault(false) -> config.delete()
        }
    }

    private fun downloadIfMissing(
        target: File,
        url: String,
        expectedMachine: PeMachine,
    ): File {
        val metadataFile = File(target.parentFile, "${target.name}.metadata")
        val metadata = Properties().apply {
            runCatching { metadataFile.inputStream().use { input -> load(input) } }
        }
        val storedSha256 = metadata.getProperty("sha256")
        val validCached =
            target.isFile &&
                target.length() > 0L &&
                target.peMachine() == expectedMachine &&
                (storedSha256 == null ||
                    runCatching { target.sha256().equals(storedSha256, ignoreCase = true) }
                        .getOrDefault(false))
        val lastChecked = metadata.getProperty("checkedAt")?.toLongOrNull() ?: 0L
        if (validCached && System.currentTimeMillis() - lastChecked < OPENCOMPOSITE_CHECK_INTERVAL_MS) {
            return target
        }

        Timber.i("Checking OpenComposite payload: $url")
        val request = Request.Builder()
            .url(url)
            .apply {
                if (validCached) {
                    metadata.getProperty("etag")?.let { header("If-None-Match", it) }
                    metadata.getProperty("lastModified")?.let { header("If-Modified-Since", it) }
                }
            }
            .build()
        val response = try {
            Net.http.newCall(request).execute()
        } catch (error: IOException) {
            if (validCached) {
                Timber.w(error, "OpenComposite update check failed; using verified cache")
                return target
            }
            throw error
        }
        response.use {
            if (response.code == 304 && validCached) {
                metadata["checkedAt"] = System.currentTimeMillis().toString()
                metadata["sha256"] = target.sha256()
                metadataFile.outputStream().use { metadata.store(it, "OpenComposite cache metadata") }
                return target
            }
            if (!response.isSuccessful) {
                if (validCached) {
                    Timber.w("OpenComposite update check failed (HTTP ${response.code}); using verified cache")
                    return target
                }
                throw IOException("OpenComposite download failed: HTTP ${response.code} $url")
            }
            val body = response.body ?: throw IOException("OpenComposite download had no body: $url")
            target.parentFile?.mkdirs()
            val pending = File(target.parentFile, "${target.name}.download")
            pending.outputStream().use { output -> body.byteStream().copyTo(output) }
            if (pending.length() <= 0L || pending.peMachine() != expectedMachine) {
                pending.delete()
                throw IOException(
                    "OpenComposite download was not a valid $expectedMachine Windows DLL: $url",
                )
            }
            if (target.exists() && !target.delete()) {
                pending.delete()
                throw IOException("Could not replace cached OpenComposite DLL: ${target.path}")
            }
            if (!pending.renameTo(target)) {
                pending.delete()
                throw IOException("Could not commit OpenComposite download: ${target.path}")
            }
            response.header("ETag")?.let { metadata["etag"] = it }
            response.header("Last-Modified")?.let { metadata["lastModified"] = it }
        }

        metadata["checkedAt"] = System.currentTimeMillis().toString()
        metadata["sha256"] = target.sha256()
        metadataFile.outputStream().use { metadata.store(it, "OpenComposite cache metadata") }
        return target
    }

    private fun installPerGameOpenComposite(container: Container, appId: String, x64Dll: File, x86Dll: File) {
        val appDir = gameInstallDir(container, appId) ?: run {
            Timber.w("Could not resolve game directory for OpenComposite: $appId")
            return
        }
        val isSteam = ContainerUtils.extractGameSourceFromContainerId(appId) == GameSource.STEAM
        val gameId = if (isSteam) ContainerUtils.extractGameIdFromContainerId(appId) else -1

        val executable = container.executablePath.takeIf { it.isNotBlank() }
            ?: if (isSteam) SteamService.getInstalledExe(gameId) else ""
        val executableMachine = File(appDir, executable).takeIf { it.isFile }?.peMachine()

        appDir.walkTopDown()
            .filter { it.isFile && it.name.equals("openvr_api.dll", ignoreCase = true) }
            .forEach { dll ->
                val backup = File(dll.parentFile, "${dll.name}$OPENVR_BACKUP_SUFFIX")
                if (!backup.isFile) {
                    FileUtils.copy(dll, backup)
                }
                // Machine type must come from the backed-up stock DLL: after a previous
                // install the in-place DLL is already OpenComposite's.
                val machine = backup.peMachine() ?: executableMachine
                val source = if (machine == PeMachine.X86) x86Dll else x64Dll
                copyAndVerify(source, dll)
                // OpenComposite resolves its configuration next to openvr_api.dll first.
                // Put the same known-good settings beside every per-game replacement.
                dll.parentFile?.let(::installPerGameOpenCompositeConfig)
                Timber.i("Installed and verified OpenComposite per-game DLL: ${dll.path}")
            }
    }

    private fun gameInstallDir(container: Container, appId: String): File? {
        steamAppDir(appId)?.let { return it }
        val executable = container.executablePath.trim().replace('\\', '/')
        if (executable.length < 3 || executable[1] != ':') return null
        val drive = executable.substring(0, 2).lowercase()
        val relative = executable.substring(2).trimStart('/')
        val mappedExecutable = File(container.rootDir, ".wine/dosdevices/$drive/$relative")
        return mappedExecutable.parentFile?.takeIf { it.isDirectory }
    }

    private fun copyAndVerify(source: File, target: File) {
        target.parentFile?.mkdirs()
        if (!FileUtils.copy(source, target)) {
            throw IOException("Failed to copy ${source.path} to ${target.path}")
        }
        if (source.length() != target.length() ||
            source.peMachine() != target.peMachine() ||
            source.sha256() != target.sha256()
        ) {
            throw IOException("OpenComposite copy verification failed: ${target.path}")
        }
    }

    private fun File.sha256(): String {
        val digest = MessageDigest.getInstance("SHA-256")
        inputStream().use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun assetSha256(context: Context, assetPath: String): String {
        val digest = MessageDigest.getInstance("SHA-256")
        context.assets.open(assetPath).use { input ->
            val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun File.hasElfMagic(): Boolean = runCatching {
        inputStream().use { input ->
            val magic = ByteArray(4)
            input.read(magic) == 4 &&
                magic[0] == 0x7f.toByte() &&
                magic[1] == 'E'.code.toByte() &&
                magic[2] == 'L'.code.toByte() &&
                magic[3] == 'F'.code.toByte()
        }
    }.getOrDefault(false)

    private enum class PeMachine {
        X86,
        X64,
    }

    private fun File.peMachine(): PeMachine? = runCatching {
        inputStream().use { input ->
            val mz = ByteArray(64)
            if (input.read(mz) != mz.size) return@runCatching null
            if (mz[0] != 'M'.code.toByte() || mz[1] != 'Z'.code.toByte()) return@runCatching null
            val peOffset = ByteBuffer.wrap(mz, 0x3c, 4).order(ByteOrder.LITTLE_ENDIAN).int
            if (peOffset < 0) return@runCatching null
            input.channel.position(peOffset.toLong())
            val header = ByteArray(6)
            if (input.read(header) != header.size) return@runCatching null
            if (header[0] != 'P'.code.toByte() || header[1] != 'E'.code.toByte()) return@runCatching null
            val machine = ByteBuffer.wrap(header, 4, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() and 0xffff
            when (machine) {
                0x014c -> PeMachine.X86
                0x8664 -> PeMachine.X64
                else -> null
            }
        }
    }.getOrNull()
}
