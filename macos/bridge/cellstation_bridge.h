// CellStation macOS bridge — a C ABI over the unmodified rpcs3 core, mirroring
// the Android JNI surface (native/bridge/chrysalis_jni.cpp) with the JNI and
// Android specifics stripped. Every emulator verb the Swift app / AppleScript
// layer and the CLI harness need maps onto exactly one function here.
//
// CellStation is based on RPCS3 (GPL-2.0), (c) RPCS3 team and contributors.

#ifndef CELLSTATION_BRIDGE_H
#define CELLSTATION_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- lifecycle -------------------------------------------------------------

// Initialize the core exactly once. data_dir, when non-empty, becomes the root
// of an isolated app-data tree (config + cache nest under it) so a dev build
// never disturbs a real rpcs3 install; pass "" to use the default macOS dirs
// (~/Library/Application Support/rpcs3, ~/Library/Caches/rpcs3). user is the
// PS3 user id ("00000001" when empty). Returns 1 on success.
int cs_initialize(const char* data_dir, const char* user);

// Drain the main-thread work queue forever. Call this ONCE, on the thread you
// designate as the "main" thread (the core posts work to it via
// call_from_main_thread). Blocks until cs_stop_main_loop().
void cs_run_main_loop(void);

// Ask cs_run_main_loop() to return after the queue drains.
void cs_stop_main_loop(void);

// ---- boot / control --------------------------------------------------------

// Boot an ELF/SELF/ISO/game-dir. Returns rpcs3's game_boot_result (0 ==
// no_errors). Runs the boot on the main-loop thread and waits for it.
int cs_boot(const char* path);

void cs_kill(void);    // stop emulation (no savestate)
void cs_pause(void);
void cs_resume(void);
void cs_reset(void);   // restart the current title

// rpcs3 system_state enum value (0 stopped .. running/paused/...).
int cs_get_state(void);

// Human-readable state ("stopped"/"running"/"paused"/...). Valid until the
// next cs_status_text() call (returns a pointer into a static buffer).
const char* cs_status_text(void);

// Emulated frames-per-second from the core's perf counters, or 0 when not
// running / unavailable.
double cs_get_fps(void);

// ---- firmware --------------------------------------------------------------

// Install a PS3 firmware PUP. Returns "" on success or a human-readable error.
const char* cs_install_firmware(const char* pup_path);

// Installed firmware version, or "" when none is installed.
const char* cs_firmware_version(void);

// ---- pad input -------------------------------------------------------------
// Byte-array snapshot, same layout the Android app uses (see
// macos/bridge/macos_pad.h). Delivery to the emulated pad is wired in M3.

void cs_set_pad_state(const uint8_t* bytes, int len);
void cs_set_pad_connected(int connected);

// ---- savestates ------------------------------------------------------------
// slot is an integer 0..N; the bridge maps it to a file under
// <config>/savestates/. Returns 1 on success.

int cs_save_state(int slot);
int cs_load_state(int slot);

// ---- surface / rendering (M3) ----------------------------------------------
// layer is a CAMetalLayer* (as void*). Passing NULL detaches the surface.
void cs_surface_changed(void* metal_layer, int width, int height);

// Grab the last presented frame to a PNG. Returns 1 on success. Headless
// builds (Null renderer) return 0.
int cs_capture_screenshot(const char* png_path);

// ---- misc ------------------------------------------------------------------

// Verbose rpcs3 version string.
const char* cs_version(void);

#ifdef __cplusplus
}
#endif

#endif // CELLSTATION_BRIDGE_H
