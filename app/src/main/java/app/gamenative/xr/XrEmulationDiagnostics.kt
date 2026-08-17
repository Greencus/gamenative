package app.gamenative.xr

import com.winlator.container.Container
import com.winlator.xenvironment.components.GuestProgramLauncherComponent
import org.json.JSONObject

/** Formats the persisted and effective emulation choices used by a VR launch. */
object XrEmulationDiagnostics {
    private val persistedKeys = listOf(
        "graphicsDriver",
        "graphicsDriverVersion",
        "graphicsDriverConfig",
        "dxwrapper",
        "dxwrapperConfig",
        "displayRendererMode",
        "containerVariant",
        "wineVersion",
        "wow64Mode",
        "emulator",
        "fexcoreVersion",
        "fexcorePreset",
        "box64Version",
        "box64Preset",
        "box86Version",
        "box86Preset",
        "cpuList",
        "cpuListWoW64",
        "startupSelection",
        "steamType",
    )

    fun savedAndLoadedSnapshot(container: Container): String {
        val configFile = container.configFile
        val persisted = runCatching { JSONObject(configFile.readText()) }.getOrNull()
        val persistedValues = persistedKeys.joinToString(separator = " ") { key ->
            "$key=${persisted.valueForLog(key)}"
        }

        return buildString {
            appendLine("GameNativeVR emulation settings at activity start:")
            appendLine(
                "  configFile=${configFile.absolutePath} exists=${configFile.isFile} " +
                    "bytes=${if (configFile.isFile) configFile.length() else 0} " +
                    "modified=${if (configFile.isFile) configFile.lastModified() else 0}",
            )
            appendLine("  saved: $persistedValues")
            appendLine("  loaded: ${loadedValues(container)}")
        }.trimEnd()
    }

    fun effectiveLaunchSnapshot(
        container: Container,
        launcher: GuestProgramLauncherComponent,
    ): String {
        val wine = launcher.wineInfo
        val isArm64Ec = wine?.isArm64EC == true
        val win64Translator = when {
            isArm64Ec -> "Wine-ARM64EC/native"
            else -> "Box64/${launcher.box64Version}"
        }
        val win32Translator = when {
            isArm64Ec && container.emulator.equals("FEXCore", ignoreCase = true) ->
                "FEXCore/${container.fexCoreVersion} via libwow64fex.dll"
            isArm64Ec -> "WoWBox64/${container.box64Version} via wowbox64.dll"
            container.isWoW64Mode -> "Wine-WoW64 under Box64"
            else -> "Box86/${container.box86Version}"
        }

        return buildString {
            appendLine("GameNativeVR effective emulation launch parameters:")
            appendLine(
                "  launcher=${launcher.javaClass.simpleName} wine=${wine?.identifier() ?: container.wineVersion} " +
                    "arch=${wine?.arch ?: "unknown"} variant=${container.containerVariant}",
            )
            appendLine("  win64=$win64Translator")
            appendLine("  win32=$win32Translator")
            appendLine(
                "  box64Version=${launcher.box64Version} box64Preset=${launcher.box64Preset} " +
                    "box86Version=${launcher.box86Version} box86Preset=${launcher.box86Preset}",
            )
            appendLine(
                "  fexcoreVersion=${container.fexCoreVersion} fexcorePreset=${container.fexCorePreset} " +
                    "emulator=${container.emulator} wow64Mode=${launcher.isWoW64Mode}",
            )
            appendLine(
                "  cpuList=${container.cpuList} cpuListWoW64=${container.cpuListWoW64} " +
                    "startupSelection=${container.startupSelection} steamType=${launcher.steamType}",
            )
        }.trimEnd()
    }

    private fun loadedValues(container: Container): String =
        "graphicsDriver=${container.graphicsDriver} " +
            "graphicsDriverVersion=${container.graphicsDriverVersion} " +
            "graphicsDriverConfig=${container.graphicsDriverConfig.forLog()} " +
            "dxwrapper=${container.dxWrapper} " +
            "dxwrapperConfig=${container.dxWrapperConfig.forLog()} " +
            "displayRendererMode=${container.displayRenderer} " +
            "containerVariant=${container.containerVariant} " +
            "wineVersion=${container.wineVersion} " +
            "wow64Mode=${container.isWoW64Mode} " +
            "emulator=${container.emulator} " +
            "fexcoreVersion=${container.fexCoreVersion} " +
            "fexcorePreset=${container.fexCorePreset} " +
            "box64Version=${container.box64Version} " +
            "box64Preset=${container.box64Preset} " +
            "box86Version=${container.box86Version} " +
            "box86Preset=${container.box86Preset} " +
            "cpuList=${container.cpuList} " +
            "cpuListWoW64=${container.cpuListWoW64} " +
            "startupSelection=${container.startupSelection} " +
            "steamType=${container.steamType}"

    private fun String.forLog(): String = replace('\n', ' ').replace('\r', ' ')

    private fun JSONObject?.valueForLog(key: String): String {
        if (this == null) return "<unreadable>"
        if (!has(key) || isNull(key)) return "<omitted/default>"
        return opt(key)?.toString()?.replace('\n', ' ')?.replace('\r', ' ') ?: "<null>"
    }
}
