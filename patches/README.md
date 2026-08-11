# patches/

The **only** place this project is allowed to diverge from upstream RPCS3.

The `rpcs3/` submodule is read-only and pinned to an upstream master SHA. Any change
to upstream code must be a numbered patch file here, applied to the submodule at build
time in the order listed in `series`. CI verifies the whole series applies cleanly to
the pinned SHA on every commit and every submodule bump.

## Rules

1. Prefer no patch at all: use upstream extension points first.
2. Every patch starts with a header block:

   ```
   Subject: <what it does>
   Why: <why Android needs it>
   Upstream-status: candidate | submitted (<PR link>) | rejected | local-only
   ```

3. Patches that qualify get submitted to RPCS3 through their normal PR process and
   are deleted from here once merged upstream.
4. Keep patches minimal and mechanical; emulation logic changes belong upstream, not here.

## Applying

```sh
cd rpcs3
git apply --check ../patches/*.patch   # verify
git apply ../patches/*.patch           # apply (build systems do this automatically)
```

Use `git -C rpcs3 checkout -- . && git -C rpcs3 clean -fd` to reset the submodule.

## Current series

1. `0001-cmake-embed-as-subproject.patch` — resolve rpcs3-relative CMake paths
   via `rpcs3_SOURCE_DIR`/`rpcs3_BINARY_DIR` so the core builds when embedded
   via `add_subdirectory`. Upstream-status: candidate.
2. `0002-cellmic-without-openal.patch` — fix WITHOUT_OPENAL build: guard the
   `alc.h` include (opaque ALC typedefs instead) and the `fmt::alc_error`
   formatter. Upstream-status: candidate.
3. `0003-strfmt-noreturn-dtor-clang18.patch` — clang 18+ ignores `[[noreturn]]`
   on defaulted functions; give `fmt::throw_exception`'s destructor a
   `__builtin_unreachable()` body so `-Werror=return-type` passes.
   Upstream-status: candidate.
4. `0004-vk-android-surface-fix.patch` — `make_WSI_surface` for Android used
   `VkWin32SurfaceCreateInfoKHR` and `this->m_instance` in a free function;
   never-compiled code. Upstream-status: candidate.
5. `0005-vkoverlays-include-image.patch` — `VKOverlays.h` needs the full
   `vk::image` definition (unique_ptr members + virtual dtor); libc++
   rejects the forward-decl-only include chain. Upstream-status: candidate.
6. `0006-jitllvm-llvm-pre21-compat.patch` — `Module::setTargetTriple(llvm::Triple)`
   requires LLVM ≥ 21 but the build system accepts prebuilt LLVM ≥ 18; guard
   the two call sites. Upstream-status: candidate.
7. `0007-arm-baseline-override.patch` — make the non-Apple arm64 `-march`
   baseline overridable (`RPCS3_ARM_BASELINE`, default armv8.1-a); the
   emulator's arm64 binary translator SIGILLs on LSE. Upstream-status:
   candidate.
8. `0008-simd-qrdmx-guard.patch` — guard ARMv8.1-only code for lower
   baselines: `vqrdmlahq_s16` (QRDMX) gets a bit-exact armv8.0 NEON
   fallback in `simd.hpp`; the hand-coded LSE `ldset` in
   `trigger_write_page_fault` is gated on `__ARM_FEATURE_ATOMICS`
   (generic atomic-RMW branch otherwise). Upstream-status: candidate.
9. `0009-vk-adreno-driver-vendor.patch` — add `driver_vendor::ADRENO`
   (Qualcomm proprietary + Mesa Turnip driver IDs, GPU-name fallback) so
   Snapdragon GPUs stop logging "Unknown driver vendor!" and can grow
   vendor-specific paths. Upstream-status: candidate.
