package nu.hyperworks.cellstation

import android.view.Surface

/**
 * JNI surface over the RPCS3 core (libcellstation.so). Keep in sync with
 * native/bridge/bridge.cpp. CellStation is based on RPCS3 (GPL-2.0).
 */
object EmuBridge {
    init {
        System.loadLibrary("cellstation")
    }

    const val SURFACE_READY = 0
    const val SURFACE_DESTROYED = 2

    /**
     * config.yml paths for the core's own RSX performance overlay
     * (`g_cfg.video.perf_overlay`, drawn by overlay_perf_metrics.cpp). Every one
     * of these is marked dynamic in the core, so [globalConfigSet] applies them
     * to a running game as well as persisting them.
     *
     * Shared between the settings screen and the in-game quick panel, so the
     * two cannot drift apart.
     */
    const val PERF_ENABLED = "Video/Performance Overlay/Enabled"
    const val PERF_LEVEL = "Video/Performance Overlay/Detail level"
    const val PERF_POSITION = "Video/Performance Overlay/Position"
    const val PERF_FRAMERATE_GRAPH = "Video/Performance Overlay/Enable Framerate Graph"
    const val PERF_FRAMETIME_GRAPH = "Video/Performance Overlay/Enable Frametime Graph"

    /** True when the core's performance overlay is switched on in config.yml. */
    fun perfOverlayEnabled(): Boolean = globalConfigGet(PERF_ENABLED) == "true"

    /** Switches the performance overlay on or off. False when the core refused. */
    fun setPerfOverlayEnabled(enabled: Boolean): Boolean =
        globalConfigSet(PERF_ENABLED, enabled.toString())

    /**
     * Selects the GPU driver for this process; must be called before
     * [initialize] (driver changes take effect on the next app start).
     * Null [driverDir] means the system driver.
     */
    external fun setGpuDriver(driverDir: String?, driverName: String?, hookDir: String?, tmpDir: String?)

    external fun initialize(rootDir: String, user: String): Boolean

    /** Blocks forever, draining the core's main-thread queue. Call from a dedicated thread. */
    external fun runMainLoop()

    external fun installFirmware(fd: Int): Boolean

    /** Installs firmware from a path the app can open directly (see MainActivity: SAF fds can't be reopened). */
    external fun installFirmwarePath(path: String): Boolean

    external fun firmwareVersion(): String

    /**
     * Extracts PARAM.SFO / ICON0.PNG from a disc image into [outDir] using the
     * core's ISO reader. No-op while a game is running.
     */
    external fun extractIsoAssets(isoPath: String, outDir: String): Boolean

    /**
     * Deletes the compiled PPU/SPU/shader caches for one game (by TITLE_ID).
     * The next boot recompiles from scratch. Returns bytes freed; no-op while
     * a game is running.
     */
    external fun clearGameCache(serial: String): Long

    /**
     * Current boot/compile progress as "done\ttotal\ttext", or "" when nothing
     * is in progress. Backed by the same counters that drive rpcs3's desktop
     * progress dialog, so it covers PPU/SPU compilation as well as firmware and
     * disc installs.
     */
    external fun bootProgress(): String

    /**
     * Per-game settings (config/custom_configs/config_<SERIAL>.yml). The core
     * applies these on top of the global config when booting, so a game that
     * needs a compatibility setting doesn't impose it on the whole library.
     * [path] is "Section/Setting" exactly as the names appear in config.yml,
     * e.g. "Core/SPU XFloat Accuracy".
     */
    external fun gameConfigGet(serial: String, path: String): String

    /** Accepted values for [path], newline-separated; empty when unknown. */
    external fun gameConfigOptions(path: String): String

    external fun gameConfigSet(serial: String, path: String, value: String): Boolean

    /** True when this game has an override file at all. */
    external fun gameConfigExists(serial: String): Boolean

    /** Deletes the override file so the game follows the global config again. */
    external fun gameConfigReset(serial: String): Boolean

    /**
     * The global config (config/config.yml), addressed by the same
     * "Section/Setting" paths as the per-game calls. Nested sections are
     * spelled out in full, e.g. "Video/Performance Overlay/Enabled".
     */
    external fun globalConfigGet(path: String): String

    /** Accepted values for [path], newline-separated; empty when unknown. */
    external fun globalConfigOptions(path: String): String

    /**
     * Writes [path] to config.yml. Settings the core marks dynamic are also
     * applied to the running game; the rest wait for the next boot. Returns
     * false when the path or the value was rejected.
     */
    external fun globalConfigSet(path: String, value: String): Boolean

    /** Boots a game (path on the local filesystem). Returns game_boot_result (0 = ok). */
    external fun boot(path: String): Int

    /** Persisted "Stretch To Display Area" (config.yml). False = aspect-correct pillarboxing. */
    external fun stretchToDisplayArea(): Boolean

    /**
     * Stretches the emulated output to fill the display instead of honoring the
     * game's aspect ratio. Applies immediately (the setting is re-read per frame);
     * [persist] writes it to config.yml, otherwise it lasts for this boot only.
     */
    external fun setStretchToDisplayArea(enabled: Boolean, persist: Boolean)

    external fun surfaceEvent(surface: Surface?, event: Int): Boolean

    /** Pushes a full pad snapshot (values 0..255, indexed by PadButton). */
    external fun setPadState(values: ByteArray)

    external fun setPadConnected(connected: Boolean)

    /**
     * Enables ADPF performance hints. Takes effect on the next flip, so it can
     * be flipped mid-scene to A/B the same moment of a game.
     */
    external fun setAdpfEnabled(enabled: Boolean)

    external fun kill()
    external fun pause()
    external fun resume()
    external fun getState(): Int
    external fun getVersion(): String
}
