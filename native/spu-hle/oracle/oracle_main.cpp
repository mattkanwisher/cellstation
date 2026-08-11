// SPU DSP HLE — host-side recorded-I/O oracle + analytic self-test.
//
// This is the fast local loop the pipeline doc calls for: it runs entirely on
// the host, no device, no emulator. Two jobs:
//
//   oracle selftest
//       Verify each effect against an INDEPENDENT reference (closed-form
//       transfer function for the filters, a brute-force scan for the meter, a
//       Schroeder RT60 measurement for the reverb). This is the real proof of
//       correctness of the algorithm and is what runs in CI today, because no
//       on-device capture ships in the repo yet.
//
//   oracle gen <file.dspcap>
//       Write a synthetic capture whose reference output is the C++ effect's
//       own output. Exercises the record/replay plumbing end-to-end.
//
//   oracle replay <file.dspcap>
//       Replay recorded (params,input) through the C++ effect and diff against
//       the recorded output. THIS is the mode a real device capture goes
//       through: an effect ships only if replay of a real capture passes. With
//       a synthetic capture it is a plumbing/round-trip check.
//
// Build: native/spu-hle/oracle/{Makefile,CMakeLists.txt} or run.sh.

#include "../dsp_capture.hpp"
#include "../dsp_effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace spu_hle;