10. `0010-cntfrq-sigill-safe.patch` — `mrs cntfrq_el0` in a static
    initializer SIGILLs under binary translators that don't implement the
    register (the emulator's ndk_translation); read it via a SIGILL-guarded
    helper that falls back to calibrating `cntvct` against
    `CLOCK_MONOTONIC`. Upstream-status: candidate.
11. `0011-vk-unsized-ubo-array-fallback.patch` — the shader backends
    require `VK_EXT_shader_uniform_buffer_unsized_array`, which no
    Qualcomm Adreno driver exposes, so every pipeline creation failed
    with `VK_ERROR_UNKNOWN` on Snapdragon; emit the affected uniform
    blocks as readonly storage buffers when the extension is missing
    (hardware-verified on AYN Thor / Adreno 740). Upstream-status:
    candidate.
12. `0012-pad-thread-android-handler-hook.patch` — pad_thread's Android
    branches short-circuit handler creation, so an embedder-provided
    pad handler (the JNI bridge's) was never wired in; hook it into
    both `#ifdef ANDROID` paths. Upstream-status: not applicable.
13. `0013-cubeb-default-device-fallback.patch` — cubeb's AAudio/OpenSL
    backends do not support device enumeration, so audio init failed;
    fall back to the default output device. Upstream-status: candidate.
14. `0014-vk-adreno-gpu-memory-management.patch` — on unified-memory
    Android devices the driver reports all of system RAM as VRAM, so
    no eviction heuristic ever fired, and Adreno backs command buffers
    and descriptor sets with GPU memory that is invisible to the
    process's RSS: a 512-deep command-buffer ring pinned 0.7–5 MB per
    buffer of retained indirect-buffer memory (gigabytes across a heavy
    scene) until the kernel OOM killer took the process. Cap the VRAM
    budget at a quarter of RAM, shrink the ring to 64 on Android, reset
    command buffers with `RELEASE_RESOURCES`, and use larger descriptor
    subpools to cut pool churn (hardware-verified on AYN Thor / Adreno
    740: GPU memory now plateaus instead of climbing ~300 MB/s).
    Upstream-status: candidate (Android parts).
15. `0015-overlay-fonts-android-system-fonts.patch` — the overlay renderer
    (progress dialogs, messages, home menu) throws a fatal when no font is
    found, which kills the RSX thread and leaves a black screen. Its font
    name list has no Android entries and its only lookup dir on Android was
    `dev_flash`, so before firmware is installed there was nothing to load:
    booting a game without firmware black-screened instead of showing the
    compile progress dialog. Add Android's system font names (Roboto/Noto/
    DroidSans, plus NotoSansCJK for CJK and hangul); the embedder supplies
    `/system/fonts/` et al through `get_font_dirs`. Upstream-status:
    candidate.
16. `0016-vk-unknown-gpu-safe-transport.patch` — `chip_class::unknown` is 0,
    so any GPU whose vendor `get_chip_family()` doesn't recognize (e.g.
    Adreno) satisfied `gpu_family < chip_class::NV_turing` and took an
    NVIDIA-only unsafe typeless-transport hack; require a recognized NVIDIA
    family instead. Upstream-status: candidate.
17. `0017-android-big-core-affinity.patch` — add `arm_big_little` CPU
    topology so emulated CPU threads stay off the little cores on Android.
    Upstream-status: candidate.
18. `0018-vk-surface-lost-recovery.patch` — Android destroys a backgrounded
    app's native window and with it the `VkSurfaceKHR`; the RSX thread died
    in `vk::die_with_error` when `vkAcquireNextImageKHR` returned
    `VK_ERROR_SURFACE_LOST` (only `OUT_OF_DATE` was handled). Treat surface
    loss like an out-of-date swapchain — pause presentation, drain flips —
    and on the next rebuild recreate the surface itself from the new window
    before the swapchain (ANDROID-gated; desktop windows outlive the
    renderer). Surface queries inside swapchain init fail softly for the
    same reason. Upstream-status: candidate.
