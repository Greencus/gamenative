package app.gamenative.xr

import app.gamenative.data.LaunchInfo
import app.gamenative.enums.OS
import app.gamenative.enums.OSArch
import java.util.EnumSet
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class XrLaunchPreferencesTest {
    @Test
    fun `recognizes common OpenXR and SteamVR launch entries`() {
        assertTrue(XrLaunchPreferences.isVrLaunchInfo(info("Play in OpenXR", "game.exe")))
        assertTrue(XrLaunchPreferences.isVrLaunchInfo(info("SteamVR mode", "game_vr.exe")))
        assertTrue(XrLaunchPreferences.isVrLaunchInfo(info("Launch with HMD", "game.exe")))
    }

    @Test
    fun `flat qualifiers take precedence over incidental vr text`() {
        assertFalse(XrLaunchPreferences.isVrLaunchInfo(info("Non-VR desktop mode", "game.exe")))
        assertFalse(XrLaunchPreferences.isVrLaunchInfo(info("Flat screen", "game_vr_launcher.exe")))
    }

    @Test
    fun `recognizes games with a Windows VR launch option`() {
        val launchInfos = listOf(
            info("Play on desktop", "game.exe"),
            info("Play in SteamVR", "game_vr.exe"),
        )

        assertTrue(XrLaunchPreferences.hasWindowsVrLaunchOption(launchInfos))
    }

    @Test
    fun `does not classify flat or non-Windows launch options as Windows VR games`() {
        assertFalse(
            XrLaunchPreferences.hasWindowsVrLaunchOption(
                listOf(
                    info("Non-VR desktop mode", "game.exe"),
                    info("OpenXR", "game.sh"),
                ),
            ),
        )
        assertFalse(XrLaunchPreferences.hasWindowsVrLaunchOption(emptyList()))
    }

    @Test
    fun `display name includes the selected executable`() {
        val launchInfo = info("OpenXR", "bin\\win64\\game.exe")
        assertEquals("OpenXR - game.exe", XrLaunchPreferences.displayName(launchInfo, 1))
    }

    @Test
    fun `Steam launch arguments are preserved without command placeholders`() {
        assertEquals(
            "-vr -openxr",
            XrLaunchPreferences.steamLaunchArguments(
                info("OpenXR", "game.exe", "  %command% -vr -openxr  "),
            ),
        )
    }

    @Test
    fun `VR launch removes desktop fpfc flags for every game`() {
        assertEquals(
            "--verbose \"two words\"",
            XrLaunchPreferences.sanitizeVrLaunchArguments("-fpfc --verbose \"two words\""),
        )
        assertEquals(
            "",
            XrLaunchPreferences.sanitizeVrLaunchArguments("FPFC"),
        )
    }

    @Test
    fun `VR launch removes common no-VR flags case-insensitively`() {
        assertEquals(
            "--verbose",
            XrLaunchPreferences.sanitizeVrLaunchArguments("--No-VR --verbose /NOHMD"),
        )
    }

    @Test
    fun `VR launch removes paired and inline disabled mode options`() {
        assertEquals(
            "--quality high",
            XrLaunchPreferences.sanitizeVrLaunchArguments(
                "-vrmode none --xr-mode=off --quality high",
            ),
        )
        assertEquals(
            "",
            XrLaunchPreferences.sanitizeVrLaunchArguments(
                "/vrmode:desktop --xrmode disabled",
            ),
        )
    }

    @Test
    fun `VR launch preserves enabling modes and unrelated quoted arguments`() {
        assertEquals(
            "-vrmode openxr --name \"flat world\" --count 0",
            XrLaunchPreferences.sanitizeVrLaunchArguments(
                "-vrmode openxr --name \"flat world\" --count 0",
            ),
        )
    }

    private fun info(description: String, executable: String, arguments: String = "") = LaunchInfo(
        executable = executable,
        workingDir = "",
        description = description,
        type = "",
        configOS = EnumSet.of(OS.windows),
        configArch = OSArch.Unknown,
        arguments = arguments,
    )
}
