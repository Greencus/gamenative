package app.gamenative.xr

import timber.log.Timber
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * Control-plane server for the Windows OpenXR bridge DLL (protocol v2).
 *
 * Floats on the wire are fixed-point micro-units (value * 1e6 as a signed decimal
 * integer) because the CRT-less PE DLL only parses integers.
 *
 * State flows in from the native Quest XR loop via [updateFrameState], [updateInput]
 * and [updateViewConfig] (called by QuestVrActivity), and out to the Windows game as
 * responses to WAIT_FRAME / LOCATE_VIEWS / GET_INPUT. HAPTIC commands flow the other
 * way through [onHaptic].
 */
class XrBridgeServer {
    private val running = AtomicBoolean(false)
    private val predictedDisplayTime = AtomicLong(1L)
    private val predictedDisplayPeriod = AtomicLong(11111111L)
    @Volatile private var shouldRender = true
    @Volatile private var sessionState = 0

    // Per eye: qx qy qz qw px py pz fovLeft fovRight fovUp fovDown (11 floats).
    private val trackingLock = Object()
    private val tracking: FloatArray = defaultTracking()
    @Volatile private var viewWidth = 1440
    @Volatile private var viewHeight = 1584
    @Volatile private var stageWidth = 0f
    @Volatile private var stageHeight = 0f

    // Per hand: active flag, buttons mask and 18 floats
    // (trigger squeeze sx sy | grip quat+pos | aim quat+pos).
    private val handActive = arrayOf(AtomicBoolean(false), AtomicBoolean(false))
    private val handButtons = arrayOf(AtomicInteger(0), AtomicInteger(0))
    private val inputStateLock = Object()
    private val handData = arrayOf(FloatArray(18), FloatArray(18))

    /** Invoked from a bridge client thread when the game requests controller haptics. */
    @Volatile var onHaptic: ((hand: Int, amplitude: Float, durationNs: Long, frequency: Float) -> Unit)? = null
    /** Invoked when the Windows application calls xrRequestExitSession. */
    @Volatile var onRequestExit: (() -> Unit)? = null
    /** Low-volume milestones used by the persistent Quest launch diagnostics. */
    @Volatile var onDiagnosticEvent: ((String) -> Unit)? = null

    // Diagnostics (see docs/xr: adb logcat -s XrBridgeServer, or the STATUS command).
    @Volatile private var clientConnected = false
    @Volatile private var lastCommand = ""
    @Volatile private var lastError = ""
    @Volatile private var gfxApi = "none"
    @Volatile private var swapchainRequests = 0
    private val activeSwapchains = AtomicInteger(0)
    private val submittedFrames = AtomicLong(0L)
    private val lastStatusPublishNs = AtomicLong(0L)
    private val framePacingLock = Object()
    private val framePacingAnnounced = AtomicBoolean(false)
    @Volatile private var framePacingDivisor = 1
    private var frameSequence = 0L
    private var lastDeliveredFrameSequence = 0L

    private var serverSocket: ServerSocket? = null
    private var acceptThread: Thread? = null

    fun statusLine(): String =
        "connected=${if (clientConnected) 1 else 0} state=$sessionState gfx=$gfxApi " +
            "swapchains=${activeSwapchains.get()}/$swapchainRequests " +
            "frames=${submittedFrames.get()} command=${lastCommand.ifEmpty { "none" }} " +
            "pacing=${framePacingDivisor}:1 " +
            "error=${lastError.ifEmpty { "none" }}"

    fun setFramePacingDivisor(value: Int) {
        framePacingDivisor = value.coerceIn(1, 2)
    }

    private fun publishStatus(force: Boolean = false) {
        val now = System.nanoTime()
        if (!force) {
            val previous = lastStatusPublishNs.get()
            if (now - previous < STATUS_PUBLISH_INTERVAL_NS ||
                !lastStatusPublishNs.compareAndSet(previous, now)
            ) {
                return
            }
        } else {
            lastStatusPublishNs.set(now)
        }
        activeStatus = statusLine()
    }

