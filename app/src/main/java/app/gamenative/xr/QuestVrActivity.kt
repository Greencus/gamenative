package app.gamenative.xr

import android.app.Activity
import android.content.Context
import android.content.pm.ActivityInfo
import android.graphics.SurfaceTexture
import android.os.Build
import android.os.Bundle
import android.view.Surface
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.gamenative.PluviaApp
import app.gamenative.PrefManager
import app.gamenative.data.LaunchInfo
import app.gamenative.enums.OS
import app.gamenative.enums.OSArch
import app.gamenative.events.AndroidEvent
import app.gamenative.ui.screen.xserver.XServerScreen
import app.gamenative.ui.theme.PluviaTheme
import app.gamenative.utils.ContainerUtils
import app.gamenative.utils.LocaleHelper
import com.winlator.core.AppUtils
import com.winlator.inputcontrols.ControllerManager
import timber.log.Timber
import java.util.EnumSet
import kotlinx.coroutines.delay

class QuestVrActivity : ComponentActivity() {
    @Volatile private var nativeXrHandle: Long = 0
    private var xrSurfaceTexture: SurfaceTexture? = null
    private var xrRenderSurface: Surface? = null
    private val bridgeServer = XrBridgeServer()

    override fun attachBaseContext(newBase: Context) {
        PrefManager.init(newBase)
        super.attachBaseContext(LocaleHelper.applyLanguage(newBase, PrefManager.appLanguage))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE

        AppUtils.keepScreenOn(this)
        ControllerManager.getInstance().init(applicationContext)
        ContainerUtils.setContainerDefaults(applicationContext)
        PluviaApp.events.emit(AndroidEvent.SetSystemUIVisibility(false))

        val appId = intent.getStringExtra(QuestVrLauncher.EXTRA_APP_ID).orEmpty()
        if (appId.isBlank()) {
            Timber.e("GameNativeVR launch missing app id")
            finish()
            return
        }

        val bootToContainer = intent.getBooleanExtra(QuestVrLauncher.EXTRA_BOOT_TO_CONTAINER, false)
        val testGraphics = intent.getBooleanExtra(QuestVrLauncher.EXTRA_TEST_GRAPHICS, false)
        val isOffline = intent.getBooleanExtra(QuestVrLauncher.EXTRA_IS_OFFLINE, false)
        val resolvedExecutable = intent.getStringExtra(QuestVrLauncher.EXTRA_RESOLVED_EXECUTABLE).orEmpty()
        val launchInfoOverride = intent.toLaunchInfoOverride()
        if (resolvedExecutable.isNotBlank()) {
            runCatching {
                val container = ContainerUtils.getContainer(applicationContext, appId)
                if (container.executablePath != resolvedExecutable) {
                    container.executablePath = resolvedExecutable
                    container.saveData()
                    Timber.i("GameNativeVR launch seeded executablePath=$resolvedExecutable")
                }
            }.onFailure { error ->
                Timber.w(error, "Failed to seed GameNativeVR executablePath")
            }
        }
        Timber.i("Starting GameNativeVR activity for appId=$appId")
        bridgeServer.onHaptic = { hand, amplitude, durationNs, frequency ->
            val handle = nativeXrHandle
            if (handle != 0L) {
                nativeHaptic(handle, hand, amplitude, durationNs, frequency)
            }
        }
        bridgeServer.onRequestExit = {
            val handle = nativeXrHandle
            if (handle != 0L) nativeRequestExit(handle)
        }
        bridgeServer.start()
        nativeXrHandle = nativeStart()
        Timber.i("GameNativeVR native handle=$nativeXrHandle")

        setContent {
            PluviaTheme {
                QuestVrTheaterScreen(statusProvider = bridgeServer::statusLine) {
                    XServerScreen(
                    appId = appId,
                    bootToContainer = bootToContainer,
                    testGraphics = testGraphics,
                    isOffline = isOffline,
                    launchInfoOverride = launchInfoOverride,
                    registerBackAction = {},
                    navigateBack = { finish() },
                    onExit = { onComplete ->
                        onComplete?.invoke()
                        finish()
                    },
                    onGameLaunchError = { error ->
                Timber.e("GameNativeVR game launch error: $error")
                        finish()
                    },
                    )
                }
            }
        }
    }

    override fun onDestroy() {
        bridgeServer.onHaptic = null
        bridgeServer.onRequestExit = null
        if (nativeXrHandle != 0L) {
            nativeStop(nativeXrHandle)
            nativeXrHandle = 0
        }
        bridgeServer.stop()
        QuestVrSurfaceRegistry.clearSurface()
        releaseXrRenderSurface()
        if (!isChangingConfigurations) {
            PluviaApp.events.emit(AndroidEvent.ActivityDestroyed)
        }
        super.onDestroy()
    }

    @Suppress("unused")
    private fun onNativeXrSurfaceReady(surface: Surface, width: Int, height: Int) {
        Timber.i("GameNativeVR native surface ready: ${width}x$height")
        runOnUiThread {
            QuestVrSurfaceRegistry.setSurface(surface, width, height)
        }
    }

