package app.gamenative.xr

import android.content.Context
import app.gamenative.data.LaunchInfo
import app.gamenative.utils.ContainerUtils
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
        context: Context,
        appId: String,
        mode: String,
        steamLaunchIndex: Int = -1,
        customArgs: String,
        promptEveryLaunch: Boolean,
    ) {
        // Never save launch preferences through a Container retained by Compose. A different
        // Container instance may have persisted regular settings since that object was loaded;
        // Container.saveData() serializes the complete snapshot and would restore every stale
        // field. Reload immediately before changing the launch extras instead.
        val container = ContainerUtils.getContainer(context, appId)
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

    fun hasWindowsVrLaunchOption(launchInfos: Iterable<LaunchInfo>): Boolean =
        launchInfos.any { info ->
            info.executable.endsWith(".exe", ignoreCase = true) && isVrLaunchInfo(info)
        }

    fun shouldLaunchInVr(container: Container, launchInfo: LaunchInfo?): Boolean {
        return when (mode(container)) {
            MODE_VR -> true
            MODE_FLAT -> false
            else -> launchInfo?.let(::isVrLaunchInfo) ?: false
        }
    }

    fun steamLaunchArguments(launchInfo: LaunchInfo?): String =
        launchInfo?.arguments
            ?.replace("%command%", "", ignoreCase = true)
            ?.trim()
            .orEmpty()

    /**
     * Remove arguments that explicitly request desktop/non-VR operation when
     * the user selected a VR launch. Launch metadata, container arguments, and
     * custom arguments all pass through this game-agnostic policy.
     */
    fun sanitizeVrLaunchArguments(arguments: String): String {
        val tokens = tokenizeCommandLine(arguments)
        val sanitized = mutableListOf<String>()
        var index = 0
        while (index < tokens.size) {
            val argument = normalizeArgument(tokens[index])
            if (argument in VR_DISABLED_ARGUMENTS) {
                index++
                continue
            }

            if (argument in VR_MODE_ARGUMENTS && index + 1 < tokens.size) {
                val value = normalizeArgument(tokens[index + 1])
                if (value in VR_DISABLED_VALUES) {
                    index += 2
                    continue
                }
            }

            val inlineMode = splitInlineOption(argument)
            if (
                inlineMode != null &&
                inlineMode.first in VR_MODE_ARGUMENTS &&
                inlineMode.second in VR_DISABLED_VALUES
            ) {
                index++
                continue
            }

            sanitized += tokens[index]
            index++
        }
        return sanitized.joinToString(" ")
    }

    private fun normalizeArgument(argument: String): String =
        argument.trim().trim('"', '\'').lowercase()

    private fun splitInlineOption(argument: String): Pair<String, String>? {
        val separatorIndex = argument.indexOfFirst { it == '=' || it == ':' }
        if (separatorIndex <= 0 || separatorIndex == argument.lastIndex) return null
        return argument.substring(0, separatorIndex) to argument.substring(separatorIndex + 1)
    }

    private fun tokenizeCommandLine(commandLine: String): List<String> {
        val tokens = mutableListOf<String>()
        val token = StringBuilder()
        var quote: Char? = null
        var escaped = false
        commandLine.forEach { character ->
            when {
                escaped -> {
                    token.append(character)
                    escaped = false
                }
                character == '\\' && quote != null -> {
                    token.append(character)
                    escaped = true
                }
                quote != null && character == quote -> {
                    token.append(character)
                    quote = null
                }
                quote == null && (character == '"' || character == '\'') -> {
                    token.append(character)
                    quote = character
                }
                quote == null && character.isWhitespace() -> {
                    if (token.isNotEmpty()) {
                        tokens += token.toString()
                        token.clear()
                    }
                }
                else -> token.append(character)
            }
        }
        if (token.isNotEmpty()) tokens += token.toString()
        return tokens
    }

    private val VR_DISABLED_ARGUMENTS = setOf(
        "fpfc",
        "-fpfc",
        "--fpfc",
        "novr",
        "-novr",
        "--novr",
        "/novr",
        "no-vr",
        "-no-vr",
        "--no-vr",
        "/no-vr",
        "nohmd",
        "-nohmd",
        "--nohmd",
        "/nohmd",
        "no-hmd",
        "-no-hmd",
        "--no-hmd",
        "/no-hmd",
    )

    private val VR_MODE_ARGUMENTS = setOf(
        "-vrmode",
        "--vrmode",
        "/vrmode",
        "-vr-mode",
        "--vr-mode",
        "/vr-mode",
        "-xrmode",
        "--xrmode",
        "/xrmode",
        "-xr-mode",
        "--xr-mode",
        "/xr-mode",
    )

    private val VR_DISABLED_VALUES = setOf(
        "none",
        "off",
        "disabled",
        "false",
        "0",
        "desktop",
        "flat",
    )

    fun displayName(info: LaunchInfo, index: Int): String {
        val label = info.description.ifBlank { info.type.ifBlank { "Launch option ${index + 1}" } }
        val exe = info.executable.substringAfterLast('/').substringAfterLast('\\')
        return if (exe.isBlank()) label else "$label - $exe"
    }
}
