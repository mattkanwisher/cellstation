#!/usr/bin/env python3
# Fold a simpleperf recording into flame-graph "folded stacks", naming RPCS3
# JIT frames from the core's perf map (patch 0018 + debug.cellstation.perfmap).
#
# Pipeline (see docs/FLAMEGRAPHS.md for the full walkthrough):
#   tools/flamegraph.py -i perf.data --map perf-1234.map --symfs binary_cache \
#       -o doa5.folded
#   npx speedscope doa5.folded          # interactive flame chart
#   flamegraph.pl doa5.folded > doa5.svg  # classic static flame graph
#
# Needs the NDK's simpleperf host scripts (simpleperf_report_lib.py +
# libsimpleperf_report.so) — the same ones PROFILE-doa5.md already uses for
# binary_cache_builder.py. Located via --ndk or $NDK / $ANDROID_NDK_HOME /
# $ANDROID_NDK_ROOT.

import argparse
import bisect
import os
import sys
from collections import Counter


def find_ndk(cli_value):
    candidates = [cli_value] if cli_value else []
    candidates += [os.environ.get(v) for v in ("NDK", "ANDROID_NDK_HOME", "ANDROID_NDK_ROOT")]
    for c in candidates:
        if c and os.path.isdir(os.path.join(c, "simpleperf")):
            return c
    sys.exit("error: NDK with simpleperf scripts not found; pass --ndk or set $NDK")


class PerfMap:
    """Lookup table built from perf-<pid>.map lines: '<hex addr> <hex size> <name>'."""

    def __init__(self):
        self.starts = []
        self.entries = []  # (start, end, name), sorted by start

    def load(self, path):
        rows = []
        with open(path, "r", errors="replace") as f:
            for line in f:
                parts = line.rstrip("\n").split(" ", 2)
                if len(parts) != 3:
                    continue
                try:
                    addr, size = int(parts[0], 16), int(parts[1], 16)
                except ValueError:
                    continue
                if size > 0 and parts[2]:
                    rows.append((addr, addr + size, parts[2]))
        # Later announces win on overlap (a re-JIT reuses the address range).
        rows.sort(key=lambda r: r[0])
        self.entries = rows
        self.starts = [r[0] for r in rows]

    def lookup(self, ip):
        i = bisect.bisect_right(self.starts, ip) - 1
        if i >= 0:
            start, end, name = self.entries[i]
            if start <= ip < end:
                return name
        return None


def frame_name(map_, ip, symbol, args):
    jit = map_.lookup(ip)
    if jit is not None:
        return jit
    name = symbol.symbol_name
    if name and name != "unknown":
        return name
    dso = os.path.basename(symbol.dso_name or "") or "??"
    if args.addrs:
        return "[%s]+0x%x" % (dso, ip)
    return "[%s]" % dso


def clean(name):
    # ';' separates frames and trailing ' <count>' ends the line in the folded
    # format; keep frame names unambiguous for the downstream tools.
    return name.replace(";", ":").strip() or "??"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--record-file", default="perf.data", help="simpleperf perf.data (default: ./perf.data)")
    ap.add_argument("--map", action="append", default=[], help="RPCS3 perf-<pid>.map for JIT symbols (repeatable)")
    ap.add_argument("--symfs", help="binary_cache dir from binary_cache_builder.py (unstripped libcellstation.so)")
    ap.add_argument("--kallsyms", help="optional kallsyms file for kernel symbols")
    ap.add_argument("-o", "--output", default="-", help="output .folded file (default: stdout)")
    ap.add_argument("--ndk", help="NDK root containing simpleperf/ (else $NDK / $ANDROID_NDK_HOME)")
    ap.add_argument("--no-threads", action="store_true", help="do not prefix stacks with the thread name")
    ap.add_argument("--thread", action="append", default=[], help="only include threads whose name contains this substring (repeatable)")
    ap.add_argument("--addrs", action="store_true", help="show raw addresses for frames with no symbol at all")
    args = ap.parse_args()

    sys.path.insert(0, os.path.join(find_ndk(args.ndk), "simpleperf"))
    from simpleperf_report_lib import ReportLib  # noqa: E402

    map_ = PerfMap()
    for m in args.map:
        map_.load(m)

    lib = ReportLib()
    lib.SetRecordFile(args.record_file)
    if args.symfs:
        lib.SetSymfs(args.symfs)
    if args.kallsyms:
        lib.SetKallsymsFile(args.kallsyms)
    lib.ShowIpForUnknownSymbol()

    folded = Counter()
    samples = jit_hits = jit_misses = 0

    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        thread = sample.thread_comm
        if args.thread and not any(t in thread for t in args.thread):
            continue
        samples += 1

        # Leaf frame, then callchain entries walking towards the root.
        stack = []
        leaf_sym = lib.GetSymbolOfCurrentSample()
        stack.append(frame_name(map_, sample.ip, leaf_sym, args))
        if map_.lookup(sample.ip) is not None:
            jit_hits += 1
        elif leaf_sym.symbol_name in ("unknown", "") or leaf_sym.symbol_name.startswith("0x"):
            jit_misses += 1

        chain = lib.GetCallChainOfCurrentSample()
        for i in range(chain.nr):
            entry = chain.entries[i]
            stack.append(frame_name(map_, entry.ip, entry.symbol, args))

        stack.reverse()  # root .. leaf
        if not args.no_threads:
            stack.insert(0, thread)
        folded[";".join(clean(f) for f in stack)] += sample.period

    lib.Close()

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    for key, count in folded.most_common():
        out.write("%s %d\n" % (key, count))
    if out is not sys.stdout:
        out.close()

    total_unsym = jit_hits + jit_misses
    sys.stderr.write(
        "%d samples, %d unique stacks; leaf in JIT map: %d, unsymbolized leaves: %d%s\n"
        % (
            samples,
            len(folded),
            jit_hits,
            jit_misses,
            "" if map_.entries or not jit_misses else "  (no --map given — pass the perf-<pid>.map to name JIT frames)",
        )
    )
    if map_.entries and total_unsym and jit_misses > jit_hits:
        sys.stderr.write(
            "warning: most unsymbolized leaves missed the JIT map — check the map is from the SAME run as perf.data\n"
        )


if __name__ == "__main__":
    main()
