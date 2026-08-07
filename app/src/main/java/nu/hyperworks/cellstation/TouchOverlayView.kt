package nu.hyperworks.cellstation

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * On-screen controller for devices without physical buttons (and for anyone who
 * wants one). Feeds the same [PadState] the physical pad uses, so the core sees
 * a single controller either way.
 *
 * Multi-touch is tracked per pointer id: each pointer owns whichever control it
 * went down on, so holding a direction while tapping a face button works.
 */
class TouchOverlayView(context: Context, private val pad: PadState) : View(context) {

    private class Control(val index: Int, val label: String, val round: Boolean = true) {
        val bounds = RectF()
        var pressed = false
    }

    private val controls = listOf(
        // D-pad
        Control(PadState.Btn.UP, "▲", round = false),
        Control(PadState.Btn.DOWN, "▼", round = false),
        Control(PadState.Btn.LEFT, "◀", round = false),
        Control(PadState.Btn.RIGHT, "▶", round = false),
        // Face buttons
        Control(PadState.Btn.TRIANGLE, "△"),
        Control(PadState.Btn.CIRCLE, "○"),
        Control(PadState.Btn.CROSS, "✕"),
        Control(PadState.Btn.SQUARE, "□"),
        // Shoulders
        Control(PadState.Btn.L1, "L1", round = false),
        Control(PadState.Btn.R1, "R1", round = false),
        Control(PadState.Btn.L2, "L2", round = false),
        Control(PadState.Btn.R2, "R2", round = false),
        // Center
        Control(PadState.Btn.SELECT, "SEL", round = false),
        Control(PadState.Btn.START, "START", round = false)
    )

    /** pointer id -> control it is holding */
    private val activePointers = HashMap<Int, Control>()

    // User-configurable look (Settings → Controls): opacity scales every
    // alpha, size scales the layout unit.
    private val opacity = Settings.overlayOpacity(context) / 100f
    private val sizeScale = Settings.overlayScale(context) / 100f

