package nu.hyperworks.cellstation

import android.os.Bundle
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewGroup
import android.widget.FrameLayout
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import nu.hyperworks.cellstation.Ui.dp
import kotlin.concurrent.thread

/**
 * Boots straight into a game. Public intent contract (docs/INTENTS.md):
 *   action  nu.hyperworks.cellstation.EMULATE
 *   extra   bootPath = filesystem path (content:// support comes later)
 */
class EmulationActivity : AppCompatActivity(), SurfaceHolder.Callback {

    companion object {
        const val ACTION_EMULATE = "nu.hyperworks.cellstation.EMULATE"
        const val EXTRA_BOOT_PATH = "bootPath"
        const val EXTRA_GAME_DIR = "gameDir"
        const val EXTRA_STRETCH = "stretch"
    }

    private var booted = false
    private val pad = PadState()
    private var overlay: TouchOverlayView? = null
    private var overlayMode = Settings.TouchOverlayMode.AUTO
    private lateinit var frame: FrameLayout
    private var quickPanel: LinearLayout? = null

    /**
     * Debug-only remote pad: `adb shell am broadcast -a nu.hyperworks.cellstation.DEBUG_PAD
     * --es keys "start,start,cross" --ei hold 250 --ei gap 3000` presses buttons through
     * [PadState.setVirtual] — the same path as the touch overlay, straight into the native
     * pad snapshot. Exists because injected evdev/keyevent input reaches the activity but
     * games ignore it until a physical press has occurred (cause still unknown; see
     * tools/drive.sh), which makes headless measurement runs impossible without it.
     */
    private var debugPadReceiver: android.content.BroadcastReceiver? = null

    private fun registerDebugPad() {
        if (applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE == 0) return
        val names = mapOf(
            "cross" to PadState.Btn.CROSS, "x" to PadState.Btn.CROSS,
            "circle" to PadState.Btn.CIRCLE, "o" to PadState.Btn.CIRCLE,
            "square" to PadState.Btn.SQUARE, "triangle" to PadState.Btn.TRIANGLE,
            "start" to PadState.Btn.START, "select" to PadState.Btn.SELECT,
            "up" to PadState.Btn.UP, "down" to PadState.Btn.DOWN,
            "left" to PadState.Btn.LEFT, "right" to PadState.Btn.RIGHT,
            "l1" to PadState.Btn.L1, "r1" to PadState.Btn.R1,
            "l2" to PadState.Btn.L2, "r2" to PadState.Btn.R2,
            "l3" to PadState.Btn.L3, "r3" to PadState.Btn.R3, "ps" to PadState.Btn.PS,
        )
        val receiver = object : android.content.BroadcastReceiver() {
            override fun onReceive(context: android.content.Context?, intent: android.content.Intent?) {
                val keys = intent?.getStringExtra("keys")?.split(',') ?: return
                val hold = intent.getIntExtra("hold", 250).toLong()
                val gap = intent.getIntExtra("gap", 3000).toLong()
                Thread {
                    for (raw in keys) {
                        val idx = names[raw.trim().lowercase()] ?: continue
                        pad.setVirtual(idx, true)
                        Thread.sleep(hold)
                        pad.setVirtual(idx, false)
                        Thread.sleep(gap)
                    }
                }.start()
            }
        }
        androidx.core.content.ContextCompat.registerReceiver(
            this, receiver, android.content.IntentFilter("nu.hyperworks.cellstation.DEBUG_PAD"),
            androidx.core.content.ContextCompat.RECEIVER_EXPORTED)
        debugPadReceiver = receiver
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        // Ask for a clock ceiling the device can actually hold. Without this the
        // governor boosts, overheats and then throttles below where it would
        // have settled — worse for a session than a slightly lower steady state.
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            runCatching { window.setSustainedPerformanceMode(true) }
        }
        WindowCompat.setDecorFitsSystemWindows(window, false)

        val surfaceView = SurfaceView(this)
        frame = FrameLayout(this)
        frame.addView(surfaceView)

        pad.keyMapping = KeyMap.load(this)
        overlayMode = Settings.touchOverlayMode(this)
        if (overlayMode != Settings.TouchOverlayMode.NEVER) {
            overlay = TouchOverlayView(this, pad).also { frame.addView(it) }
        }

        // Above the pad overlay: a cold boot compiles the whole game and can run
        // for minutes with nothing on screen, which is indistinguishable from a
        // hang. Hides itself whenever the core reports no work in progress.
        frame.addView(BootProgressView(this))

        // Host-side counters (CPU vs GPU), which the core's own overlay cannot
        // report. Off unless asked for; see Settings.
        if (Settings.hostStats(this)) {
            frame.addView(HostStatsView(this))
        }

