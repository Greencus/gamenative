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
    fun `display name includes the selected executable`() {
        val launchInfo = info("OpenXR", "bin\\win64\\game.exe")
        assertEquals("OpenXR - game.exe", XrLaunchPreferences.displayName(launchInfo, 1))
    }

    private fun info(description: String, executable: String) = LaunchInfo(
        executable = executable,
        workingDir = "",
        description = description,
        type = "",
        configOS = EnumSet.of(OS.windows),
        configArch = OSArch.Unknown,
    )
}
