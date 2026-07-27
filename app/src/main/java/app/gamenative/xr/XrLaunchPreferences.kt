package app.gamenative.xr

import app.gamenative.data.LaunchInfo
import com.winlator.container.Container

object XrLaunchPreferences {
    const val VR_MODE_EXTRA = "xrLaunchMode"
    const val STEAM_LAUNCH_INDEX_EXTRA = "xrSteamLaunchIndex"
    const val CUSTOM_ARGS_EXTRA = "xrCustomArgs"
    const val PROMPT_EVERY_LAUNCH_EXTRA = "xrPromptEveryLaunch"

    const val MODE_AUTO = "auto"
    const val MODE_FLAT = "flat"
    const val MODE_VR = "vr"

    fun mode(container: Container): String =
        container.getExtra(VR_MODE_EXTRA, MODE_AUTO).takeIf {
            it == MODE_AUTO || it == MODE_FLAT || it == MODE_VR
        } ?: MODE_AUTO

    fun selectedSteamLaunchIndex(container: Container): Int =
        container.getExtra(STEAM_LAUNCH_INDEX_EXTRA, "-1").toIntOrNull() ?: -1

    fun customArgs(container: Container): String =
        container.getExtra(CUSTOM_ARGS_EXTRA, "").trim()

    fun shouldPromptEveryLaunch(container: Container): Boolean =
        container.getExtra(PROMPT_EVERY_LAUNCH_EXTRA, "true").toBooleanStrictOrNull() != false

    fun save(
        container: Container,
        mode: String,
        steamLaunchIndex: Int = -1,
        customArgs: String = customArgs(container),
        promptEveryLaunch: Boolean = shouldPromptEveryLaunch(container),
    ) {
        container.putExtra(VR_MODE_EXTRA, mode)
        container.putExtra(STEAM_LAUNCH_INDEX_EXTRA, steamLaunchIndex)
        container.putExtra(CUSTOM_ARGS_EXTRA, customArgs.trim())
        container.putExtra(PROMPT_EVERY_LAUNCH_EXTRA, promptEveryLaunch)
        container.saveData()
    }

    fun selectedLaunchInfo(container: Container, launchInfos: List<LaunchInfo>): LaunchInfo? {
        val index = selectedSteamLaunchIndex(container)
        return launchInfos.getOrNull(index) ?: launchInfos.firstOrNull()
    }

    fun isVrLaunchInfo(info: LaunchInfo): Boolean {
        val text = "${info.description} ${info.type} ${info.executable}".lowercase()
        val vrTerms = listOf("vr", "openxr", "steamvr", "oculus", "virtual reality", "hmd")
        val flatTerms = listOf("non-vr", "non vr", "novr", "desktop", "flat")
        return vrTerms.any { text.contains(it) } && flatTerms.none { text.contains(it) }
    }

    fun shouldLaunchInVr(container: Container, launchInfo: LaunchInfo?): Boolean {
        return when (mode(container)) {
            MODE_VR -> true
            MODE_FLAT -> false
            else -> launchInfo?.let(::isVrLaunchInfo) ?: false
        }
    }

    fun displayName(info: LaunchInfo, index: Int): String {
        val label = info.description.ifBlank { info.type.ifBlank { "Launch option ${index + 1}" } }
        val exe = info.executable.substringAfterLast('/').substringAfterLast('\\')
        return if (exe.isBlank()) label else "$label - $exe"
    }
}
