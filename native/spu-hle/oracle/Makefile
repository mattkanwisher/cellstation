# Standalone build of the SPU DSP HLE oracle. No RPCS3, no Android, no deps.
#   make            build ./oracle
#   make test       build + run the analytic self-test
#   make roundtrip  build + generate a synthetic capture and replay it
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
BIN      := oracle

$(BIN): oracle_main.cpp ../dsp_effects.hpp ../dsp_capture.hpp
	$(CXX) $(CXXFLAGS) oracle_main.cpp -o $(BIN)

.PHONY: test roundtrip clean
test: $(BIN)
	./$(BIN) selftest

roundtrip: $(BIN)
	./$(BIN) gen /tmp/dsp_synth.dspcap
	./$(BIN) replay /tmp/dsp_synth.dspcap

clean:
	rm -f $(BIN) /tmp/dsp_synth.dspcap
