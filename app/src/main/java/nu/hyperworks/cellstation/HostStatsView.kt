package nu.hyperworks.cellstation

import android.content.Context
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.widget.FrameLayout
import android.widget.TextView
import nu.hyperworks.cellstation.Ui.dp
import java.io.File

/**
 * Host-side counters, which the emulator core cannot know about.
 *
 * rpcs3's own overlay reports *emulated* load — how busy the PPU/SPU/RSX
 * threads are. That does not tell you whether the phone is CPU-bound or
 * GPU-bound, which is the question you actually have when a game runs slowly.
 * This reads the host process's CPU time and the Adreno driver's own busy
 * counter, so the two can be compared directly.
 */
class HostStatsView(context: Context) : FrameLayout(context) {

    private val text = TextView(context).apply {
        textSize = 11f
        setTextColor(0xFFB9F5D0.toInt())
        typeface = Typeface.MONOSPACE
        setBackgroundColor(0x99000000.toInt())
        setPadding(context.dp(8), context.dp(5), context.dp(8), context.dp(5))
    }

    private val handler = Handler(Looper.getMainLooper())

    // CPU time is a counter, so a rate needs the previous sample.
    private var lastCpuTicks = 0L
    private var lastCpuAtMs = 0L

    // The kgsl counter resets when read, so it is already an interval measure.
    private val gpuBusyFile = File("/sys/class/kgsl/kgsl-3d0/gpubusy")
    private val gpuClockFile = File("/sys/class/kgsl/kgsl-3d0/clock_mhz")

    private val ticksPerSec = 100L // AT_CLKTCK is 100 on every Android arm64 build

    private val poll = object : Runnable {
        override fun run() {
            update()
            handler.postDelayed(this, 500)
        }
    }

    init {
        addView(
            text,
            LayoutParams(
                LayoutParams.WRAP_CONTENT,
                LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.CENTER_HORIZONTAL
            ).apply { topMargin = context.dp(4) }
        )
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        handler.post(poll)
    }

    override fun onDetachedFromWindow() {
        handler.removeCallbacks(poll)
        super.onDetachedFromWindow()
    }

    private fun update() {
        val cpu = cpuPercent()
        val gpu = gpuPercent()
        val mhz = readLongOrNull(gpuClockFile)

        text.text = buildString {
            append("HOST  cpu ")
            append(if (cpu != null) "%5.1f%%".format(cpu) else "  n/a")
            append("   gpu ")
            append(if (gpu != null) "%5.1f%%".format(gpu) else "  n/a")
            if (mhz != null) append("  @${mhz}MHz")
        }
    }

    /**
     * Whole-process CPU as a percentage of one core. Above 100% means several
     * threads are running at once, which is the normal case here — six SPU
     * threads plus PPU and RSX.
     */
    private fun cpuPercent(): Double? {
        val stat = runCatching { File("/proc/self/stat").readText() }.getOrNull() ?: return null
        // utime and stime are fields 14 and 15, counting from after the
        // parenthesised comm — which can itself contain spaces.
        val fields = stat.substringAfterLast(") ").split(' ')
        if (fields.size < 15) return null
        val ticks = (fields[11].toLongOrNull() ?: return null) + (fields[12].toLongOrNull() ?: return null)
        val now = SystemClock.elapsedRealtime()

        val prevTicks = lastCpuTicks
        val prevAt = lastCpuAtMs
        lastCpuTicks = ticks
        lastCpuAtMs = now

        if (prevAt == 0L || now <= prevAt) return null
        val cpuMs = (ticks - prevTicks) * 1000.0 / ticksPerSec
        return cpuMs * 100.0 / (now - prevAt)
    }

    /**
     * Adreno's own busy counter: "busy total" in driver ticks, reset on read,
     * so each sample already covers the interval since the last one.
     */
    private fun gpuPercent(): Double? {
        val raw = runCatching { gpuBusyFile.readText() }.getOrNull()?.trim() ?: return null
        val parts = raw.split(Regex("\\s+"))
        if (parts.size < 2) return null
        val busy = parts[0].toDoubleOrNull() ?: return null
        val total = parts[1].toDoubleOrNull() ?: return null
        if (total <= 0.0) return null
        return busy * 100.0 / total
    }

    private fun readLongOrNull(f: File): Long? =
        runCatching { f.readText().trim().toLong() }.getOrNull()
}