        setContentView(frame)
        surfaceView.holder.addCallback(this)
        registerDebugPad()

        WindowInsetsControllerCompat(window, surfaceView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        // Back opens the quick panel instead of silently killing the game;
        // quitting is an explicit action inside the panel (design doc, screen E).
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (quickPanel == null) showQuickPanel() else hideQuickPanel()
            }
        })
    }

    // ---- in-game quick panel ----------------------------------------------

    private fun showQuickPanel() {
        if (quickPanel != null) return
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xF20B0F19.toInt())
            setPadding(dp(18), dp(20), dp(18), dp(16))
        }
        panel.addView(Ui.title(this, getString(R.string.quick_title), 17f).apply {
            setPadding(0, 0, 0, dp(12))
        })

        panel.addView(Ui.actionButton(this, getString(R.string.quick_resume), primary = true) {
            hideQuickPanel()
        }.apply { layoutParams = panelItem() })

        var toggleButton: TextView? = null
        toggleButton = Ui.actionButton(this, overlayToggleLabel(), primary = false) {
            val next = when (Settings.touchOverlayMode(this)) {
                Settings.TouchOverlayMode.AUTO -> Settings.TouchOverlayMode.ALWAYS
                Settings.TouchOverlayMode.ALWAYS -> Settings.TouchOverlayMode.NEVER
                Settings.TouchOverlayMode.NEVER -> Settings.TouchOverlayMode.AUTO
            }
            Settings.setTouchOverlayMode(this, next)
            applyOverlayMode(next)
            toggleButton?.text = overlayToggleLabel()
        }
        panel.addView(toggleButton.apply { layoutParams = panelItem() })

        // The performance overlay is a global core setting, but the moment you
        // want it is while a slow scene is on screen — so it gets a toggle here
        // too. Everything else about it (detail, corner, graphs) stays in
        // Settings; this only flips it on and off. The core marks the setting
        // dynamic and the bridge rebuilds the overlay after the write, so it
        // appears on the running game without a reboot.
        var perfButton: TextView? = null
        perfButton = Ui.actionButton(this, perfOverlayLabel(), primary = false) {
            if (EmuBridge.setPerfOverlayEnabled(!EmuBridge.perfOverlayEnabled())) {
                perfButton?.text = perfOverlayLabel()
            } else {
                Toast.makeText(this, R.string.game_settings_rejected, Toast.LENGTH_SHORT).show()
            }
        }
        panel.addView(perfButton.apply { layoutParams = panelItem() })

        panel.addView(android.view.View(this), LinearLayout.LayoutParams(0, 0, 1f))

        panel.addView(Ui.body(this, getString(R.string.quick_close_hint), Ui.MUTED, 11.5f).apply {
            setPadding(dp(4), 0, 0, dp(6))
        })
        panel.addView(TextView(this).apply {
            text = getString(R.string.quick_close)
            textSize = 14.5f
            gravity = Gravity.CENTER
            setTextColor(Ui.BAD)
            background = Ui.focusable(this@EmulationActivity, Ui.PANEL_RAISED, 10)
            isFocusable = true
            isClickable = true
            isLongClickable = true
            setPadding(dp(18), dp(11), dp(18), dp(11))
            setOnClickListener {
                Toast.makeText(this@EmulationActivity, R.string.quick_close_hint, Toast.LENGTH_SHORT).show()
            }
            setOnLongClickListener { finish(); true }
            layoutParams = panelItem()
        })

        val width = (resources.displayMetrics.widthPixels *
            if (Ui.isLandscape(this)) 0.32 else 0.72).toInt()
        frame.addView(panel, FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.MATCH_PARENT).apply {
            gravity = Gravity.END
        })
        quickPanel = panel
        panel.requestFocus()
    }

    private fun hideQuickPanel() {
        quickPanel?.let { frame.removeView(it) }
        quickPanel = null
    }

    private fun panelItem() = LinearLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
    ).apply { bottomMargin = dp(8) }

    private fun overlayToggleLabel(): String {
        val mode = when (Settings.touchOverlayMode(this)) {
            Settings.TouchOverlayMode.AUTO -> getString(R.string.touch_auto)
            Settings.TouchOverlayMode.ALWAYS -> getString(R.string.touch_always)
            Settings.TouchOverlayMode.NEVER -> getString(R.string.touch_never)
        }
        return getString(R.string.touch_overlay, mode)
    }

    private fun perfOverlayLabel(): String = getString(
        R.string.quick_perf_overlay,
        getString(if (EmuBridge.perfOverlayEnabled()) R.string.opt_on else R.string.opt_off)
    )

    /** Applies an overlay mode change immediately, mid-session. */
    private fun applyOverlayMode(mode: Settings.TouchOverlayMode) {
        overlayMode = mode
        when (mode) {
            Settings.TouchOverlayMode.NEVER -> {
                overlay?.let { view ->
                    view.releaseAll()
                    frame.removeView(view)
                }
                overlay = null
            }
            else -> if (overlay == null) {
                overlay = TouchOverlayView(this, pad).also {
                    // Keep the overlay under the quick panel if one is open.
                    frame.addView(it, frame.indexOfChild(quickPanel).takeIf { i -> i >= 0 } ?: frame.childCount)
                }
            }
        }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        EmuBridge.surfaceEvent(holder.surface, EmuBridge.SURFACE_READY)

        if (!booted) {
            booted = true
            // Accepts bootPath (path or URI), gameDir (folder-format), or a
            // data URI from ACTION_VIEW — see docs/INTENTS.md.
            val path = BootTarget.resolve(this, intent)
            if (path.isNullOrEmpty()) {
                Toast.makeText(this, R.string.no_boot_path, Toast.LENGTH_LONG).show()
                finish()
                return
            }
            EmulationService.start(this, java.io.File(path).nameWithoutExtension)

            val stretch = intentBoolean(EXTRA_STRETCH)
            // Read before boot: the flip path consults this on every frame, so
            // it is in effect from the first presented frame onward.
            EmuBridge.setAdpfEnabled(Settings.adpf(this))
            thread(name = "EmuBoot") {
                val result = EmuBridge.boot(path)
                if (result != 0) {
                    runOnUiThread {
                        Toast.makeText(this, getString(R.string.boot_failed, result), Toast.LENGTH_LONG).show()
                        finish()
                    }
                    return@thread
                }
                // Emu.Load() reloads config.yml, so a per-launch override can only
                // be applied once the boot returns; the renderer re-reads the
                // setting every frame, well before the game presents anything.
                if (stretch != null) {
                    EmuBridge.setStretchToDisplayArea(stretch, persist = false)
                }

            }
        }
    }

    /**
     * Reads an optional boolean extra. `am start -e <name> true` delivers a
     * string, so frontends can pass either form; absent/unparsable means
     * "leave the persisted setting alone".
     */
    @Suppress("DEPRECATION")
    private fun intentBoolean(name: String): Boolean? {
        return when (val value = intent.extras?.get(name)) {
            is Boolean -> value
            is Number -> value.toInt() != 0
            is String -> when (value.lowercase()) {
                "true", "1", "on", "yes" -> true
                "false", "0", "off", "no" -> false
                else -> null
            }
            else -> null
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        EmuBridge.surfaceEvent(holder.surface, EmuBridge.SURFACE_READY)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        EmuBridge.surfaceEvent(null, EmuBridge.SURFACE_DESTROYED)
    }

    // Controller input: Android delivers gamepad events to the focused activity,
    // so forward them to the native pad handler and consume them. Non-gamepad
    // keys (e.g. volume, back on a phone) fall through to the default handling.
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean =
        handledByPad(pad.onKey(event)) || super.onKeyDown(keyCode, event)

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean =
        handledByPad(pad.onKey(event)) || super.onKeyUp(keyCode, event)

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        handledByPad(pad.onMotion(event)) || super.onGenericMotionEvent(event)

    /**
     * In AUTO mode the on-screen controller is redundant once a real pad is in
     * use, so the first physical input retires it for this session.
     */
    private fun handledByPad(consumed: Boolean): Boolean {
        if (consumed && overlayMode == Settings.TouchOverlayMode.AUTO) {
            overlay?.let { view ->
                overlay = null
                view.releaseAll()
                (view.parent as? android.view.ViewGroup)?.removeView(view)
            }
        }
        return consumed
    }

    override fun onResume() {
        super.onResume()
        // Re-read on every resume, not just at boot: changing the face-button
        // setting (or the handheld's own controller-style switch) while a game
        // is suspended should take effect on the way back in, without making
        // the player quit and relaunch to find out whether it helped.
        pad.keyMapping = KeyMap.load(this)
        // Same reasoning for the performance hints, and it makes them testable:
        // the setting can be re-applied without ending the session, so the same
        // scene can be measured with and without them.
        EmuBridge.setAdpfEnabled(Settings.adpf(this))
        EmuBridge.setPadConnected(true)
    }

    override fun onPause() {
        // Release every button so a game doesn't see input stuck down while
        // the activity is backgrounded.
        EmuBridge.setPadConnected(false)
        super.onPause()
    }

    override fun onDestroy() {
        debugPadReceiver?.let { runCatching { unregisterReceiver(it) } }
        debugPadReceiver = null
        EmulationService.stop(this)
        EmuBridge.kill()
        super.onDestroy()
    }
}
