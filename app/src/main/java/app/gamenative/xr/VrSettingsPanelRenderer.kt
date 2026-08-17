package app.gamenative.xr

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.Surface
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Draws the Quest runtime settings panel into its own SurfaceTexture.
 *
 * This deliberately does not use or capture the activity's flat Compose hierarchy. The worker
 * exists only to publish panel frames and remains idle while the OpenXR panel is hidden.
 */
internal class VrSettingsPanelRenderer(
    initialValues: XrContainerSettings.Values,
    private val onValuesChanged: (XrContainerSettings.Values) -> Unit,
    private val onCloseRequested: () -> Unit,
) {
    private val worker = HandlerThread("GameNativeVR-SettingsPanel").apply { start() }
    private val handler = Handler(worker.looper)
    private val mainHandler = Handler(Looper.getMainLooper())
    private val frameQueued = AtomicBoolean(false)

    @Volatile private var values = initialValues
    @Volatile private var surface: Surface? = null
    @Volatile private var width = 0
    @Volatile private var height = 0
    @Volatile private var pointerX = 0.5f
    @Volatile private var pointerY = 0.5f
    @Volatile private var pointerActive = false
    @Volatile private var hoveredControl = VrSettingsPanelLayout.Control.NONE
    @Volatile private var visible = false

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create("sans-serif", Typeface.NORMAL)
    }
    private val boldTextPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create("sans-serif", Typeface.BOLD)
    }

    fun attachSurface(surface: Surface, width: Int, height: Int) {
        this.surface = surface
        this.width = width
        this.height = height
        visible = true
        requestFrame()
    }

    fun detachSurface() {
        visible = false
        surface = null
        hoveredControl = VrSettingsPanelLayout.Control.NONE
        pointerActive = false
    }

    fun setVisible(visible: Boolean) {
        this.visible = visible
        if (visible) requestFrame()
    }

    fun updateValues(newValues: XrContainerSettings.Values) {
        values = newValues
        requestFrame()
    }

    fun updatePointer(x: Float, y: Float, active: Boolean, select: Boolean) {
        val safeX = x.coerceIn(0f, 1f)
        val safeY = y.coerceIn(0f, 1f)
        val nextHovered = if (active) {
            VrSettingsPanelLayout.hitTest(safeX, safeY)
        } else {
            VrSettingsPanelLayout.Control.NONE
        }
        val moved = kotlin.math.abs(pointerX - safeX) >= POINTER_REDRAW_DISTANCE ||
            kotlin.math.abs(pointerY - safeY) >= POINTER_REDRAW_DISTANCE
        val changed = pointerActive != active || hoveredControl != nextHovered
        pointerX = safeX
        pointerY = safeY
        pointerActive = active
        hoveredControl = nextHovered
        if (moved || changed) requestFrame()
        if (select && active && nextHovered != VrSettingsPanelLayout.Control.NONE) {
            activate(nextHovered)
        }
    }

    fun close() {
        detachSurface()
        handler.removeCallbacksAndMessages(null)
        worker.quitSafely()
    }

    private fun activate(control: VrSettingsPanelLayout.Control) {
        val current = values
        val updated = when (control) {
            VrSettingsPanelLayout.Control.RENDER_SCALE_DOWN -> current.copy(
                renderScale = adjacentRenderScale(current.renderScale, -1),
            )
            VrSettingsPanelLayout.Control.RENDER_SCALE_UP -> current.copy(
                renderScale = adjacentRenderScale(current.renderScale, 1),
            )
            VrSettingsPanelLayout.Control.PACING_NATIVE -> current.copy(framePacingDivisor = 1)
            VrSettingsPanelLayout.Control.PACING_HALF -> current.copy(framePacingDivisor = 2)
            VrSettingsPanelLayout.Control.OPENCOMPOSITE -> current.copy(
                openCompositeEnabled = !current.openCompositeEnabled,
            )
            VrSettingsPanelLayout.Control.THEATER -> current.copy(
                theaterScreenEnabled = !current.theaterScreenEnabled,
            )
            VrSettingsPanelLayout.Control.CLOCK -> current.copy(clockEnabled = !current.clockEnabled)
            VrSettingsPanelLayout.Control.CLOSE -> {
                mainHandler.post(onCloseRequested)
                return
            }
            VrSettingsPanelLayout.Control.NONE -> return
        }
        values = updated
        mainHandler.post { onValuesChanged(updated) }
        requestFrame()
    }

    private fun adjacentRenderScale(current: Int, delta: Int): Int {
        val options = XrContainerSettings.renderScaleOptions
        val index = options.indexOf(current).takeIf { it >= 0 } ?: options.indexOf(
            XrContainerSettings.DEFAULT_RENDER_SCALE,
        )
        return options[(index + delta).coerceIn(0, options.lastIndex)]
    }

    private fun requestFrame() {
        if (!visible || surface == null || !frameQueued.compareAndSet(false, true)) return
        handler.post {
            try {
                drawFrame()
            } finally {
                frameQueued.set(false)
            }
        }
    }

    private fun drawFrame() {
        val target = surface ?: return
        if (!visible || !target.isValid || width <= 0 || height <= 0) return
        var canvas: Canvas? = null
        try {
            canvas = target.lockHardwareCanvas()
            canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR)
            drawPanel(canvas)
        } catch (_: Exception) {
            // The native XR thread can tear down the Surface while a queued draw finishes.
        } finally {
            if (canvas != null) runCatching { target.unlockCanvasAndPost(canvas) }
        }
    }

    private fun drawPanel(canvas: Canvas) {
        val sx = width / VrSettingsPanelLayout.DESIGN_WIDTH
        val sy = height / VrSettingsPanelLayout.DESIGN_HEIGHT
        canvas.save()
        canvas.scale(sx, sy)

        fillPaint.color = Color.argb(248, 14, 18, 27)
        canvas.drawRoundRect(RectF(8f, 8f, 1016f, 1272f), 42f, 42f, fillPaint)
        strokePaint.color = Color.rgb(71, 91, 118)
        canvas.drawRoundRect(RectF(10f, 10f, 1014f, 1270f), 40f, 40f, strokePaint)

        drawText(canvas, "GameNativeVR", 70f, 92f, 44f, Color.WHITE, bold = true)
        drawText(canvas, "Runtime settings", 70f, 142f, 27f, Color.rgb(154, 181, 218))
        drawText(canvas, "Point with the right controller and pull trigger", 70f, 179f, 21f, Color.rgb(126, 145, 171))

        val snapshot = values
        drawStepperRow(
            canvas,
            top = 220f,
            title = "Render resolution",
            value = "${snapshot.renderScale}%",
            left = VrSettingsPanelLayout.Control.RENDER_SCALE_DOWN,
            right = VrSettingsPanelLayout.Control.RENDER_SCALE_UP,
            note = "Applied on next launch",
        )
        drawPacingRow(canvas, snapshot.framePacingDivisor)
        drawToggleRow(
            canvas,
            top = 590f,
            title = "OpenComposite",
            note = "Applied on next launch",
            checked = snapshot.openCompositeEnabled,
            control = VrSettingsPanelLayout.Control.OPENCOMPOSITE,
        )
        drawToggleRow(
            canvas,
            top = 760f,
            title = "Theater fallback",
            note = "Live",
            checked = snapshot.theaterScreenEnabled,
            control = VrSettingsPanelLayout.Control.THEATER,
        )
        drawToggleRow(
            canvas,
            top = 930f,
            title = "Left-hand clock",
            note = "Live",
            checked = snapshot.clockEnabled,
            control = VrSettingsPanelLayout.Control.CLOCK,
        )
        drawButton(
            canvas,
            RectF(70f, 1120f, 954f, 1210f),
            "Close menu",
            VrSettingsPanelLayout.Control.CLOSE,
            accent = false,
        )

        if (pointerActive) {
            fillPaint.color = Color.WHITE
            canvas.drawCircle(
                pointerX * VrSettingsPanelLayout.DESIGN_WIDTH,
                pointerY * VrSettingsPanelLayout.DESIGN_HEIGHT,
                13f,
                fillPaint,
            )
            strokePaint.color = Color.rgb(38, 188, 255)
            strokePaint.strokeWidth = 5f
            canvas.drawCircle(
                pointerX * VrSettingsPanelLayout.DESIGN_WIDTH,
                pointerY * VrSettingsPanelLayout.DESIGN_HEIGHT,
                19f,
                strokePaint,
            )
            strokePaint.strokeWidth = 3f
        }
        canvas.restore()
    }

    private fun drawStepperRow(
        canvas: Canvas,
        top: Float,
        title: String,
        value: String,
        left: VrSettingsPanelLayout.Control,
        right: VrSettingsPanelLayout.Control,
        note: String,
    ) {
        drawCard(canvas, top, top + 160f)
        drawText(canvas, title, 100f, top + 52f, 29f, Color.WHITE, bold = true)
        drawText(canvas, note, 100f, top + 91f, 20f, Color.rgb(244, 176, 82))
        drawButton(canvas, RectF(620f, top + 30f, 710f, top + 125f), "−", left)
        drawText(canvas, value, 766f, top + 91f, 31f, Color.WHITE, bold = true, centered = true)
        drawButton(canvas, RectF(860f, top + 30f, 950f, top + 125f), "+", right)
    }

    private fun drawPacingRow(canvas: Canvas, divisor: Int) {
        val top = 405f
        drawCard(canvas, top, top + 160f)
        drawText(canvas, "Frame pacing", 100f, top + 52f, 29f, Color.WHITE, bold = true)
        drawText(canvas, "Live", 100f, top + 91f, 20f, Color.rgb(80, 220, 159))
        drawButton(
            canvas,
            RectF(560f, top + 30f, 745f, top + 125f),
            "1:1",
            VrSettingsPanelLayout.Control.PACING_NATIVE,
            accent = divisor == 1,
        )
        drawButton(
            canvas,
            RectF(765f, top + 30f, 950f, top + 125f),
            "2:1",
            VrSettingsPanelLayout.Control.PACING_HALF,
            accent = divisor == 2,
        )
    }

    private fun drawToggleRow(
        canvas: Canvas,
        top: Float,
        title: String,
        note: String,
        checked: Boolean,
        control: VrSettingsPanelLayout.Control,
    ) {
        drawCard(canvas, top, top + 145f, control)
        drawText(canvas, title, 100f, top + 57f, 29f, Color.WHITE, bold = true)
        drawText(
            canvas,
            note,
            100f,
            top + 96f,
            20f,
            if (note == "Live") Color.rgb(80, 220, 159) else Color.rgb(244, 176, 82),
        )
        val switchRect = RectF(790f, top + 42f, 940f, top + 103f)
        fillPaint.color = if (checked) Color.rgb(34, 163, 228) else Color.rgb(57, 67, 84)
        canvas.drawRoundRect(switchRect, 31f, 31f, fillPaint)
        fillPaint.color = Color.WHITE
        val knobX = if (checked) switchRect.right - 31f else switchRect.left + 31f
        canvas.drawCircle(knobX, switchRect.centerY(), 24f, fillPaint)
    }

    private fun drawCard(
        canvas: Canvas,
        top: Float,
        bottom: Float,
        control: VrSettingsPanelLayout.Control = VrSettingsPanelLayout.Control.NONE,
    ) {
        val hovered = hoveredControl == control && control != VrSettingsPanelLayout.Control.NONE
        fillPaint.color = if (hovered) Color.rgb(34, 58, 79) else Color.rgb(25, 31, 43)
        canvas.drawRoundRect(RectF(70f, top, 954f, bottom), 28f, 28f, fillPaint)
        strokePaint.color = if (hovered) Color.rgb(38, 188, 255) else Color.rgb(48, 59, 76)
        canvas.drawRoundRect(RectF(71f, top + 1f, 953f, bottom - 1f), 27f, 27f, strokePaint)
    }

    private fun drawButton(
        canvas: Canvas,
        bounds: RectF,
        label: String,
        control: VrSettingsPanelLayout.Control,
        accent: Boolean = true,
    ) {
        val hovered = hoveredControl == control
        fillPaint.color = when {
            hovered -> Color.rgb(38, 188, 255)
            accent -> Color.rgb(32, 115, 165)
            else -> Color.rgb(42, 50, 65)
        }
        canvas.drawRoundRect(bounds, 22f, 22f, fillPaint)
        strokePaint.color = if (hovered) Color.WHITE else Color.rgb(78, 96, 120)
        canvas.drawRoundRect(bounds, 22f, 22f, strokePaint)
        drawText(
            canvas,
            label,
            bounds.centerX(),
            bounds.centerY() + 11f,
            28f,
            Color.WHITE,
            bold = true,
            centered = true,
        )
    }

    private fun drawText(
        canvas: Canvas,
        text: String,
        x: Float,
        baseline: Float,
        size: Float,
        color: Int,
        bold: Boolean = false,
        centered: Boolean = false,
    ) {
        val paint = if (bold) boldTextPaint else textPaint
        paint.textSize = size
        paint.color = color
        paint.textAlign = if (centered) Paint.Align.CENTER else Paint.Align.LEFT
        canvas.drawText(text, x, baseline, paint)
    }

    companion object {
        private const val POINTER_REDRAW_DISTANCE = 0.006f
    }
}

internal object VrSettingsPanelLayout {
    const val DESIGN_WIDTH = 1024f
    const val DESIGN_HEIGHT = 1280f

    enum class Control {
        NONE,
        RENDER_SCALE_DOWN,
        RENDER_SCALE_UP,
        PACING_NATIVE,
        PACING_HALF,
        OPENCOMPOSITE,
        THEATER,
        CLOCK,
        CLOSE,
    }

    fun hitTest(x: Float, y: Float): Control {
        val px = x * DESIGN_WIDTH
        val py = y * DESIGN_HEIGHT
        return when {
            px in 620f..710f && py in 250f..345f -> Control.RENDER_SCALE_DOWN
            px in 860f..950f && py in 250f..345f -> Control.RENDER_SCALE_UP
            px in 560f..745f && py in 435f..530f -> Control.PACING_NATIVE
            px in 765f..950f && py in 435f..530f -> Control.PACING_HALF
            px in 70f..954f && py in 590f..735f -> Control.OPENCOMPOSITE
            px in 70f..954f && py in 760f..905f -> Control.THEATER
            px in 70f..954f && py in 930f..1075f -> Control.CLOCK
            px in 70f..954f && py in 1120f..1210f -> Control.CLOSE
            else -> Control.NONE
        }
    }
}
