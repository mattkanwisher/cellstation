package nu.hyperworks.cellstation

import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import nu.hyperworks.cellstation.Ui.dp
import java.io.File
import kotlin.concurrent.thread

/**
 * App settings (emulator-core options still live in the on-device config.yml).
 *
 * Orientation-adaptive like the library: landscape gets the two-pane layout
 * (categories under the left thumb, controls on the right), portrait collapses
 * to one pane with horizontally scrolling category pills.
 */
class SettingsActivity : AppCompatActivity() {

    private enum class Category { QUICK, CONTROLS, FOLDERS, ABOUT }

    private lateinit var pane: LinearLayout
    private var category = Category.QUICK

    private val pickScanFolder = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri ?: return@registerForActivityResult
        val path = BootTarget.pathFromTreeUri(uri)
        if (path == null) {
            Toast.makeText(this, R.string.folder_unreadable, Toast.LENGTH_LONG).show()
        } else {
            Settings.addScanFolder(this, path)
            Toast.makeText(this, getString(R.string.folder_added, path), Toast.LENGTH_SHORT).show()
            renderPane()
        }
    }

    private val pickDriver = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        thread {
            val entry = GpuDriver.install(this, uri)
            runOnUiThread {
                if (entry == null) {
                    Toast.makeText(this, R.string.driver_invalid, Toast.LENGTH_LONG).show()
                } else {
                    Settings.setGpuDriver(this, entry.dir.name)
                    Toast.makeText(this, getString(R.string.driver_installed, entry.name), Toast.LENGTH_LONG).show()
                    renderPane()
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        savedInstanceState?.getInt(STATE_CATEGORY)?.let {
            category = Category.entries.getOrElse(it) { Category.QUICK }
        }
        setContentView(if (Ui.isLandscape(this)) buildLandscape() else buildPortrait())
        renderPane()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(STATE_CATEGORY, category.ordinal)
    }

    // ---- scaffolds ---------------------------------------------------------

    private fun buildLandscape(): LinearLayout {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Ui.BG)
        }
        val rail = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Ui.PANEL_RAISED)
            setPadding(dp(14), dp(18), dp(14), dp(14))
        }
        rail.addView(Ui.title(this, getString(R.string.settings), 19f).apply {
            setPadding(dp(12), 0, 0, dp(14))
        })
        for (cat in Category.entries) {
            rail.addView(categoryItem(cat, vertical = true))
        }
        root.addView(rail, LinearLayout.LayoutParams(dp(190), ViewGroup.LayoutParams.MATCH_PARENT))
        root.addView(Ui.divider(this, vertical = true))

        pane = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(22), dp(18), dp(22), dp(18))
        }
        root.addView(ScrollView(this).apply {
            isVerticalScrollBarEnabled = false
            addView(pane)
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        return root
    }

    private fun buildPortrait(): LinearLayout {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Ui.BG)
            setPadding(dp(16), dp(16), dp(16), 0)
        }
        root.addView(Ui.title(this, getString(R.string.settings), 21f).apply {
            setPadding(0, 0, 0, dp(12))
        })
        val pills = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        for (cat in Category.entries) {
            pills.addView(categoryItem(cat, vertical = false))
        }
        root.addView(HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = false
            addView(pills)
        })

        pane = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(14), 0, dp(18))
        }
        root.addView(ScrollView(this).apply {
            isVerticalScrollBarEnabled = false
            addView(pane)
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        return root
    }

    private fun categoryItem(cat: Category, vertical: Boolean): TextView {
        val on = cat == category
        return TextView(this).apply {
            text = getString(when (cat) {
                Category.QUICK -> R.string.cat_quick
                Category.CONTROLS -> R.string.cat_controls
                Category.FOLDERS -> R.string.section_folders
                Category.ABOUT -> R.string.section_about
            })
            textSize = 14f
            maxLines = 1
            setTextColor(if (on) Ui.INK else Ui.MUTED)
            typeface = if (on) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
            background = if (on) Ui.rounded(0x2A8B7CFA, if (vertical) 9 else 18, this@SettingsActivity)
                        else Ui.focusable(this@SettingsActivity, Color.TRANSPARENT, if (vertical) 9 else 18)
            isFocusable = true
            isClickable = true
            setPadding(dp(14), dp(9), dp(14), dp(9))
            layoutParams = if (vertical) {
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { bottomMargin = dp(4) }
            } else {
                LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { marginEnd = dp(8) }
            }
            setOnClickListener {
                if (category != cat) {
                    category = cat
                    recreate()
                }
            }
        }
    }

    // ---- panes -------------------------------------------------------------

    private fun renderPane() {
        pane.removeAllViews()
        when (category) {
            Category.QUICK -> renderQuick()
            Category.CONTROLS -> renderControls()
            Category.FOLDERS -> renderFolders()
            Category.ABOUT -> renderAbout()
        }
    }

    /** One settings row: title + plain-language subtitle, control below or beside. */
    private fun row(title: String, subtitle: String, control: android.view.View) {
        val item = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(10), 0, dp(12))
        }
        item.addView(TextView(this).apply {
            text = title
            textSize = 15f
            setTextColor(Ui.INK)
            typeface = Typeface.DEFAULT_BOLD
        })
        item.addView(Ui.body(this, subtitle, Ui.MUTED, 12.5f).apply {
            setPadding(0, dp(2), 0, dp(8))
        })
        item.addView(control)
        pane.addView(item)
        pane.addView(Ui.divider(this, vertical = false))
    }

    private fun renderQuick() {
        val drivers = GpuDriver.installed(this)
        val selected = Settings.gpuDriver(this)
        val names = listOf(getString(R.string.gpu_driver_system)) + drivers.map { it.name }
        val values = listOf("") + drivers.map { it.dir.name }
        val selectedIndex = values.indexOf(selected).coerceAtLeast(0)
        row(
            getString(R.string.row_gpu_driver),
            getString(R.string.row_gpu_driver_sub),
            Ui.segmented(this, names, selectedIndex) { i ->
                Settings.setGpuDriver(this, values[i])
                Toast.makeText(this, R.string.driver_restart_needed, Toast.LENGTH_SHORT).show()
            }
        )
        pane.addView(Ui.actionButton(this, getString(R.string.install_gpu_driver), primary = false) {
            pickDriver.launch(arrayOf("application/zip", "application/octet-stream"))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(12) }
        })
        pane.addView(Ui.divider(this, vertical = false).apply {
            (layoutParams as? LinearLayout.LayoutParams)?.topMargin = dp(12)
        })
        row(
            getString(R.string.stretch_to_display),
            getString(R.string.stretch_to_display_hint),
            Ui.segmented(
                this,
                listOf(getString(R.string.seg_off), getString(R.string.seg_on)),
                if (EmuBridge.stretchToDisplayArea()) 1 else 0
            ) { i -> EmuBridge.setStretchToDisplayArea(i == 1, persist = true) }
        )

        renderPerfOverlay()
        renderHostStats()

        row(
            getString(R.string.row_online_data),
            getString(R.string.row_online_data_sub),
            Ui.segmented(
                this,
                listOf(getString(R.string.seg_off), getString(R.string.seg_on)),
                if (Settings.onlineData(this)) 1 else 0
            ) { i -> Settings.setOnlineData(this, i == 1) }
        )
    }

    /**
     * The core's own RSX performance overlay (overlay_perf_metrics.cpp), driven
     * straight from config.yml. Everything under "Video/Performance Overlay" is
     * marked dynamic in the core, so a running game picks these up immediately —
     * the bridge re-runs reset_performance_overlay() after each write. Nothing
     * here is drawn or measured by the app.
     *
     * The rest of the controls only appear once the overlay is on, so the pane
     * stays short for the common case of "just show me the frame rate".
     */
    /**
     * Host counters are ours, not the core's, so they live next to the core's
     * overlay rather than inside it — the emulator cannot see host GPU load.
     */
    private fun renderHostStats() {
        val on = Settings.hostStats(this)
        row(
            getString(R.string.row_host_stats),
            getString(R.string.row_host_stats_sub),
            Ui.segmented(this, listOf(getString(R.string.opt_off), getString(R.string.opt_on)), if (on) 1 else 0) { i ->
                Settings.setHostStats(this, i == 1)
            }
        )
    }

    private fun renderPerfOverlay() {
        perfOverlayChoice(
            EmuBridge.PERF_ENABLED,
            R.string.row_perf_overlay, R.string.row_perf_overlay_sub,
            rerenderOnChange = true
        )

        if (!EmuBridge.perfOverlayEnabled()) return

        perfOverlayChoice(
            EmuBridge.PERF_LEVEL,
            R.string.row_perf_detail, R.string.row_perf_detail_sub
        )
        perfOverlayChoice(
            EmuBridge.PERF_POSITION,
            R.string.row_perf_position, R.string.row_perf_position_sub
        )
        perfOverlayChoice(
            EmuBridge.PERF_FRAMERATE_GRAPH,
            R.string.row_perf_fps_graph, R.string.row_perf_fps_graph_sub
        )
        perfOverlayChoice(
            EmuBridge.PERF_FRAMETIME_GRAPH,
            R.string.row_perf_frametime_graph, R.string.row_perf_frametime_graph_sub
        )
    }

    /**
     * Segmented picker over whatever values the core accepts for [path]. The
     * labels come from the core's own enum names (cfg::_enum::to_list), which is
     * exactly what cfg::_enum::from_string parses back, so a value shown here is
     * always a value the core will take.
     */
    private fun perfOverlayChoice(path: String, label: Int, hint: Int, rerenderOnChange: Boolean = false) {
        val options = EmuBridge.globalConfigOptions(path)
            .split('\n')
            .filter { it.isNotBlank() }

        // A setting rpcs3 renamed or dropped upstream: skip it rather than
        // render a control that cannot resolve to a config node.
        if (options.isEmpty()) return

        val selected = options.indexOf(EmuBridge.globalConfigGet(path)).coerceAtLeast(0)
        row(
            getString(label),
            getString(hint),
            Ui.segmented(this, options.map(::prettify), selected) { i ->
                if (!EmuBridge.globalConfigSet(path, options[i])) {
                    Toast.makeText(this, R.string.game_settings_rejected, Toast.LENGTH_LONG).show()
                } else if (rerenderOnChange) {
                    renderPane()
                }
            }
        )
    }

    /** The core spells booleans "true"/"false"; give the toggles real labels. */
    private fun prettify(value: String): String = when (value) {
        "true" -> getString(R.string.opt_on)
        "false" -> getString(R.string.opt_off)
        else -> value
    }

    private fun renderControls() {
        DeviceProfile.current()?.let { profile ->
            pane.addView(Ui.statusChip(
                this,
                getString(R.string.detected_device, profile.displayName),
                if (Settings.appliedDeviceProfile(this) == profile.id) Ui.OK else Ui.MUTED
            ).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { bottomMargin = dp(10) }
            })
        }

        val overlayModes = listOf(
            getString(R.string.seg_auto), getString(R.string.seg_always), getString(R.string.seg_off)
        )
        val overlaySelected = when (Settings.touchOverlayMode(this)) {
            Settings.TouchOverlayMode.AUTO -> 0
            Settings.TouchOverlayMode.ALWAYS -> 1
            Settings.TouchOverlayMode.NEVER -> 2
        }
        row(
            getString(R.string.row_touch_overlay),
            getString(R.string.row_touch_overlay_sub),
            Ui.segmented(this, overlayModes, overlaySelected) { i ->
                Settings.setTouchOverlayMode(this, when (i) {
                    1 -> Settings.TouchOverlayMode.ALWAYS
                    2 -> Settings.TouchOverlayMode.NEVER
                    else -> Settings.TouchOverlayMode.AUTO
                })
            }
        )

        // The Thor's built-in pad reports Nintendo-labeled key codes; positional
        // mapping puts Cross on the bottom regardless. A saved custom mapping
        // (Map controller buttons) takes precedence over this switch.
        row(
            getString(R.string.row_face_buttons),
            getString(R.string.row_face_buttons_sub),
            Ui.segmented(
                this,
                listOf(getString(R.string.face_by_label), getString(R.string.face_positional)),
                if (Settings.nintendoLayout(this)) 1 else 0
            ) { i -> Settings.setNintendoLayout(this, i == 1) }
        )

        row(
            getString(R.string.row_overlay_opacity),
            getString(R.string.row_overlay_opacity_sub),
            slider(min = 20, max = 100, value = Settings.overlayOpacity(this)) {
                Settings.setOverlayOpacity(this, it)
            }
        )
        row(
            getString(R.string.row_overlay_scale),
            getString(R.string.row_overlay_scale_sub),
            slider(min = 60, max = 150, value = Settings.overlayScale(this)) {
                Settings.setOverlayScale(this, it)
            }
        )

        pane.addView(Ui.actionButton(this, getString(R.string.map_buttons), primary = false) {
            startActivity(android.content.Intent(this, ControllerMappingActivity::class.java))
        }.apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(14) }
        })
    }

    /** Percent slider with a live value label. */
    private fun slider(min: Int, max: Int, value: Int, onChange: (Int) -> Unit): LinearLayout {
        val rowView = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        val label = Ui.body(this, "$value%", Ui.INK, 13f)
        val bar = android.widget.SeekBar(this).apply {
            this.min = min
            this.max = max
            progress = value
            setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: android.widget.SeekBar?, p: Int, fromUser: Boolean) {
                    label.text = "$p%"
                    if (fromUser) onChange(p)
                }
                override fun onStartTrackingTouch(sb: android.widget.SeekBar?) {}
                override fun onStopTrackingTouch(sb: android.widget.SeekBar?) {}
            })
        }
        rowView.addView(bar, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        rowView.addView(label, LinearLayout.LayoutParams(dp(52), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            marginStart = dp(8)
        })
        return rowView
    }

    private fun renderFolders() {
        pane.addView(Ui.actionButton(this, getString(R.string.add_scan_folder), primary = true) {
            pickScanFolder.launch(null)
        }.apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(12) }
        })

        // Built-in roots are always scanned; show them so nobody wonders where
        // games are picked up from.
        for (root in GameLibrary.searchRoots()) {
            row(
                root.absolutePath.substringAfterLast('/'),
                root.absolutePath +
                    if (root.isDirectory) "" else getString(R.string.folder_missing),
                Ui.statusChip(this, getString(R.string.folder_builtin),
                    if (root.isDirectory) Ui.OK else Ui.MUTED)
            )
        }

        val folders = Settings.scanFolders(this).sorted()
        for (path in folders) {
            val missing = if (File(path).isDirectory) "" else getString(R.string.folder_missing)
            row(
                path.substringAfterLast('/'),
                path + missing,
                Ui.actionButton(this, getString(R.string.remove), primary = false) {
                    Settings.removeScanFolder(this, path)
                    renderPane()
                }
            )
        }

        if (Settings.hiddenGames(this).isNotEmpty()) {
            pane.addView(Ui.actionButton(
                this,
                getString(R.string.restore_hidden, Settings.hiddenGames(this).size),
                primary = false
            ) {
                Settings.unhideAllGames(this)
                renderPane()
                Toast.makeText(this, R.string.hidden_restored, Toast.LENGTH_SHORT).show()
            }.apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { topMargin = dp(16) }
            })
        }
    }

    private fun renderAbout() {
        pane.addView(Ui.body(this, getString(R.string.app_banner, EmuBridge.getVersion()), Ui.INK, 14.5f))
        pane.addView(Ui.body(this, getString(R.string.about_attribution), Ui.MUTED, 13f).apply {
            setPadding(0, dp(12), 0, 0)
        })
        pane.addView(Ui.body(this, getString(R.string.about_online_data), Ui.MUTED, 13f).apply {
            setPadding(0, dp(12), 0, 0)
        })
    }

    private companion object {
        const val STATE_CATEGORY = "category"
    }
}