    fun start() {
        if (!running.compareAndSet(false, true)) return
        publishStatus(force = true)
        acceptThread = thread(name = "GameNativeVR-Bridge", isDaemon = true) {
            runServer()
        }
    }

    fun stop() {
        running.set(false)
        synchronized(framePacingLock) {
            framePacingLock.notifyAll()
        }
        runCatching { serverSocket?.close() }
        serverSocket = null
        runCatching { acceptThread?.join(750) }
        acceptThread = null
        publishStatus(force = true)
    }

    private fun runServer() {
        try {
            ServerSocket(PORT, 4, InetAddress.getByName(HOST)).use { server ->
                serverSocket = server
                Timber.i("GameNativeVR bridge server listening on $HOST:$PORT")
                while (running.get()) {
                    val socket = try {
                        server.accept()
                    } catch (e: SocketException) {
                        if (running.get()) Timber.w(e, "GameNativeVR bridge accept failed")
                        break
                    }
                    thread(name = "GameNativeVR-Bridge-Client", isDaemon = true) {
                        handleClient(socket)
                    }
                }
            }
        } catch (e: Exception) {
            if (running.get()) {
                lastError = "server: ${e.message}"
                Timber.e(e, "GameNativeVR bridge server failed")
            }
        } finally {
            serverSocket = null
            running.set(false)
        }
    }

    private fun handleClient(socket: Socket) {
        socket.use { client ->
            client.tcpNoDelay = true
            val reader = BufferedReader(InputStreamReader(client.getInputStream(), Charsets.US_ASCII))
            val writer = BufferedWriter(OutputStreamWriter(client.getOutputStream(), Charsets.US_ASCII))
            clientConnected = true
            publishStatus(force = true)
            Timber.i("GameNativeVR bridge client connected from ${client.remoteSocketAddress}")
            onDiagnosticEvent?.invoke("OpenXR bridge connected")
            while (running.get()) {
                val line = reader.readLine() ?: break
                val response = handleCommand(line.trim())
                writer.write(response)
                writer.write('\n'.code)
                writer.flush()
            }
            clientConnected = false
            publishStatus(force = true)
            Timber.i("GameNativeVR bridge client disconnected (lastCommand=$lastCommand)")
            onDiagnosticEvent?.invoke("OpenXR bridge disconnected (lastCommand=${lastCommand.ifEmpty { "none" }})")
        }
    }