    @Suppress("unused")
    private fun onNativeXrSurfaceDestroyed() {
        Timber.i("GameNativeVR native surface destroyed")
        runOnUiThread {
            QuestVrSurfaceRegistry.clearSurface()
        }
    }

    @Suppress("unused")
    private fun onNativeXrFrameState(
        predictedTime: Long,
        predictedPeriod: Long,
        shouldRender: Boolean,
        sessionState: Int,
        tracking: FloatArray,
    ) {
        bridgeServer.updateFrameState(
            predictedTime = predictedTime,
            predictedPeriod = predictedPeriod,
            render = shouldRender,
            state = sessionState,
            trackingData = tracking,
        )
    }

    @Suppress("unused")
    private fun onNativeXrInput(hand: Int, buttons: Int, active: Boolean, data: FloatArray) {
        bridgeServer.updateInput(hand = hand, buttons = buttons, active = active, data = data)
    }

    @Suppress("unused")
    private fun onNativeXrViewConfig(width: Int, height: Int) {
        Timber.i("GameNativeVR recommended per-eye view size: ${width}x$height")
        bridgeServer.updateViewConfig(width, height)
    }

    @Suppress("unused")
    private fun createNativeXrSurfaceTexture(textureId: Int, width: Int, height: Int): SurfaceTexture? {
        Timber.i("Creating GameNativeVR SurfaceTexture target: texture=$textureId ${width}x$height")
        releaseXrRenderSurface()
        return runCatching {
            SurfaceTexture(textureId).also { texture ->
                texture.setDefaultBufferSize(width, height)
                val surface = Surface(texture)
                xrSurfaceTexture = texture
                xrRenderSurface = surface
                runOnUiThread {
                    QuestVrSurfaceRegistry.setSurface(surface, width, height)
                }
            }
        }.onFailure { error ->
            Timber.e(error, "Failed to create GameNativeVR SurfaceTexture")
        }.getOrNull()
    }

    private fun releaseXrRenderSurface() {
        QuestVrSurfaceRegistry.clearSurface()
        xrRenderSurface?.release()
        xrRenderSurface = null
        xrSurfaceTexture?.release()
        xrSurfaceTexture = null
    }

    companion object {
        init {
            if (Build.BRAND.equals("oculus", ignoreCase = true)) {
                try {
                    System.loadLibrary("openxr_forwardloader.oculus")
                } catch (_: UnsatisfiedLinkError) {
                    // Quest OS v62+ no longer exposes this loader as an app-loadable library.
                }
            }
            System.loadLibrary("gamenative_xr")
        }
    }

    private external fun nativeStart(): Long
    private external fun nativeStop(handle: Long)
    private external fun nativeHaptic(handle: Long, hand: Int, amplitude: Float, durationNs: Long, frequency: Float)
    private external fun nativeRequestExit(handle: Long)
}

private fun android.content.Intent.toLaunchInfoOverride(): LaunchInfo? {
    val executable = getStringExtra(QuestVrLauncher.EXTRA_LAUNCH_EXECUTABLE).orEmpty()
    if (executable.isBlank()) return null
    return LaunchInfo(
        executable = executable,
        workingDir = getStringExtra(QuestVrLauncher.EXTRA_LAUNCH_WORKING_DIR).orEmpty(),
        description = getStringExtra(QuestVrLauncher.EXTRA_LAUNCH_DESCRIPTION).orEmpty(),
        type = getStringExtra(QuestVrLauncher.EXTRA_LAUNCH_TYPE).orEmpty(),
        configOS = EnumSet.of(OS.windows),
        configArch = OSArch.Unknown,
    )
}

@Composable
private fun QuestVrTheaterScreen(
    statusProvider: () -> String,
    content: @Composable () -> Unit,
) {
    val diagnostics by produceState(initialValue = statusProvider()) {
        while (true) {
            value = statusProvider()
            delay(1_000)
        }
    }
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black),
        contentAlignment = Alignment.Center,
    ) {
        BoxWithConstraints(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 48.dp, vertical = 32.dp),
            contentAlignment = Alignment.Center,
        ) {
            val maxPanelWidth = maxWidth
            val maxPanelHeight = maxHeight
            val panelModifier = Modifier
                .widthIn(max = maxPanelWidth)
                .heightIn(max = maxPanelHeight)
                .aspectRatio(16f / 9f)
                .background(Color.Black)
                .border(1.dp, Color(0xFF2A2F38))

            Box(
                modifier = panelModifier,
                contentAlignment = Alignment.Center,
            ) {
                content()
            }
        }
        Text(
            text = "GameNativeVR  $diagnostics",
            color = Color(0xFFD5F4FF),
            fontSize = 11.sp,
            modifier = Modifier
                .align(Alignment.TopStart)
                .background(Color(0xB0000000))
                .padding(horizontal = 8.dp, vertical = 4.dp),
        )
    }
}
