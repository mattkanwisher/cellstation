# Flame graphs with guest-code symbols

`docs/PROFILE-doa5.md` established the simpleperf capture workflow, and also its
limit: **58.3% of samples land in anonymous JIT mappings** with no symbols, so a
flame graph of that data is one giant `unknown` tower exactly where the answers
are. This page is the workflow that fixes it.

The core announces every JIT'd function (address, size, name) through
`jit_announce()`. Patch `0019-jit-perf-map-export.patch` makes it write those
announcements as a Linux perf map — `<hex addr> <hex size> <name>` lines in
`perf-<pid>.map` — whenever `RPCS3_PERF_MAP_DIR` is set. The JNI bridge sets
that variable when the `debug.cellstation.perfmap` system property is on, so
enabling it needs no UI and no rebuild-with-flags:

```sh
adb shell setprop debug.cellstation.perfmap 1     # setprop debug.… survives until reboot
```

PPU functions appear with their LLVM symbol names (module + `0x`-address
names), SPU blocks as `spu-b-<hash>`/LLVM SPU symbols. Names are addresses and
hashes, not source names — but that is enough to (a) separate PPU from SPU from
RSX time, (b) aggregate by function across the profile, and (c) cross-reference
a hot address into the SPU dump / Ghidra for the per-game analysis workflows.

## 1. Record (unchanged from PROFILE-doa5.md)

```sh
adb shell setprop debug.cellstation.perfmap 1
# launch the game, confirm "JIT perf-map export enabled" in adb logcat -s RPCS3
adb shell "simpleperf record --app nu.hyperworks.cellstation -g -f 1000 --duration 10 \
  -o /data/local/tmp/perf.data"
adb pull /data/local/tmp/perf.data
```

## 2. Pull the perf map

The map lands in the app root's `cache/` (the directory passed to
`EmuBridge.initialize`), one file per emulator process:

```sh
adb shell "ls /sdcard/Android/data/nu.hyperworks.cellstation/files/cache/perf-*.map"
adb pull /sdcard/Android/data/nu.hyperworks.cellstation/files/cache/perf-<pid>.map
```

(Adjust the root if your install passes a different directory; the boot log's
`JIT perf-map export enabled (…)` line prints the exact path. If `adb pull`
can't reach it on your Android version, `adb shell run-as` or the app's own
file manager export works.)

**The map must come from the same process as `perf.data`** — addresses are
runtime addresses. Match the pid, and re-pull after every run you record.
A warm boot writes fewer entries than a cold boot (cached modules still
announce; nothing needs recompiling).

## 3. Symbolize the emulator's own frames

As before — unstripped `libcellstation.so` via the NDK cache builder:

```sh
python3 $NDK/simpleperf/binary_cache_builder.py -i perf.data -lib <repo>/build-android
```

## 4. Fold, with JIT names merged in

```sh
tools/flamegraph.py -i perf.data --map perf-<pid>.map --symfs binary_cache -o out.folded
```

The script walks every sample's callchain through the NDK's
`simpleperf_report_lib`, substitutes JIT-map names for addresses that fall
inside announced ranges, and writes standard folded stacks
(`thread;frame;frame;… count`, weighted by sample period). Useful flags:

- `--thread SPU` — only threads whose name contains `SPU` (repeatable);
  emulator threads have meaningful names (`PPU[0x…]`, `SPU[0x…]`, `RSX`,
  SPURS threads carry the game's `…CellSpursKernel<n>` names).
- `--no-threads` — merge all threads into one graph.
- `--addrs` — keep raw addresses on frames with no symbol at all.

It prints how many leaves hit the JIT map, and warns when the map looks like it
came from a different run than the recording.

## 5. View

- **speedscope** (`npx speedscope out.folded`, or drag onto
  [speedscope.app](https://www.speedscope.app)) — interactive; *Sandwich* view
  ranks functions by self time, which is the flame-graph equivalent of the
  tables in PROFILE-doa5.md.
- **FlameGraph** (`flamegraph.pl out.folded > out.svg`) — the classic shareable
  SVG, nice for docs like this one.
- For a **timeline** flame chart per thread (what did SPU2 do during that 50 ms
  frame spike), the NDK's `gecko_profile_generator.py` → profiler.firefox.com
  works on the same `perf.data`, but without the JIT names; use it for timing
  shape, and this folded pipeline for attribution.

## Caveats

- **Guest frames are flat.** JIT code has no frame pointers or DWARF, so
  unwinding stops at the first guest frame: guest functions appear as leaves
  (or roots of one-frame stacks), not as guest→guest call trees. Attribution
  by function still works; caller/callee structure within guest code does not.
  That is the correct expectation for the questions this data serves (e.g.
  "how much of SPU time is the SPURS kernel vs. job payloads").
- **A few early helper stubs stay unnamed.** asmjit helpers built during
  static initialization run before `initialize()` can set the env var; their
  announces are lost. `jit_announce` re-checks the variable until it appears,
  so everything from emulator boot onward is captured.
- **The map file persists** in `cache/` (kept deliberately, since symbolization
  happens after the process dies) — old `perf-*.map` files accumulate until the
  app's cache is cleared. Harmless; delete freely.
- Property off again: `adb shell setprop debug.cellstation.perfmap 0`.