    private fun handleCommand(command: String): String {
        if (command != "WAIT_FRAME" && command != "LOCATE_VIEWS" &&
            !command.startsWith("GET_INPUT") && !command.startsWith("BEGIN_FRAME") &&
            !command.startsWith("END_FRAME")
        ) {
            lastCommand = command
        }
        val response = when {
            command == "HELLO" -> "OK GameNativeVR 2"
            command == "GET_SYSTEM" -> "OK system=1 vendor=18254 name=Meta_Quest_GameNativeVR"
            command == "GET_VIEWS" -> "OK count=2 width=$viewWidth height=$viewHeight"
            command == "GET_BOUNDS" ->
                "OK available=${if (stageWidth > 0f && stageHeight > 0f) 1 else 0} " +
                    "width=${micro(stageWidth)} height=${micro(stageHeight)}"
            command == "BEGIN_SESSION" -> {
                Timber.i("GameNativeVR game began session")
                onDiagnosticEvent?.invoke("OpenXR session began")
                "OK"
            }
            command == "END_SESSION" -> {
                Timber.i("GameNativeVR game ended session")
                "OK"
            }
            command == "REQUEST_EXIT" -> {
                Timber.i("GameNativeVR game requested session exit")
                onRequestExit?.invoke()
                "OK"
            }
            command == "WAIT_FRAME" -> waitFrameResponse()
            command == "BEGIN_FRAME" -> "OK"
            command.startsWith("END_FRAME") -> {
                if ((parseLong(command, "layers") ?: 0L) > 0L) {
                    if (submittedFrames.incrementAndGet() == 1L) {
                        onDiagnosticEvent?.invoke("First OpenXR frame submitted")
                    }
                }
                "OK"
            }
            command == "LOCATE_VIEWS" -> locateViewsResponse()
            command.startsWith("GET_INPUT") -> inputResponse(command)
            command.startsWith("HAPTIC") -> hapticResponse(command)
            command.startsWith("GFX_API") -> {
                gfxApi = command.removePrefix("GFX_API ").trim()
                Timber.i("GameNativeVR game graphics binding -> %s", gfxApi)
                onDiagnosticEvent?.invoke("OpenXR graphics binding: $gfxApi")
                "OK"
            }
            command.startsWith("SWAPCHAIN_CREATE") -> {
                swapchainRequests++
                activeSwapchains.incrementAndGet()
                Timber.i("GameNativeVR producer created swapchain: %s", command)
                onDiagnosticEvent?.invoke("OpenXR swapchain created")
                "OK"
            }
            command == "SWAPCHAIN_DESTROY" -> {
                activeSwapchains.updateAndGet { current -> (current - 1).coerceAtLeast(0) }
                "OK"
            }
            command == "SWAPCHAIN_RESET" -> {
                activeSwapchains.set(0)
                "OK"
            }
            command == "STATUS" ->
                "OK connected=${if (clientConnected) 1 else 0} state=$sessionState gfx=$gfxApi " +
                    "swapchains=${activeSwapchains.get()} swapchainRequests=$swapchainRequests " +
                    "frames=${submittedFrames.get()} lastCommand=${lastCommand.replace(' ', '_')} " +
                    "lastError=${lastError.replace(' ', '_').ifEmpty { "none" }}"
            command == "BYE" -> "OK"
            else -> {
                lastError = "unknown command: $command"
                Timber.w("GameNativeVR bridge unknown command: %s", command)
                "ERR unknown_command"
            }
        }
        publishStatus()
        return response
    }

    private fun locateViewsResponse(): String {
        synchronized(trackingLock) {
            val sb = StringBuilder(320)
            sb.append("OK flags=15")
            for (eye in 0 until 2) {
                val prefix = if (eye == 0) "l" else "r"
                for (i in VIEW_FIELD_NAMES.indices) {
                    sb.append(' ').append(prefix).append(VIEW_FIELD_NAMES[i]).append('=').append(micro(tracking[eye * 11 + i]))
                }
            }
            return sb.toString()
        }
    }

    /**
     * OpenXR requires xrWaitFrame to pace the application. Returning the latest timestamp
     * immediately allows the Windows game to render unbounded and build a deep DXVK queue,
     * which wastes power and turns small timing variations into visible latency spikes.
     * Wake the game on compositor-aligned intervals from the native Quest XR loop instead.
     * A divisor of two intentionally produces an even half-refresh cadence while the Quest
     * compositor continues to timewarp the most recent submitted projection layer.
     */
    private fun waitFrameResponse(): String {
        val response = synchronized(framePacingLock) {
            while (
                running.get() &&
                frameSequence - lastDeliveredFrameSequence < framePacingDivisor
            ) {
                try {
                    framePacingLock.wait(250L)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    break
                }
            }
            if (frameSequence > lastDeliveredFrameSequence) {
                lastDeliveredFrameSequence = frameSequence
            }
            val applicationPeriod = predictedDisplayPeriod.get() * framePacingDivisor
            "OK time=${predictedDisplayTime.get()} period=$applicationPeriod " +
                "render=${if (shouldRender) 1 else 0} state=$sessionState"
        }
        if (framePacingAnnounced.compareAndSet(false, true)) {
            onDiagnosticEvent?.invoke(
                "OpenXR game frame pacing active (${framePacingDivisor}:1)",
            )
        }
        return response
    }

