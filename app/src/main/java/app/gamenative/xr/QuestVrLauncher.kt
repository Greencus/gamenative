package app.gamenative.xr

import android.app.Activity
import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.hardware.display.DisplayManager
import android.view.Display
import app.gamenative.PluviaApp
import app.gamenative.data.LaunchInfo
import app.gamenative.utils.ContainerUtils

object QuestVrLauncher {
    const val EXTRA_APP_ID = "app.gamenative.xr.APP_ID"
    const val EXTRA_BOOT_TO_CONTAINER = "app.gamenative.xr.BOOT_TO_CONTAINER"
    const val EXTRA_TEST_GRAPHICS = "app.gamenative.xr.TEST_GRAPHICS"
    const val EXTRA_IS_OFFLINE = "app.gamenative.xr.IS_OFFLINE"
    const val EXTRA_RESOLVED_EXECUTABLE = "app.gamenative.xr.RESOLVED_EXECUTABLE"
    const val EXTRA_LAUNCH_EXECUTABLE = "app.gamenative.xr.LAUNCH_EXECUTABLE"
    const val EXTRA_LAUNCH_WORKING_DIR = "app.gamenative.xr.LAUNCH_WORKING_DIR"
    const val EXTRA_LAUNCH_DESCRIPTION = "app.gamenative.xr.LAUNCH_DESCRIPTION"
    const val EXTRA_LAUNCH_TYPE = "app.gamenative.xr.LAUNCH_TYPE"
    const val EXTRA_LAUNCH_ARGUMENTS = "app.gamenative.xr.LAUNCH_ARGUMENTS"
    const val EXTRA_RENDER_SCALE = "app.gamenative.xr.RENDER_SCALE"
    const val EXTRA_FRAME_PACING_DIVISOR = "app.gamenative.xr.FRAME_PACING_DIVISOR"
    const val EXTRA_OPENCOMPOSITE = "app.gamenative.xr.OPENCOMPOSITE"
    const val EXTRA_THEATER_SCREEN = "app.gamenative.xr.THEATER_SCREEN"
    const val EXTRA_CLOCK = "app.gamenative.xr.CLOCK"

    fun launch(
        activity: Activity,
        appId: String,
        bootToContainer: Boolean,
        testGraphics: Boolean,
        isOffline: Boolean,
        resolvedExecutable: String = "",
        launchInfo: LaunchInfo? = null,
    ) {
        val displayId = getMainDisplayId(activity)
        require(displayId >= 0) { "Could not find the primary display for GameNativeVR launch" }
        XrLaunchDiagnostics.begin(
            context = activity,
            appId = appId,
            executable = resolvedExecutable,
            arguments = launchInfo?.arguments.orEmpty(),
        )

        val xrSettings = runCatching {
            val container = ContainerUtils.getContainer(activity, appId)
            XrContainerSettings.read(activity, appId, container)
        }.getOrDefault(XrContainerSettings.Values())

        val intent = Intent(activity, QuestVrActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_CLEAR_TOP or
                Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(EXTRA_APP_ID, appId)
            putExtra(EXTRA_BOOT_TO_CONTAINER, bootToContainer)
            putExtra(EXTRA_TEST_GRAPHICS, testGraphics)
            putExtra(EXTRA_IS_OFFLINE, isOffline)
            putExtra(EXTRA_RESOLVED_EXECUTABLE, resolvedExecutable)
            // Carry the exact durable snapshot into the XR activity. This avoids a second,
            // potentially racing container read during the activity hand-off.
            putExtra(EXTRA_RENDER_SCALE, xrSettings.renderScale)
            putExtra(EXTRA_FRAME_PACING_DIVISOR, xrSettings.framePacingDivisor)
            putExtra(EXTRA_OPENCOMPOSITE, xrSettings.openCompositeEnabled)
            putExtra(EXTRA_THEATER_SCREEN, xrSettings.theaterScreenEnabled)
            putExtra(EXTRA_CLOCK, xrSettings.clockEnabled)
            launchInfo?.let {
                putExtra(EXTRA_LAUNCH_EXECUTABLE, it.executable)
                putExtra(EXTRA_LAUNCH_WORKING_DIR, it.workingDir)
                putExtra(EXTRA_LAUNCH_DESCRIPTION, it.description)
                putExtra(EXTRA_LAUNCH_TYPE, it.type)
                putExtra(EXTRA_LAUNCH_ARGUMENTS, it.arguments)
            }
        }
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)

        // Mark the handoff before MainActivity can enter its stopped/destroyed lifecycle. The
        // library activity may be reclaimed under VR memory pressure, but it must not tear down
        // the Wine environment owned by QuestVrActivity.
        PluviaApp.isVrSessionActive = true
        try {
            activity.startActivity(intent, options.toBundle())
        } catch (error: Exception) {
            PluviaApp.isVrSessionActive = false
            XrLaunchDiagnostics.record(activity, "Could not start VR activity: ${error.message}", error)
            throw error
        }
    }

    private fun getMainDisplayId(context: Context): Int {
        val displayManager = context.getSystemService(DisplayManager::class.java) ?: return -1
        return displayManager.displays
            .firstOrNull { it.displayId == Display.DEFAULT_DISPLAY }
            ?.displayId
            ?: -1
    }
}
