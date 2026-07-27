package app.gamenative.xr

import android.app.Activity
import android.app.ActivityOptions
import android.content.Context
import android.content.ContextWrapper
import android.content.Intent
import android.hardware.display.DisplayManager
import android.view.Display
import app.gamenative.data.LaunchInfo

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

        val intent = Intent(activity, QuestVrActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                Intent.FLAG_ACTIVITY_CLEAR_TASK or
                Intent.FLAG_ACTIVITY_CLEAR_TOP or
                Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(EXTRA_APP_ID, appId)
            putExtra(EXTRA_BOOT_TO_CONTAINER, bootToContainer)
            putExtra(EXTRA_TEST_GRAPHICS, testGraphics)
            putExtra(EXTRA_IS_OFFLINE, isOffline)
            putExtra(EXTRA_RESOLVED_EXECUTABLE, resolvedExecutable)
            launchInfo?.let {
                putExtra(EXTRA_LAUNCH_EXECUTABLE, it.executable)
                putExtra(EXTRA_LAUNCH_WORKING_DIR, it.workingDir)
                putExtra(EXTRA_LAUNCH_DESCRIPTION, it.description)
                putExtra(EXTRA_LAUNCH_TYPE, it.type)
            }
        }
        val options = ActivityOptions.makeBasic().setLaunchDisplayId(displayId)

        val launchContext = if (activity is ContextWrapper) activity.baseContext else activity
        activity.finish()
        launchContext.startActivity(intent, options.toBundle())
    }

    private fun getMainDisplayId(context: Context): Int {
        val displayManager = context.getSystemService(DisplayManager::class.java) ?: return -1
        return displayManager.displays
            .firstOrNull { it.displayId == Display.DEFAULT_DISPLAY }
            ?.displayId
            ?: -1
    }
}
