package nu.hyperworks.cellstation

import android.content.Context

/** App-side preferences (emulator settings live in the core's config.yml). */
object Settings {

    /** When the on-screen controller is shown. */
    enum class TouchOverlayMode {
        /** Shown until a physical controller is used, then hidden. */
        AUTO,
        ALWAYS,
        NEVER;

        companion object {
            fun from(name: String?): TouchOverlayMode =
                entries.firstOrNull { it.name == name } ?: AUTO
        }
    }

    private const val PREFS = "cellstation"
    private const val KEY_TOUCH_OVERLAY = "touch_overlay_mode"
    private const val KEY_SCAN_FOLDERS = "scan_folders"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun touchOverlayMode(context: Context): TouchOverlayMode =
        TouchOverlayMode.from(prefs(context).getString(KEY_TOUCH_OVERLAY, null))

    fun setTouchOverlayMode(context: Context, mode: TouchOverlayMode) {
        prefs(context).edit().putString(KEY_TOUCH_OVERLAY, mode.name).apply()
    }

    /** Extra folders the user pointed us at, on top of the conventional roots. */
    fun scanFolders(context: Context): Set<String> =
        prefs(context).getStringSet(KEY_SCAN_FOLDERS, emptySet()).orEmpty()

    fun addScanFolder(context: Context, path: String) {
        val updated = scanFolders(context) + path
        prefs(context).edit().putStringSet(KEY_SCAN_FOLDERS, updated).apply()
    }

    fun removeScanFolder(context: Context, path: String) {
        val updated = scanFolders(context) - path
        prefs(context).edit().putStringSet(KEY_SCAN_FOLDERS, updated).apply()
    }

    /**
     * Positional face-button mapping for Nintendo-layout pads (the Thor's
     * built-in controller): bottom = Cross, right = Circle, top = Triangle,
     * left = Square, regardless of the printed A/B/X/Y labels.
     */
    fun nintendoLayout(context: Context): Boolean =
        prefs(context).getBoolean(KEY_NINTENDO_LAYOUT, false)

