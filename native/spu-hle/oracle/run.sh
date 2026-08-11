#!/usr/bin/env bash
# Build and run the SPU DSP HLE oracle with no dependencies beyond a C++17
# compiler. Runs the analytic self-test, then the record/replay round-trip.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra oracle_main.cpp -o oracle

echo "=== analytic self-test ==="
./oracle selftest

echo
echo "=== synthetic capture round-trip ==="
tmp="$(mktemp -t dsp_synth.XXXXXX.dspcap)"
./oracle gen "$tmp"
./oracle replay "$tmp"
rm -f "$tmp"
