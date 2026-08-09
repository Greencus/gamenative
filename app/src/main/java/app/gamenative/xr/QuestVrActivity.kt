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
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.setValue
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import app.gamenative.PluviaApp
import app.gamenative.PrefManager
import app.gamenative.CrashHandler
import app.gamenative.data.LaunchInfo
import app.gamenative.enums.OS
import app.gamenative.enums.OSArch
import app.gamenative.events.AndroidEvent
import app.gamenative.service.SteamService
import app.gamenative.ui.screen.xserver.XServerScreen
import app.gamenative.ui.theme.PluviaTheme
import app.gamenative.utils.ContainerUtils
import app.gamenative.utils.LocaleHelper
import com.winlator.core.AppUtils
import com.winlator.core.ProcessHelper
import com.winlator.inputcontrols.ControllerManager
import timber.log.Timber
import java.util.EnumSet
import kotlinx.coroutines.delay

class QuestVrActivity : ComponentActivity() {
    @Volatile private var nativeXrHandle: Long = 0
    private var xrSurfaceTexture: SurfaceTexture? = null
    private var xrRenderSurface: Surface? = null
    private val bridgeServer = XrBridgeServer()
    @Volatile private var pendingBridgeMilestone: String? = null
    @Volatile private var launchFailurePending: Boolean = false
    private var xrContainerSettings = XrContainerSettings.Values()

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
            XrLaunchDiagnostics.record(this, "Launch rejected: missing app id")
            PluviaApp.isVrSessionActive = false
            finish()
            return
        }

        val bootToContainer = intent.getBooleanExtra(QuestVrLauncher.EXTRA_BOOT_TO_CONTAINER, false)
        val testGraphics = intent.getBooleanExtra(QuestVrLauncher.EXTRA_TEST_GRAPHICS, false)
        val isOffline = intent.getBooleanExtra(QuestVrLauncher.EXTRA_IS_OFFLINE, false)
        val resolvedExecutable = intent.getStringExtra(QuestVrLauncher.EXTRA_RESOLVED_EXECUTABLE).orEmpty()
        val launchInfoOverride = intent.toLaunchInfoOverride()?.let { launchInfo ->
            if (resolvedExecutable.isBlank()) launchInfo else launchInfo.copy(executable = resolvedExecutable)
        }
        Timber.i(
            "GameNativeVR session executable=${resolvedExecutable.ifBlank { "<container default>" }} " +
                "arguments=${launchInfoOverride?.arguments.orEmpty()}",
        )
        Timber.i("Starting GameNativeVR activity for appId=$appId")
        XrLaunchDiagnostics.record(
            this,
            "VR activity created; executable=${resolvedExecutable.ifBlank { "<container default>" }} " +
                "arguments=${launchInfoOverride?.arguments.orEmpty().ifBlank { "<none>" }}",
        )
        xrContainerSettings = runCatching {
            val container = ContainerUtils.getContainer(applicationContext, appId)
            val persisted = XrContainerSettings.read(applicationContext, appId, container)
            persisted.copy(
                renderScale = if (intent.hasExtra(QuestVrLauncher.EXTRA_RENDER_SCALE)) {
                    XrContainerSettings.sanitizeRenderScale(
                        intent.getIntExtra(QuestVrLauncher.EXTRA_RENDER_SCALE, persisted.renderScale),
                    )
                } else {
                    persisted.renderScale
                },
                openCompositeEnabled = if (intent.hasExtra(QuestVrLauncher.EXTRA_OPENCOMPOSITE)) {
                    intent.getBooleanExtra(
                        QuestVrLauncher.EXTRA_OPENCOMPOSITE,
                        persisted.openCompositeEnabled,
                    )
                } else {
                    persisted.openCompositeEnabled
                },
                theaterScreenEnabled = if (intent.hasExtra(QuestVrLauncher.EXTRA_THEATER_SCREEN)) {
                    intent.getBooleanExtra(
                        QuestVrLauncher.EXTRA_THEATER_SCREEN,
                        persisted.theaterScreenEnabled,
                    )
                } else {
                    persisted.theaterScreenEnabled
                },
                clockEnabled = if (intent.hasExtra(QuestVrLauncher.EXTRA_CLOCK)) {
                    intent.getBooleanExtra(QuestVrLauncher.EXTRA_CLOCK, persisted.clockEnabled)
                } else {
                    persisted.clockEnabled
                },
            )
        }.onFailure { error ->
            Timber.w(error, "Could not read per-container VR settings; using defaults")
        }.getOrDefault(XrContainerSettings.Values())
        val settingsSummary =
            "renderScale=${xrContainerSettings.renderScale}% " +
                "openComposite=${xrContainerSettings.openCompositeEnabled} " +
                "theater=${xrContainerSettings.theaterScreenEnabled} " +
                "clock=${xrContainerSettings.clockEnabled}"
        Timber.i("GameNativeVR container settings: $settingsSummary")
        XrLaunchDiagnostics.record(this, "VR container settings: $settingsSummary")
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
        bridgeServer.onDiagnosticEvent = { event ->
            pendingBridgeMilestone = event
            XrLaunchDiagnostics.record(this, event)
        }
        bridgeServer.start()
        var startupError: String? = null
        nativeXrHandle = runCatching {
            nativeStart(
                renderScale = xrContainerSettings.renderScale,
                theaterScreenEnabled = xrContainerSettings.theaterScreenEnabled,
                clockEnabled = xrContainerSettings.clockEnabled,
            )
        }
            .onFailure { error ->
                Timber.e(error, "GameNativeVR native startup failed")
                XrLaunchDiagnostics.record(this, "Native XR startup failed: ${error.message}", error)
            }
            .getOrElse { error ->
                startupError = "Native XR startup failed: ${error.message ?: error.javaClass.simpleName}"
                0L
            }
        if (nativeXrHandle == 0L && startupError == null) {
            startupError = "Native XR startup returned no session handle"
            XrLaunchDiagnostics.record(this, startupError!!)
        }
        Timber.i("GameNativeVR native handle=$nativeXrHandle")
        XrLaunchDiagnostics.record(this, "Native XR handle=$nativeXrHandle")

        setContent {
            var launchError by rememberSaveable { mutableStateOf(startupError) }
            var launchStage by rememberSaveable { mutableStateOf("Starting Wine") }
            var stallWarning by rememberSaveable { mutableStateOf<String?>(null) }
            LaunchedEffect(Unit) {
                while (true) {
                    pendingBridgeMilestone?.let { milestone ->
                        pendingBridgeMilestone = null
                        launchStage = milestone
                    }
                    delay(250)
                }
            }
            LaunchedEffect(launchStage, launchError) {
                stallWarning = null
                if (launchError == null &&
                    !launchStage.startsWith("Windows process exited") &&
                    launchStage != "Stereo projection active: game images displayed"
                ) {
                    val watchedStage = launchStage
                    delay(120_000)
                    if (launchError == null && launchStage == watchedStage) {
                        val message = "No progress for 2 minutes at: $watchedStage (${bridgeServer.statusLine()})"
                        stallWarning = message
                        XrLaunchDiagnostics.record(this@QuestVrActivity, message)
                        captureGuestDiagnostics(appId)
                    }
                }
            }
            PluviaTheme {
                QuestVrTheaterScreen(
                    statusProvider = bridgeServer::statusLine,
                    launchStage = launchStage,
                    stallWarning = stallWarning,
                    launchError = launchError,
                    logPath = XrLaunchDiagnostics.path(this),
                    onReturn = { finish() },
                ) {
                    if (launchError == null) {
                        XServerScreen(
                            appId = appId,
                            bootToContainer = bootToContainer,
                            testGraphics = testGraphics,
                            isOffline = isOffline,
                            launchInfoOverride = launchInfoOverride,
                            registerBackAction = {},
                            navigateBack = {
                                XrLaunchDiagnostics.record(this, "VR screen requested navigation back")
                                if (!launchFailurePending) finish()
                            },
                            onExit = { onComplete ->
                                XrLaunchDiagnostics.record(this, "Game process exited")
                                onComplete?.invoke()
                                if (!launchFailurePending) runOnUiThread { finish() }
                            },
                            onGameLaunchError = { error ->
                                val message = error.ifBlank { "Unknown Wine launch error" }
                                launchFailurePending = true
                                Timber.e("GameNativeVR game launch error: $message")
                                XrLaunchDiagnostics.record(this, "Wine launch failed: $message")
                                captureGuestDiagnostics(appId)
                                runOnUiThread { launchError = message }
                            },
                            onGameLaunchStage = { stage ->
                                XrLaunchDiagnostics.record(this, stage)
                                runOnUiThread { launchStage = stage }
                            },
                        )
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        PluviaApp.isActivityInForeground = true
        SteamService.autoStopWhenIdle = false
        if (PluviaApp.xEnvironment != null && !PluviaApp.isNeverSuspendMode() && !PluviaApp.isOverlayPaused) {
            PluviaApp.xEnvironment?.onResume()
        }
        XrLaunchDiagnostics.record(this, "VR activity resumed")
    }

    override fun onPause() {
        XrLaunchDiagnostics.record(this, "VR activity paused")
        PluviaApp.isActivityInForeground = false
        if (PluviaApp.xEnvironment != null && !PluviaApp.isNeverSuspendMode()) {
            PluviaApp.xEnvironment?.onPause()
            if (PluviaApp.isManualSuspendMode()) PluviaApp.isOverlayPaused = true
        }
        super.onPause()
    }

    override fun onDestroy() {
        XrLaunchDiagnostics.record(this, "VR activity destroyed; changingConfiguration=$isChangingConfigurations")
        bridgeServer.onHaptic = null
        bridgeServer.onRequestExit = null
        bridgeServer.onDiagnosticEvent = null
        if (nativeXrHandle != 0L) {
            nativeStop(nativeXrHandle)
            nativeXrHandle = 0
        }
        bridgeServer.stop()
        QuestVrSurfaceRegistry.clearSurface()
        releaseXrRenderSurface()
        if (!isChangingConfigurations) {
            PluviaApp.events.emit(AndroidEvent.ActivityDestroyed)
            PluviaApp.isVrSessionActive = false
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
    private fun onNativeXrOverlayReady() {
        Timber.i("GameNativeVR activity overlay ready")
        XrLaunchDiagnostics.record(
            this,
            "XR activity overlay ready: left-hand clock",
        )
    }

    @Suppress("unused")
    private fun onNativeXrDiagnostic(message: String) {
        pendingBridgeMilestone = message
        XrLaunchDiagnostics.record(this, message)
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

    private fun captureGuestDiagnostics(appId: String) {
        Thread({
            XrLaunchDiagnostics.record(
                this,
                "Android app log tail:\n${CrashHandler.getAppLogs(800)}",
            )
            runCatching { ContainerUtils.getContainer(applicationContext, appId) }
                .onSuccess { container ->
                    XrLaunchDiagnostics.record(
                        this,
                        XrPayloadManager.diagnosticSummary(applicationContext, container, appId),
                    )
                    XrLaunchDiagnostics.recordFileTail(
                        this,
                        "Windows OpenXR bridge log",
                        java.io.File(container.rootDir, ".wine/drive_c/gamenative/xr/bridge.log"),
                    )
                    XrLaunchDiagnostics.recordFileTail(
                        this,
                        "Wine OpenXR Unix bridge log",
                        java.io.File(container.rootDir, ".wine/drive_c/gamenative/xr/unix.log"),
                    )
                    XrLaunchDiagnostics.recordFileTail(
                        this,
                        "ColdClientLoader configuration",
                        java.io.File(
                            container.rootDir,
                            ".wine/drive_c/Program Files (x86)/Steam/ColdClientLoader.ini",
                        ),
                    )
                    val openCompositeLogs = java.io.File(container.rootDir, ".wine/drive_c/users")
                        .listFiles()
                        .orEmpty()
                        .map { userDir ->
                            java.io.File(
                                userDir,
                                "AppData/Local/OpenComposite/logs/opencomposite.log",
                            )
                        }
                        .filter(java.io.File::isFile)
                    val openCompositeLog = openCompositeLogs.maxByOrNull(java.io.File::lastModified)
                    XrLaunchDiagnostics.recordFileTail(
                        this,
                        "OpenComposite log",
                        openCompositeLog ?: java.io.File(
                            container.rootDir,
                            ".wine/drive_c/users/<wine-user>/AppData/Local/OpenComposite/logs/opencomposite.log",
                            ),
                    )
                    val unityPlayerLog = java.io.File(container.rootDir, ".wine/drive_c/users")
                        .walkTopDown()
                        .filter {
                            it.isFile &&
                                it.name.equals("Player.log", ignoreCase = true) &&
                                it.path.contains("Beat Saber", ignoreCase = true)
                        }
                        .maxByOrNull(java.io.File::lastModified)
                    XrLaunchDiagnostics.recordFileTail(
                        this,
                        "Beat Saber Unity Player log",
                        unityPlayerLog ?: java.io.File(
                            container.rootDir,
                            ".wine/drive_c/users/<wine-user>/AppData/LocalLow/Hyperbolic Magnetism/Beat Saber/Player.log",
                        ),
                    )
                }
                .onFailure { error ->
                    XrLaunchDiagnostics.record(this, "Could not resolve container diagnostics: ${error.message}")
                }
            val processes = runCatching {
                ProcessHelper.listSubProcesses()
                    .sortedBy { it.pid }
                    .joinToString(separator = "\n") { process ->
                        val commandLine = runCatching {
                            java.io.File("/proc/${process.pid}/cmdline")
                                .readBytes()
                                .toString(Charsets.UTF_8)
                                .replace('\u0000', ' ')
                                .trim()
                        }.getOrDefault("<unavailable>")
                        val state = runCatching {
                            java.io.File("/proc/${process.pid}/status")
                                .useLines { lines ->
                                    lines.firstOrNull { it.startsWith("State:") }
                                }
                        }.getOrNull() ?: "State: <unavailable>"
                        val waitChannel = runCatching {
                            java.io.File("/proc/${process.pid}/wchan").readText().trim()
                        }.getOrDefault("<unavailable>")
                        "pid=${process.pid} ppid=${process.ppid} rss=${process.rssBytes} " +
                            "name=${process.name} $state wchan=$waitChannel cmd=$commandLine"
                    }
            }.getOrElse { error -> "<unavailable: ${error.message}>" }
            XrLaunchDiagnostics.record(
                this,
                "GameNativeVR process snapshot:\n${processes.ifBlank { "<none>" }}",
            )
            XrLaunchDiagnostics.recordFileTail(
                this,
                "Wine debug log",
                java.io.File(getExternalFilesDir(null), "wine_logs/wine_debug.log"),
            )
        }, "GameNativeVR-Diagnostics").start()
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

    private external fun nativeStart(
        renderScale: Int,
        theaterScreenEnabled: Boolean,
        clockEnabled: Boolean,
    ): Long
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
        arguments = getStringExtra(QuestVrLauncher.EXTRA_LAUNCH_ARGUMENTS).orEmpty(),
    )
}

@Composable
private fun QuestVrTheaterScreen(
    statusProvider: () -> String,
    launchStage: String,
    stallWarning: String?,
    launchError: String?,
    logPath: String,
    onReturn: () -> Unit,
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
            text = "GameNativeVR  stage=$launchStage  $diagnostics",
            color = Color(0xFFD5F4FF),
            fontSize = 11.sp,
            modifier = Modifier
                .align(Alignment.TopStart)
                .background(Color(0xB0000000))
                .padding(horizontal = 8.dp, vertical = 4.dp),
        )
        if (stallWarning != null && launchError == null) {
            Text(
                text = stallWarning,
                color = Color(0xFFFFD166),
                fontSize = 12.sp,
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .background(Color(0xDD1A1A1A))
                    .padding(horizontal = 12.dp, vertical = 8.dp),
            )
        }
        if (launchError != null) {
            Column(
                modifier = Modifier
                    .align(Alignment.Center)
                    .widthIn(max = 720.dp)
                    .background(Color(0xEE14181F))
                    .border(1.dp, Color(0xFFFF6B6B))
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = "GameNativeVR could not start the game",
                    color = Color(0xFFFF8A8A),
                    fontSize = 20.sp,
                )
                Text(
                    text = launchError,
                    color = Color.White,
                    fontSize = 15.sp,
                    modifier = Modifier.padding(top = 12.dp),
                )
                Text(
                    text = "Diagnostic log: $logPath",
                    color = Color(0xFFD5F4FF),
                    fontSize = 11.sp,
                    modifier = Modifier.padding(top = 12.dp, bottom = 18.dp),
                )
                Button(onClick = onReturn) {
                    Text("Return to library")
                }
            }
        }
    }
}