namespace {

int g_fail = 0;
void check(bool ok, const char* what, double got = 0, double want = 0, double tol = 0) {
	if (ok) {
		std::printf("  PASS  %s\n", what);
	} else {
		std::printf("  FAIL  %s (got %.6g want %.6g tol %.3g)\n", what, got, want, tol);
		++g_fail;
	}
}

// Discrete-time transfer-function magnitude of an impulse response h at freq f.
double dft_mag(const std::vector<float>& h, double fs, double f) {
	const double w = 2.0 * M_PI * f / fs;
	double re = 0.0, im = 0.0;
	for (size_t n = 0; n < h.size(); ++n) {
		re += h[n] * std::cos(w * n);
		im -= h[n] * std::sin(w * n);
	}
	return std::sqrt(re * re + im * im);
}

// ---- meter -----------------------------------------------------------------

void test_meter() {
	std::printf("[meter]\n");
	const int ch = 2, frames = 1536;
	std::vector<float> block(ch * frames);
	// deterministic pseudo-random + a known spike in channel 0
	uint32_t s = 0x1234567u;
	auto rnd = [&] { s = s * 1664525u + 1013904223u; return (int32_t(s) / float(1u << 31)); };
	float ref_peak[2] = {0, 0};
	double ref_ss[2] = {0, 0};
	for (int f = 0; f < frames; ++f)
		for (int c = 0; c < ch; ++c) {
			float v = 0.5f * rnd();
			if (c == 0 && f == 700) v = 0.9f; // known peak
			block[f * ch + c] = v;
			ref_peak[c] = std::max(ref_peak[c], std::fabs(v));
			ref_ss[c] += double(v) * v;
		}

	meter_state mst;
	meter_params mp{ 0.0f }; // no hold: block peak == max|x|
	meter_result r = meter_process(mst, block.data(), sample_format::f32, ch, frames, mp);

	for (int c = 0; c < ch; ++c) {
		check(std::fabs(r.peak[c] - ref_peak[c]) < 1e-6f, "peak == max|x|",
		      r.peak[c], ref_peak[c], 1e-6);
		double ref_rms = std::sqrt(ref_ss[c] / frames);
		check(std::fabs(r.rms[c] - ref_rms) < 1e-5, "rms == sqrt(mean(x^2))",
		      r.rms[c], ref_rms, 1e-5);
	}

	// Peak-hold decay: feed a spike block then silent blocks; peak must fall by
	// exactly the decay factor each block.
	meter_state hst;
	meter_params hp{ kMeterDecay };
	std::vector<float> spike(ch * frames, 0.0f);
	spike[0] = 1.0f;
	meter_result a = meter_process(hst, spike.data(), sample_format::f32, ch, frames, hp);
	std::vector<float> silent(ch * frames, 0.0f);
	meter_result b = meter_process(hst, silent.data(), sample_format::f32, ch, frames, hp);
	check(std::fabs(a.peak[0] - 1.0f) < 1e-6f, "hold latches spike", a.peak[0], 1.0f, 1e-6);
	check(std::fabs(b.peak[0] - kMeterDecay) < 1e-6f, "hold decays by coeff",
	      b.peak[0], kMeterDecay, 1e-6);
}

// ---- biquad: time-domain filter vs analytic transfer function --------------

void test_biquad_response(const char* name, const biquad_coeffs& k, double fs,
                          const std::vector<double>& probe_hz) {
	std::printf("[%s]\n", name);
	// Impulse response, long enough for the IIR tail to decay.
	const int N = 8192;
	std::vector<float> imp(N, 0.0f), h(N);
	imp[0] = 1.0f;
	biquad_params bp{}; bp.stages = 1; bp.coeffs[0] = k;
	biquad_state bs{};
	// process as single-channel float block
	biquad_process(bs, bp, imp.data(), h.data(), sample_format::f32, 1, N);

	for (double f : probe_hz) {
		double got = dft_mag(h, fs, f);
		double want = biquad_response(k, fs, f);
		double tol = 1e-3 + 5e-3 * want; // DFT truncation + float rounding
		char buf[64];
		std::snprintf(buf, sizeof buf, "|H| @ %.0f Hz", f);
		check(std::fabs(got - want) < tol, buf, got, want, tol);
	}
}

void test_filters() {
	const double fs = 48000.0;
	std::vector<double> probes = { 100, 500, 1000, 4000, 8000, 16000, 20000 };
	test_biquad_response("filter lowpass 2kHz Q0.707",
	                     biquad_lowpass(fs, 2000, 0.7071), fs, probes);
	test_biquad_response("filter highpass 500Hz Q0.707",
	                     biquad_highpass(fs, 500, 0.7071), fs, probes);

	// para_eq: peaking stages. Verify the gain at f0 equals the requested dB.
	std::printf("[para_eq peaking gains]\n");
	struct { double f0, q, db; } stages[] = {
		{ 120, 1.0, 6.0 }, { 1000, 2.0, -4.0 }, { 6000, 1.5, 3.0 },
	};
	for (auto& sdef : stages) {
		biquad_coeffs k = biquad_peaking(fs, sdef.f0, sdef.q, sdef.db);
		double mag = biquad_response(k, fs, sdef.f0);
		double got_db = 20.0 * std::log10(mag);
		char buf[80];
		std::snprintf(buf, sizeof buf, "peaking %.0fHz gain", sdef.f0);
		check(std::fabs(got_db - sdef.db) < 0.05, buf, got_db, sdef.db, 0.05);
	}

	// Cascade the three peaking stages and confirm the time-domain cascade's
	// response equals the product of the analytic stage responses.
	std::printf("[para_eq cascade vs analytic product]\n");
	biquad_params bp{}; bp.stages = 3;
	for (int i = 0; i < 3; ++i)
		bp.coeffs[i] = biquad_peaking(fs, stages[i].f0, stages[i].q, stages[i].db);
	const int N = 8192;
	std::vector<float> imp(N, 0.0f), h(N); imp[0] = 1.0f;
	biquad_state bs{};
	biquad_process(bs, bp, imp.data(), h.data(), sample_format::f32, 1, N);
	for (double f : { 120.0, 1000.0, 6000.0, 3000.0 }) {
		double got = dft_mag(h, fs, f);
		double want = 1.0;
		for (int i = 0; i < 3; ++i) want *= biquad_response(bp.coeffs[i], fs, f);
		double tol = 1e-3 + 8e-3 * want;
		char buf[64];
		std::snprintf(buf, sizeof buf, "cascade |H| @ %.0f Hz", f);
		check(std::fabs(got - want) < tol, buf, got, want, tol);
	}
}

// ---- i3dl2 reverb: RT60 vs requested decay time ----------------------------

double measure_rt60(const std::vector<float>& h, double fs) {
	// Schroeder backward energy integration, then fit the -5..-25 dB slope (T20).
	const int N = static_cast<int>(h.size());
	std::vector<double> edc(N);
	double acc = 0.0;
	for (int n = N - 1; n >= 0; --n) { acc += double(h[n]) * h[n]; edc[n] = acc; }
	if (edc[0] <= 0) return 0;
	std::vector<double> db(N);
	for (int n = 0; n < N; ++n) db[n] = 10.0 * std::log10(edc[n] / edc[0] + 1e-30);

	int i5 = -1, i25 = -1;
	for (int n = 0; n < N; ++n) {
		if (i5 < 0 && db[n] <= -5.0) i5 = n;
		if (i25 < 0 && db[n] <= -25.0) { i25 = n; break; }
	}
	if (i5 < 0 || i25 < 0 || i25 <= i5) return 0;
	double t20 = (i25 - i5) / fs;
	return 3.0 * t20; // RT60 = 3 * T20
}

void test_reverb() {
	std::printf("[i3dl2 reverb]\n");
	const int fs = 48000;
	const double want_rt60 = 1.5;
	i3dl2_params p{};
	p.room = 0; p.room_hf = 0;
	p.decay_time = float(want_rt60);
	p.decay_hf_ratio = 1.0f; // no extra HF damping for a clean RT60 test
	p.reflections = -1000; p.reflections_delay = 0.007f;
	p.reverb = 0; p.reverb_delay = 0.011f;
	p.diffusion = 100.0f; p.density = 100.0f; p.hf_reference = 5000.0f;
	p.dry_level = 0.0f; p.sample_rate = fs;

	i3dl2_state st;
	i3dl2_configure(st, p, fs);

	const int N = int(2.5 * want_rt60 * fs);
	std::vector<float> imp(2 * N, 0.0f);
	imp[0] = 1.0f; imp[1] = 1.0f; // stereo impulse
	std::vector<float> out(2 * N);
	i3dl2_process(st, p, imp.data(), out.data(), sample_format::f32, 2, N, fs);

	// Split back to per-channel to measure decay and stereo decorrelation.
	std::vector<float> L(N), R(N);
	bool finite = true;
	for (int n = 0; n < N; ++n) {
		L[n] = out[2 * n]; R[n] = out[2 * n + 1];
		if (!std::isfinite(L[n]) || !std::isfinite(R[n])) finite = false;
	}
	check(finite, "output is finite (no blowup)");

	double rt60 = measure_rt60(L, fs);
	// Reverb is perceptual, not bit-exact: require the measured tail within a
	// factor of 2 of the requested decay time (the mapping is principled, but
	// comb summation and diffusion perturb the exact slope).
	check(rt60 > want_rt60 * 0.5 && rt60 < want_rt60 * 2.0,
	      "measured RT60 ~ requested decay_time", rt60, want_rt60, want_rt60);

	// Stereo decorrelation: the two channels must differ (spread applied).
	double diff = 0.0, energy = 0.0;
	for (int n = 0; n < N; ++n) { diff += std::fabs(L[n] - R[n]); energy += std::fabs(L[n]); }
	check(energy > 0 && diff / (energy + 1e-9) > 0.05, "stereo channels decorrelated",
	      diff / (energy + 1e-9), 0.05, 0.05);

	// A shorter decay time must give a shorter tail (monotonic mapping).
	i3dl2_params p2 = p; p2.decay_time = 0.6f;
	i3dl2_state st2; i3dl2_configure(st2, p2, fs);
	std::vector<float> out2(2 * N);
	std::vector<float> imp2 = imp;
	i3dl2_process(st2, p2, imp2.data(), out2.data(), sample_format::f32, 2, N, fs);
	std::vector<float> L2(N);
	for (int n = 0; n < N; ++n) L2[n] = out2[2 * n];
	double rt60b = measure_rt60(L2, fs);
	check(rt60b < rt60, "shorter decay_time => shorter RT60", rt60b, rt60, 0);
}

// ---- capture file read/write ----------------------------------------------

void write_record(std::ofstream& f, const dsp_cap_record_header& h,
                  const void* params, const void* in, const void* out) {
	f.write(reinterpret_cast<const char*>(&h), sizeof h);
	f.write(reinterpret_cast<const char*>(params), h.param_bytes);
	f.write(reinterpret_cast<const char*>(in), h.in_bytes);
	f.write(reinterpret_cast<const char*>(out), h.out_bytes);
}

int cmd_gen(const char* path) {
	std::ofstream f(path, std::ios::binary);
	if (!f) { std::printf("cannot open %s\n", path); return 2; }
	dsp_cap_file_header fh{};
	std::memcpy(fh.magic, kMagic, 8);
	fh.version = kVersion; fh.record_count = 2;
	f.write(reinterpret_cast<const char*>(&fh), sizeof fh);

	const int ch = 2, frames = 1536, fs = 48000;
	std::vector<float> in(ch * frames);
	uint32_t s = 0xC0FFEEu;
	for (auto& v : in) { s = s * 1664525u + 1013904223u; v = 0.3f * (int32_t(s) / float(1u << 31)); }

	// Record 0: filter (single lowpass). Reference = the C++ effect's output.
	{
		biquad_params bp{}; bp.stages = 1; bp.coeffs[0] = biquad_lowpass(fs, 3000, 0.7071);
		std::vector<float> out(ch * frames);
		biquad_state bs{};
		biquad_process(bs, bp, in.data(), out.data(), sample_format::f32, ch, frames);
		dsp_cap_record_header h{};
		h.effect = uint32_t(effect_id::filter); h.format = uint32_t(sample_format::f32);
		h.channels = ch; h.frames = frames; h.sample_rate = fs;
		h.param_bytes = sizeof(bp); h.in_bytes = in.size() * 4; h.out_bytes = out.size() * 4;
		h.seq = 0;
		write_record(f, h, &bp, in.data(), out.data());
	}
	// Record 1: meter. Reference output = the meter_result struct bytes.
	{
		meter_params mp{ kMeterDecay };
		meter_state mst{};
		meter_result r = meter_process(mst, in.data(), sample_format::f32, ch, frames, mp);
		dsp_cap_record_header h{};
		h.effect = uint32_t(effect_id::meter); h.format = uint32_t(sample_format::f32);
		h.channels = ch; h.frames = frames; h.sample_rate = fs;
		h.param_bytes = sizeof(mp); h.in_bytes = in.size() * 4; h.out_bytes = sizeof(r);
		h.seq = 1;
		write_record(f, h, &mp, in.data(), &r);
	}
	std::printf("wrote %s (2 records)\n", path);
	return 0;
}

int cmd_replay(const char* path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { std::printf("cannot open %s\n", path); return 2; }
	dsp_cap_file_header fh{};
	f.read(reinterpret_cast<char*>(&fh), sizeof fh);
	if (std::memcmp(fh.magic, kMagic, 8) != 0 || fh.version != kVersion) {
		std::printf("bad magic/version\n"); return 2;
	}
	std::printf("[replay %s] %u record(s)\n", path, fh.record_count);
	int rec = 0;
	for (;;) {
		dsp_cap_record_header h{};
		f.read(reinterpret_cast<char*>(&h), sizeof h);
		if (!f) break;
		std::vector<uint8_t> params(h.param_bytes), in(h.in_bytes), ref(h.out_bytes);
		f.read(reinterpret_cast<char*>(params.data()), h.param_bytes);
		f.read(reinterpret_cast<char*>(in.data()), h.in_bytes);
		f.read(reinterpret_cast<char*>(ref.data()), h.out_bytes);
		if (!f) { std::printf("  truncated record %d\n", rec); ++g_fail; break; }

		const auto fmt = sample_format(h.format);
		std::vector<uint8_t> got(h.out_bytes);
		double max_abs = 0.0;
		bool ok = true;

		switch (effect_id(h.effect)) {
		case effect_id::meter: {
			meter_params mp{}; std::memcpy(&mp, params.data(), std::min<size_t>(sizeof mp, params.size()));
			meter_state st{};
			meter_result r = meter_process(st, in.data(), fmt, h.channels, h.frames, mp);
			std::memcpy(got.data(), &r, std::min<size_t>(sizeof r, got.size()));
			const float* a = reinterpret_cast<const float*>(got.data());
			const float* b = reinterpret_cast<const float*>(ref.data());
			size_t nf = h.out_bytes / 4;
			for (size_t i = 0; i < nf; ++i) max_abs = std::max(max_abs, std::fabs(double(a[i]) - b[i]));
			ok = max_abs < 1e-5;
			break;
		}
		case effect_id::filter:
		case effect_id::para_eq: {
			biquad_params bp{}; std::memcpy(&bp, params.data(), std::min<size_t>(sizeof bp, params.size()));
			biquad_state st{};
			biquad_process(st, bp, in.data(), got.data(), fmt, h.channels, h.frames);
			if (fmt == sample_format::f32) {
				const float* a = reinterpret_cast<const float*>(got.data());
				const float* b = reinterpret_cast<const float*>(ref.data());
				for (size_t i = 0; i < h.out_bytes / 4; ++i) max_abs = std::max(max_abs, std::fabs(double(a[i]) - b[i]));
				ok = max_abs < 1e-4;
			} else {
				const int16_t* a = reinterpret_cast<const int16_t*>(got.data());
				const int16_t* b = reinterpret_cast<const int16_t*>(ref.data());
				for (size_t i = 0; i < h.out_bytes / 2; ++i) max_abs = std::max(max_abs, std::fabs(double(a[i] - b[i])));
				ok = max_abs <= 1.0; // allow +/-1 LSB fixed-point rounding
			}
			break;
		}
		case effect_id::i3dl2: {
			i3dl2_params ip{}; std::memcpy(&ip, params.data(), std::min<size_t>(sizeof ip, params.size()));
			i3dl2_state st{};
			i3dl2_process(st, ip, in.data(), got.data(), fmt, h.channels, h.frames, h.sample_rate);
			if (fmt == sample_format::f32) {
				const float* a = reinterpret_cast<const float*>(got.data());
				const float* b = reinterpret_cast<const float*>(ref.data());
				double se = 0, sr = 0;
				for (size_t i = 0; i < h.out_bytes / 4; ++i) { double d = double(a[i]) - b[i]; se += d * d; sr += double(b[i]) * b[i]; }
				double snr = sr > 0 ? 10 * std::log10(sr / (se + 1e-30)) : 0;
				max_abs = snr;
				ok = snr > 40.0; // perceptual: >40 dB SNR vs reference
			}
			break;
		}
		default:
			std::printf("  record %d: effect %u not handled\n", rec, h.effect);
			ok = false;
		}

		std::printf("  %s  record %d effect=%u %s=%.6g\n", ok ? "PASS" : "FAIL",
		            rec, h.effect,
		            effect_id(h.effect) == effect_id::i3dl2 ? "snr_dB" : "max_abs_err",
		            max_abs);
		if (!ok) ++g_fail;
		++rec;
	}
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	std::string cmd = argc > 1 ? argv[1] : "selftest";
	if (cmd == "selftest") {
		std::printf("== SPU DSP HLE oracle self-test ==\n");
		test_meter();
		test_filters();
		test_reverb();
		std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
		return g_fail ? 1 : 0;
	}
	if (cmd == "gen" && argc > 2) return cmd_gen(argv[2]);
	if (cmd == "replay" && argc > 2) {
		int rc = cmd_replay(argv[2]);
		std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
		return (rc || g_fail) ? 1 : 0;
	}
	std::printf("usage: %s [selftest | gen <file> | replay <file>]\n", argv[0]);
	return 2;
}
