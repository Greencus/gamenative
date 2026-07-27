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
    fun `ExeRunDir is exe directory when no workingDir`() {
        val ini = generate(gameName = "New Star GP", executablePath = "release/NSGP.exe", workingDir = null)
        assertEquals("steamapps\\common\\New Star GP\\release", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `ExeRunDir is blank when workingDir is set`() {
        // workingDir set: leave blank (legacy behaviour, same as master)
        val ini = generate(workingDir = "Binaries")
        assertEquals("", ini.iniValue("ExeRunDir"))
    }

    @Test
    fun `ExeRunDir is exe directory when workingDir is empty string`() {
        val ini = generate(gameName = "New Star GP", executablePath = "release/NSGP.exe", workingDir = "")
        assertEquals("steamapps\\common\\New Star GP\\release", ini.iniValue("ExeRunDir"))
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