    private fun alpha(base: Int): Int = (base * opacity).toInt().coerceIn(0, 255)

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
        color = Color.argb(alpha(140), 255, 255, 255)
    }
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(alpha(200), 255, 255, 255)
        textAlign = Paint.Align.CENTER
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutControls(w.toFloat(), h.toFloat())
    }

    private fun byIndex(index: Int) = controls.first { it.index == index }

    private fun layoutControls(w: Float, h: Float) {
        // Scale with the smaller dimension so the layout survives rotation and
        // the Thor's two very different panel shapes.
        val unit = min(w, h) * 0.11f * sizeScale
        val margin = unit * 0.8f

        // D-pad cross, bottom left
        val dpadCx = margin + unit * 1.6f
        val dpadCy = h - margin - unit * 1.6f
        byIndex(PadState.Btn.UP).bounds.set(dpadCx - unit / 2, dpadCy - unit * 1.5f, dpadCx + unit / 2, dpadCy - unit / 2)
        byIndex(PadState.Btn.DOWN).bounds.set(dpadCx - unit / 2, dpadCy + unit / 2, dpadCx + unit / 2, dpadCy + unit * 1.5f)
        byIndex(PadState.Btn.LEFT).bounds.set(dpadCx - unit * 1.5f, dpadCy - unit / 2, dpadCx - unit / 2, dpadCy + unit / 2)
        byIndex(PadState.Btn.RIGHT).bounds.set(dpadCx + unit / 2, dpadCy - unit / 2, dpadCx + unit * 1.5f, dpadCy + unit / 2)

        // Face buttons diamond, bottom right
        val faceCx = w - margin - unit * 1.6f
        val faceCy = h - margin - unit * 1.6f
        byIndex(PadState.Btn.TRIANGLE).bounds.set(faceCx - unit / 2, faceCy - unit * 1.6f, faceCx + unit / 2, faceCy - unit * 0.6f)
        byIndex(PadState.Btn.CROSS).bounds.set(faceCx - unit / 2, faceCy + unit * 0.6f, faceCx + unit / 2, faceCy + unit * 1.6f)
        byIndex(PadState.Btn.SQUARE).bounds.set(faceCx - unit * 1.6f, faceCy - unit / 2, faceCx - unit * 0.6f, faceCy + unit / 2)
        byIndex(PadState.Btn.CIRCLE).bounds.set(faceCx + unit * 0.6f, faceCy - unit / 2, faceCx + unit * 1.6f, faceCy + unit / 2)

        // Shoulders, top corners
        val shoulderW = unit * 1.6f
        val shoulderH = unit * 0.7f
        byIndex(PadState.Btn.L1).bounds.set(margin, margin, margin + shoulderW, margin + shoulderH)
        byIndex(PadState.Btn.L2).bounds.set(margin, margin + shoulderH * 1.2f, margin + shoulderW, margin + shoulderH * 2.2f)
        byIndex(PadState.Btn.R1).bounds.set(w - margin - shoulderW, margin, w - margin, margin + shoulderH)
        byIndex(PadState.Btn.R2).bounds.set(w - margin - shoulderW, margin + shoulderH * 1.2f, w - margin, margin + shoulderH * 2.2f)

        // Select / Start, bottom center
        val cw = unit * 1.3f
        val ch = unit * 0.6f
        byIndex(PadState.Btn.SELECT).bounds.set(w / 2 - cw - unit * 0.2f, h - margin - ch, w / 2 - unit * 0.2f, h - margin)
        byIndex(PadState.Btn.START).bounds.set(w / 2 + unit * 0.2f, h - margin - ch, w / 2 + cw + unit * 0.2f, h - margin)

        text.textSize = unit * 0.42f
    }

    override fun onDraw(canvas: Canvas) {
        for (c in controls) {
            fill.color = if (c.pressed) Color.argb(alpha(150), 255, 255, 255)
                         else Color.argb(alpha(60), 0, 0, 0)
            if (c.round) {
                val r = min(c.bounds.width(), c.bounds.height()) / 2f
                canvas.drawCircle(c.bounds.centerX(), c.bounds.centerY(), r, fill)
                canvas.drawCircle(c.bounds.centerX(), c.bounds.centerY(), r, stroke)
            } else {
                val r = c.bounds.height() * 0.2f
                canvas.drawRoundRect(c.bounds, r, r, fill)
                canvas.drawRoundRect(c.bounds, r, r, stroke)
            }
            // Vertically centre the glyph on its box.
            val baseline = c.bounds.centerY() - (text.descent() + text.ascent()) / 2f
            canvas.drawText(c.label, c.bounds.centerX(), baseline, text)
        }
    }

    private fun controlAt(x: Float, y: Float): Control? {
        controls.firstOrNull { it.bounds.contains(x, y) }?.let { return it }
        // Fingers rarely land dead centre; accept a near miss on round buttons.
        return controls.filter { it.round }.minByOrNull {
            hypot(x - it.bounds.centerX(), y - it.bounds.centerY())
        }?.takeIf {
            hypot(x - it.bounds.centerX(), y - it.bounds.centerY()) < it.bounds.width() * 0.9f
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                press(event.getPointerId(i), controlAt(event.getX(i), event.getY(i)))
            }

            MotionEvent.ACTION_MOVE -> {
                // A finger can slide from one button onto another.
                for (i in 0 until event.pointerCount) {
                    val id = event.getPointerId(i)
                    val now = controlAt(event.getX(i), event.getY(i))
                    if (activePointers[id] !== now) press(id, now)
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                if (event.actionMasked == MotionEvent.ACTION_CANCEL) {
                    activePointers.keys.toList().forEach { press(it, null) }
                } else {
                    press(event.getPointerId(event.actionIndex), null)
                }
            }
        }
        return true
    }

    private fun press(pointerId: Int, control: Control?) {
        activePointers[pointerId]?.let { previous ->
            if (activePointers.none { it.key != pointerId && it.value === previous }) {
                previous.pressed = false
                pad.setVirtual(previous.index, false)
            }
        }

        if (control == null) {
            activePointers.remove(pointerId)
        } else {
            activePointers[pointerId] = control
            control.pressed = true
            pad.setVirtual(control.index, true)
        }
        invalidate()
    }

    /** Releases everything, e.g. when the overlay is hidden mid-press. */
    /**
     * Releases only the controls this overlay is actually holding down.
     *
     * Deliberately not [PadState.releaseAll]: this is called when a physical
     * pad retires the overlay mid-session, and by then the key press that
     * triggered the retirement has already been written to the same shared
     * pad state. Zeroing everything would swallow it, so the first press on a
     * real controller would do nothing but dismiss the touch controls.
     */
    fun releaseAll() {
        activePointers.clear()
        for (c in controls) {
            if (!c.pressed) continue
            c.pressed = false
            pad.setVirtual(c.index, false)
        }
        invalidate()
    }
}