    private fun inputResponse(command: String): String {
        val hand = parseInt(command, "hand") ?: return "ERR missing_hand"
        if (hand !in 0..1) return "ERR bad_hand"
        synchronized(inputStateLock) {
            val sb = StringBuilder(360)
            sb.append("OK active=").append(if (handActive[hand].get()) 1 else 0)
            sb.append(" buttons=").append(handButtons[hand].get())
            for (i in INPUT_FIELD_NAMES.indices) {
                sb.append(' ').append(INPUT_FIELD_NAMES[i]).append('=').append(micro(handData[hand][i]))
            }
            return sb.toString()
        }
    }

    private fun hapticResponse(command: String): String {
        val hand = parseInt(command, "hand") ?: 0
        val amplitude = (parseLong(command, "amp") ?: 0L) / 1_000_000f
        val durationNs = parseLong(command, "dur") ?: 0L
        val frequency = (parseLong(command, "freq") ?: 0L) / 1_000_000f
        onHaptic?.invoke(hand, amplitude, durationNs, frequency)
        return "OK"
    }

    fun updateFrameState(
        predictedTime: Long,
        predictedPeriod: Long,
        render: Boolean,
        state: Int,
        trackingData: FloatArray,
    ) {
        predictedDisplayTime.set(predictedTime)
        predictedDisplayPeriod.set(predictedPeriod)
        shouldRender = render
        sessionState = state
        if (trackingData.size >= 22) {
            synchronized(trackingLock) {
                trackingData.copyInto(
                    destination = tracking,
                    endIndex = minOf(trackingData.size, tracking.size),
                )
                if (trackingData.size >= 24) {
                    stageWidth = trackingData[22]
                    stageHeight = trackingData[23]
                }
            }
        }
        synchronized(framePacingLock) {
            frameSequence++
            framePacingLock.notifyAll()
        }
    }

    fun updateInput(hand: Int, buttons: Int, active: Boolean, data: FloatArray) {
        if (hand !in 0..1) return
        synchronized(inputStateLock) {
            handActive[hand].set(active)
            handButtons[hand].set(buttons)
            if (data.size >= 18) {
                data.copyInto(handData[hand], endIndex = 18)
            }
        }
    }

    fun updateViewConfig(width: Int, height: Int) {
        if (width > 0 && height > 0) {
            viewWidth = width
            viewHeight = height
        }
    }

    private fun micro(value: Float): Long = (value * 1_000_000f).toLong()

    private fun parseInt(command: String, key: String): Int? = parseLong(command, key)?.toInt()

    private fun parseLong(command: String, key: String): Long? =
        command.splitToSequence(' ')
            .firstOrNull { it.startsWith("$key=") }
            ?.substringAfter('=')
            ?.toLongOrNull()

    companion object {
        const val HOST = "127.0.0.1"
        const val PORT = 38476
        private const val STATUS_PUBLISH_INTERVAL_NS = 250_000_000L
        private val VIEW_FIELD_NAMES = arrayOf(
            "qx", "qy", "qz", "qw", "px", "py", "pz", "fl", "fr", "fu", "fd",
        )
        private val INPUT_FIELD_NAMES = arrayOf(
            "tr", "sq", "sx", "sy",
            "gqx", "gqy", "gqz", "gqw", "gpx", "gpy", "gpz",
            "aqx", "aqy", "aqz", "aqw", "apx", "apy", "apz",
        )
        @Volatile var activeStatus: String = "GameNativeVR bridge inactive"
            private set

        private fun defaultTracking(): FloatArray {
            val t = FloatArray(24)
            for (eye in 0 until 2) {
                val base = eye * 11
                t[base + 3] = 1f // qw
                t[base + 4] = if (eye == 0) -0.032f else 0.032f // px
                t[base + 7] = -0.75f // fovLeft
                t[base + 8] = 0.75f
                t[base + 9] = 0.75f
                t[base + 10] = -0.75f
            }
            return t
        }
    }
}