    fun setNintendoLayout(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_NINTENDO_LAYOUT, enabled).apply()
    }

    private const val KEY_NINTENDO_LAYOUT = "nintendo_layout"

    /**
     * Id of the [DeviceProfile] already applied on this install, or "" if
     * none. Recording the id (not a bare flag) means a profile added in a
     * later version still reaches existing installs exactly once.
     */
    fun appliedDeviceProfile(context: Context): String =
        prefs(context).getString(KEY_DEVICE_PROFILE, "").orEmpty()

    fun setAppliedDeviceProfile(context: Context, id: String) {
        prefs(context).edit().putString(KEY_DEVICE_PROFILE, id).apply()
    }

    private const val KEY_DEVICE_PROFILE = "applied_device_profile"

    /**
     * Host CPU/GPU counters drawn over the game. Distinct from the core's own
     * performance overlay, which reports emulated load — this one answers
     * whether the phone itself is CPU- or GPU-bound.
     */
    fun hostStats(context: Context): Boolean =
        prefs(context).getBoolean(KEY_HOST_STATS, false)

    fun setHostStats(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_HOST_STATS, enabled).apply()
    }

    private const val KEY_HOST_STATS = "host_stats"

    /**
     * Android Dynamic Performance Framework hints. Experimental: it tells the
     * power governor which threads are on a frame deadline and how long their
     * work took. Read once at boot, so a change needs a restart of the game.
     */
    fun adpf(context: Context): Boolean =
        prefs(context).getBoolean(KEY_ADPF, true)

    fun setAdpf(context: Context, enabled: Boolean) =
        prefs(context).edit().putBoolean(KEY_ADPF, enabled).apply()

    private const val KEY_ADPF = "adpf"

    /** Directory name under gpu_drivers/ of the selected driver; "" = system. */
    fun gpuDriver(context: Context): String =
        prefs(context).getString(KEY_GPU_DRIVER, "").orEmpty()

    fun setGpuDriver(context: Context, dirName: String) {
        prefs(context).edit().putString(KEY_GPU_DRIVER, dirName).apply()
    }

    /** Boot path of the last game launched, for the library's Continue card. */
    fun lastPlayed(context: Context): String =
        prefs(context).getString(KEY_LAST_PLAYED, "").orEmpty()

    fun setLastPlayed(context: Context, bootPath: String) {
        prefs(context).edit()
            .putString(KEY_LAST_PLAYED, bootPath)
            .putLong(KEY_LAST_PLAYED_AT, System.currentTimeMillis())
            .apply()
    }

    fun lastPlayedAt(context: Context): Long =
        prefs(context).getLong(KEY_LAST_PLAYED_AT, 0L)

    /** True once the first-run wizard has been completed or skipped. */
    fun setupDone(context: Context): Boolean =
        prefs(context).getBoolean(KEY_SETUP_DONE, false)

    fun setSetupDone(context: Context) {
        prefs(context).edit().putBoolean(KEY_SETUP_DONE, true).apply()
    }

    /**
     * True once the wizard has been shown at all — even if the user backed out
     * without finishing. Everyone sees the wizard exactly once; after that it
     * only reappears while setup is actually incomplete.
     */
    fun setupSeen(context: Context): Boolean =
        prefs(context).getBoolean(KEY_SETUP_SEEN, false)

    fun markSetupSeen(context: Context) {
        prefs(context).edit().putBoolean(KEY_SETUP_SEEN, true).apply()
    }

    private const val KEY_SETUP_SEEN = "setup_seen"

    /** Boot paths the user removed from the library view (files stay on disk). */
    fun hiddenGames(context: Context): Set<String> =
        prefs(context).getStringSet(KEY_HIDDEN_GAMES, emptySet()).orEmpty()

    fun hideGame(context: Context, bootPath: String) {
        prefs(context).edit()
            .putStringSet(KEY_HIDDEN_GAMES, hiddenGames(context) + bootPath)
            .apply()
    }

    fun unhideAllGames(context: Context) {
        prefs(context).edit().remove(KEY_HIDDEN_GAMES).apply()
    }

    /** Opt-in for GameTDB covers + the RPCS3 compatibility feed. Default off. */
    fun onlineData(context: Context): Boolean =
        prefs(context).getBoolean(KEY_ONLINE_DATA, false)

    fun setOnlineData(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_ONLINE_DATA, enabled).apply()
    }

    /** On-screen controller opacity, percent 20..100. */
    fun overlayOpacity(context: Context): Int =
        prefs(context).getInt(KEY_OVERLAY_OPACITY, 100).coerceIn(20, 100)

    fun setOverlayOpacity(context: Context, percent: Int) {
        prefs(context).edit().putInt(KEY_OVERLAY_OPACITY, percent.coerceIn(20, 100)).apply()
    }

    /** On-screen controller size, percent 60..150. */
    fun overlayScale(context: Context): Int =
        prefs(context).getInt(KEY_OVERLAY_SCALE, 100).coerceIn(60, 150)

    fun setOverlayScale(context: Context, percent: Int) {
        prefs(context).edit().putInt(KEY_OVERLAY_SCALE, percent.coerceIn(60, 150)).apply()
    }

    private const val KEY_OVERLAY_OPACITY = "overlay_opacity"
    private const val KEY_OVERLAY_SCALE = "overlay_scale"
    private const val KEY_ONLINE_DATA = "online_data"
    private const val KEY_GPU_DRIVER = "gpu_driver"
    private const val KEY_LAST_PLAYED = "last_played"
    private const val KEY_LAST_PLAYED_AT = "last_played_at"
    private const val KEY_SETUP_DONE = "setup_done"
    private const val KEY_HIDDEN_GAMES = "hidden_games"
}
