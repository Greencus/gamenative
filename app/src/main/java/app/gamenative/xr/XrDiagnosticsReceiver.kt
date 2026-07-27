package app.gamenative.xr

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * ADB-readable status hook for an active GameNativeVR process.
 *
 * Usage:
 *   adb shell am broadcast -a app.gamenative.xr.STATUS
 */
class XrDiagnosticsReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val status = XrBridgeServer.activeStatus
        Log.i("GameNativeVR", "STATUS $status")
        resultCode = 0
        resultData = status
    }
}
