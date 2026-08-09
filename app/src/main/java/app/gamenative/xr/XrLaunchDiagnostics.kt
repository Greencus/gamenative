package app.gamenative.xr

import android.content.Context
import android.os.Process
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Durable diagnostics for failures that are caught before Android can create a crash report. */
object XrLaunchDiagnostics {
    private const val DIRECTORY = "gamenativevr"
    private const val FILE_NAME = "launch.log"
    private const val MAX_LOG_BYTES = 512 * 1024L
    private const val DEFAULT_TAIL_CHARS = 12_000
    private val lock = Any()

    fun begin(context: Context, appId: String, executable: String, arguments: String) {
        synchronized(lock) {
            val file = logFile(context)
            trimIfNeeded(file)
            file.appendText(
                buildString {
                    appendLine()
                    appendLine("===== GameNativeVR launch ${timestamp()} =====")
                    appendLine("pid=${Process.myPid()} appId=$appId")
                    appendLine("executable=${executable.ifBlank { "<unresolved>" }}")
                    appendLine("arguments=${arguments.ifBlank { "<none>" }}")
                },
            )
        }
    }

    fun record(context: Context, message: String, error: Throwable? = null) {
        synchronized(lock) {
            val file = logFile(context)
            trimIfNeeded(file)
            file.appendText(
                buildString {
                    append(timestamp()).append(' ').appendLine(message)
                    if (error != null) appendLine(error.stackTraceToString())
                },
            )
        }
    }

    fun path(context: Context): String = logFile(context).absolutePath

    fun recordFileTail(context: Context, label: String, source: File, maxChars: Int = 8_000) {
        val tail = runCatching {
            source.takeIf(File::isFile)?.readText()?.takeLast(maxChars)
        }.getOrNull()
        record(
            context,
            if (tail.isNullOrBlank()) {
                "$label is unavailable at ${source.absolutePath}"
            } else {
                "$label tail (${source.absolutePath}):\n$tail"
            },
        )
    }

    fun readRecent(context: Context, maxChars: Int = DEFAULT_TAIL_CHARS): String =
        runCatching {
            val text = logFile(context).takeIf(File::isFile)?.readText().orEmpty()
            text.takeLast(maxChars.coerceAtLeast(0))
        }.getOrDefault("")

    private fun logFile(context: Context): File {
        val root = context.getExternalFilesDir(null) ?: context.filesDir
        return File(root, "$DIRECTORY/$FILE_NAME").apply { parentFile?.mkdirs() }
    }

    private fun trimIfNeeded(file: File) {
        if (!file.isFile || file.length() <= MAX_LOG_BYTES) return
        val tail = file.readText().takeLast((MAX_LOG_BYTES / 2).toInt())
        file.writeText("[older GameNativeVR diagnostics trimmed]\n$tail")
    }

    private fun timestamp(): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.ROOT).format(Date())
}
