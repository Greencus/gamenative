package app.gamenative.utils

import java.io.File
import kotlin.io.path.createTempDirectory
import org.junit.Assert.assertEquals
import org.junit.Test

class SteamUtilsColdClientIniTest {

    private fun generate(
        gameName: String = "Batman Arkham Asylum GOTY",
        executablePath: String = "Binaries\\BmLauncher.exe",
        exeCommandLine: String = "",
        steamAppId: Int = 35140,
        workingDir: String? = null,
        isUnpackFiles: Boolean = false,
    ) = SteamUtils.generateColdClientIni(
        gameName = gameName,
        executablePath = executablePath,
        exeCommandLine = exeCommandLine,
        steamAppId = steamAppId,
        workingDir = workingDir,
        isUnpackFiles = isUnpackFiles,
    )

    private fun String.iniValue(key: String): String =
        lines().first { it.startsWith("$key=") }.removePrefix("$key=")

    @Test
    fun `ColdClient paths use the absolute mapped game drive`() {
        val ini = generate(gameName = "New Star GP", executablePath = "release/NSGP.exe", workingDir = null)
        assertEquals("A:\\release\\NSGP.exe", ini.iniValue("Exe"))
        assertEquals("A:\\release", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `root game executable uses an absolute root working directory`() {
        val ini = generate(gameName = "Beat Saber", executablePath = "Beat Saber.exe", workingDir = null)
        assertEquals("A:\\Beat Saber.exe", ini.iniValue("Exe"))
        assertEquals("A:\\", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `ExeRunDir honors a Steam working directory`() {
        val ini = generate(workingDir = "Binaries")
        assertEquals("A:\\Binaries", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `ExeRunDir is exe directory when workingDir is empty string`() {
        val ini = generate(gameName = "New Star GP", executablePath = "release/NSGP.exe", workingDir = "")
        assertEquals("A:\\release", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `ExeRunDir normalizes install-dir placeholders without regex`() {
        listOf(
            "%INSTALLDIR%/release",
            "\${INSTALLDIR}/release",
            "\$INSTALLDIR/release",
            "%installdir%/release",
        ).forEach { workingDir ->
            val ini = generate(
                gameName = "New Star GP",
                executablePath = "release/NSGP.exe",
                workingDir = workingDir,
            )
            assertEquals("A:\\release", ini.iniValue("ExeRunDir"))
        }
    }

    @Test
    fun `resolveLaunchExecutablePath strips install folder prefix`() {
        withTempDir { appDir ->
            File(appDir, "release").mkdirs()
            File(appDir, "release/NSGP.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "New Star GP",
                "New Star GP/release/NSGP.exe",
            )

            assertEquals("release/NSGP.exe", resolved)
        }
    }

    @Test
    fun `resolveLaunchExecutablePath strips steamapps common prefix`() {
        withTempDir { appDir ->
            File(appDir, "release").mkdirs()
            File(appDir, "release/NSGP.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "New Star GP",
                "C:/Program Files (x86)/Steam/steamapps/common/New Star GP/release/NSGP.exe",
            )

            assertEquals("release/NSGP.exe", resolved)
        }
    }

    @Test
    fun `resolveLaunchExecutablePath strips a legacy drive prefix`() {
        withTempDir { appDir ->
            File(appDir, "release").mkdirs()
            File(appDir, "release/NSGP.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "New Star GP",
                "A:/release/NSGP.exe",
            )

            assertEquals("release/NSGP.exe", resolved)
        }
    }

    @Test
    fun `resolveLaunchExecutablePath accepts the actual folder when metadata name differs`() {
        withTempDir { rootDir ->
            val appDir = File(rootDir, "Actual Install Folder").apply { mkdirs() }
            File(appDir, "bin").mkdirs()
            File(appDir, "bin/Game.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "Outdated Metadata Folder",
                "Actual Install Folder/bin/Game.exe",
            )

            assertEquals("bin/Game.exe", resolved)
        }
    }

    @Test
    fun `resolveLaunchExecutablePath ignores stale candidates and uses a valid fallback`() {
        withTempDir { appDir ->
            File(appDir, "bin").mkdirs()
            File(appDir, "bin/Game.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "Game",
                "missing/Old.exe",
                "bin/Game.exe",
            )

            assertEquals("bin/Game.exe", resolved)
        }
    }

    @Test
    fun `resolveLaunchExecutablePath returns blank when no candidate exists`() {
        withTempDir { appDir ->
            File(appDir, "Release").mkdirs()
            File(appDir, "Release/NSGP.exe").writeText("exe")

            val resolved = SteamUtils.resolveLaunchExecutablePath(
                appDir = appDir,
                gameName = "New Star GP",
                "release/Missing.exe",
            )

            assertEquals("", resolved)
        }
    }

    private fun withTempDir(block: (File) -> Unit) {
        val dir = createTempDirectory(prefix = "steam-utils-test")
        val dirFile = dir.toFile()
        try {
            block(dirFile)
        } finally {
            dirFile.deleteRecursively()
        }
    }

}
