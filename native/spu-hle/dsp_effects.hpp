// SPU DSP HLE — host C++ effect kernels.
//
// These are the replacements for DOA5's mstream_dsp_* SPU plugins. They are
// pure functions over interleaved PCM: no RPCS3, Android, or SPU dependency, so
// the identical code is exercised by the standalone oracle harness
// (native/spu-hle/oracle) and included by the emulator-side interception
// (patch 0021, cellSpursSpu.cpp). Keeping one implementation means the thing the
// oracle verifies is byte-for-byte the thing that runs in the game.
//
// Correctness strategy (see native/spu-hle/README.md):
//   * meter   — peak/RMS with the decay coefficient read from the disassembly.
//   * filter  — RBJ-cookbook biquad, transposed direct form II. The oracle
//               checks its impulse response against the analytic transfer
//               function |H(e^jw)|, an independent closed-form reference.
//   * para_eq — cascade of peaking biquads; same verification.
//   * i3dl2   — Schroeder/Freeverb comb+allpass network whose per-comb feedback
//               is derived from the I3DL2 decay time (RT60), so its measured
//               RT60 is verifiable against the requested decay time.
//
// Formats: effects run in float internally. int16 blocks are converted at the
// edges. State (filter histories, reverb delay lines) persists across blocks in
// the *_state objects, matching the SPU plugins which keep state in LS.

#ifndef SPU_HLE_DSP_EFFECTS_HPP
#define SPU_HLE_DSP_EFFECTS_HPP

#include "dsp_capture.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace spu_hle {

// Self-contained pi so this header does not depend on M_PI (not defined by the
// C++ standard; absent under MSVC without _USE_MATH_DEFINES). This header is
// included into the cross-platform emulator core, so keep it portable.
static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline float s16_to_f32(int16_t s) { return s * (1.0f / 32768.0f); }

inline int16_t f32_to_s16(float x) {
	float v = x * 32768.0f;
	if (v > 32767.0f) v = 32767.0f;
	if (v < -32768.0f) v = -32768.0f;
	// round-to-nearest, ties away from zero — matches typical fixed-point clip
	return static_cast<int16_t>(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

// De-interleave an input block (any supported format) into per-channel float.
inline void deinterleave(const void* in, sample_format fmt, int channels,
                         int frames, std::vector<std::vector<float>>& out) {
	out.assign(channels, std::vector<float>(frames));
	if (fmt == sample_format::f32) {
		const float* p = static_cast<const float*>(in);
		for (int f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				out[c][f] = p[f * channels + c];
	} else {
		const int16_t* p = static_cast<const int16_t*>(in);
		for (int f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				out[c][f] = s16_to_f32(p[f * channels + c]);
	}
}

inline void interleave(const std::vector<std::vector<float>>& in,
                       sample_format fmt, void* out) {
	const int channels = static_cast<int>(in.size());
	const int frames = channels ? static_cast<int>(in[0].size()) : 0;
	if (fmt == sample_format::f32) {
		float* p = static_cast<float*>(out);
		for (int f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				p[f * channels + c] = in[c][f];
	} else {
		int16_t* p = static_cast<int16_t*>(out);
		for (int f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				p[f * channels + c] = f32_to_s16(in[c][f]);
	}
}

// ---------------------------------------------------------------------------
// Meter (SPU-b5522754)
// ---------------------------------------------------------------------------
//
// A metering plugin does not alter the audio; it measures it and publishes the
// levels. This computes, per channel and per block: the block peak magnitude and
// the block RMS, then applies an exponential peak-hold decay so the reported
// peak falls smoothly between transients (the classic PPM ballistic). The decay
// constant is the one baked into the SPU code.

struct meter_result {
	int   channels = 0;
	float peak[kMaxChannels] = {}; // held peak magnitude, linear 0..~1
	float rms[kMaxChannels]  = {}; // block RMS, linear
};

struct meter_state {
	float held_peak[kMaxChannels] = {};
};

// The peak-hold decay coefficient the SPU meter uses, recovered from its
// disassembly: ilhu 0x3f72 / iohl 0xf0d4 => 0x3f72f0d4 == 0.948987f.
static constexpr float kMeterDecay = 0.948987f;

inline meter_result meter_process(meter_state& st, const void* in,
                                  sample_format fmt, int channels, int frames,
                                  const meter_params& p) {
	std::vector<std::vector<float>> ch;
	deinterleave(in, fmt, channels, frames, ch);

	meter_result r;
	r.channels = channels;
	const float decay = (p.decay > 0.0f) ? p.decay : 0.0f;

	for (int c = 0; c < channels && c < kMaxChannels; ++c) {
		float peak = 0.0f;
		double sumsq = 0.0;
		for (int f = 0; f < frames; ++f) {
			const float x = ch[c][f];
			const float a = std::fabs(x);
			if (a > peak) peak = a;
			sumsq += double(x) * double(x);
		}
		// Exponential peak hold: decay the previous peak, then latch a higher one.
		float held = st.held_peak[c] * decay;
		if (peak > held) held = peak;
		st.held_peak[c] = held;

		r.peak[c] = held;
		r.rms[c]  = frames ? static_cast<float>(std::sqrt(sumsq / frames)) : 0.0f;
	}
	return r;
}

// ---------------------------------------------------------------------------
// Biquad (filter SPU-2379c584 / para_eq SPU-e433af6d)
// ---------------------------------------------------------------------------

// RBJ "Audio EQ Cookbook" coefficient generators. Returned coeffs are already
// normalized by a0 (so a0 == 1 implicitly).
inline biquad_coeffs biquad_lowpass(double fs, double f0, double q) {
	const double w0 = 2.0 * kPi * f0 / fs;
	const double c = std::cos(w0), s = std::sin(w0);
	const double alpha = s / (2.0 * q);
	const double a0 = 1.0 + alpha;
	biquad_coeffs k;
	k.b0 = float(((1.0 - c) / 2.0) / a0);
	k.b1 = float((1.0 - c) / a0);
	k.b2 = float(((1.0 - c) / 2.0) / a0);
	k.a1 = float((-2.0 * c) / a0);
	k.a2 = float((1.0 - alpha) / a0);
	return k;
}

inline biquad_coeffs biquad_highpass(double fs, double f0, double q) {
	const double w0 = 2.0 * kPi * f0 / fs;
	const double c = std::cos(w0), s = std::sin(w0);
	const double alpha = s / (2.0 * q);
	const double a0 = 1.0 + alpha;
	biquad_coeffs k;
	k.b0 = float(((1.0 + c) / 2.0) / a0);
	k.b1 = float((-(1.0 + c)) / a0);
	k.b2 = float(((1.0 + c) / 2.0) / a0);
	k.a1 = float((-2.0 * c) / a0);
	k.a2 = float((1.0 - alpha) / a0);
	return k;
}

// Peaking EQ: gain_db of boost/cut at f0 with bandwidth set by q.
inline biquad_coeffs biquad_peaking(double fs, double f0, double q, double gain_db) {
	const double A = std::pow(10.0, gain_db / 40.0);
	const double w0 = 2.0 * kPi * f0 / fs;
	const double c = std::cos(w0), s = std::sin(w0);
	const double alpha = s / (2.0 * q);
	const double a0 = 1.0 + alpha / A;
	biquad_coeffs k;
	k.b0 = float((1.0 + alpha * A) / a0);
	k.b1 = float((-2.0 * c) / a0);
	k.b2 = float((1.0 - alpha * A) / a0);
	k.a1 = float((-2.0 * c) / a0);
	k.a2 = float((1.0 - alpha / A) / a0);
	return k;
}

// Analytic magnitude response |H(e^jw)| at frequency f (Hz) for a normalized
// biquad. Independent closed-form reference the oracle checks the time-domain
// filter against.
inline double biquad_response(const biquad_coeffs& k, double fs, double f) {
	const double w = 2.0 * kPi * f / fs;
	const double cw = std::cos(w), sw = std::sin(w);
	const double c2w = std::cos(2 * w), s2w = std::sin(2 * w);
	const double num_re = k.b0 + k.b1 * cw + k.b2 * c2w;
	const double num_im = -(k.b1 * sw + k.b2 * s2w);
	const double den_re = 1.0 + k.a1 * cw + k.a2 * c2w;
	const double den_im = -(k.a1 * sw + k.a2 * s2w);
	const double num = std::sqrt(num_re * num_re + num_im * num_im);
	const double den = std::sqrt(den_re * den_re + den_im * den_im);
	return num / den;
}

// One biquad in transposed direct form II. Per-channel state (z1,z2).
struct biquad_ch_state { float z1 = 0.0f, z2 = 0.0f; };

inline float biquad_tick(const biquad_coeffs& k, biquad_ch_state& s, float x) {
	const float y = k.b0 * x + s.z1;
	s.z1 = k.b1 * x - k.a1 * y + s.z2;
	s.z2 = k.b2 * x - k.a2 * y;
	return y;
}

struct biquad_state {
	// [stage][channel]
	biquad_ch_state z[kMaxStages][kMaxChannels];
};

inline void biquad_process(biquad_state& st, const biquad_params& p,
                           const void* in, void* out, sample_format fmt,
                           int channels, int frames) {
	std::vector<std::vector<float>> ch;
	deinterleave(in, fmt, channels, frames, ch);

	for (uint32_t stg = 0; stg < p.stages && stg < kMaxStages; ++stg) {
		const biquad_coeffs& k = p.coeffs[stg];
		for (int c = 0; c < channels && c < kMaxChannels; ++c) {
			biquad_ch_state& s = st.z[stg][c];
			for (int f = 0; f < frames; ++f)
				ch[c][f] = biquad_tick(k, s, ch[c][f]);
		}
	}
	interleave(ch, fmt, out);
}

// ---------------------------------------------------------------------------
// I3DL2 reverb (SPU-6783990e)
// ---------------------------------------------------------------------------
//
// A Schroeder/Freeverb reverberator: a parallel bank of feedback-comb filters
// (with a one-pole damping filter in each feedback path) summed into a series
// of allpass diffusers, per stereo channel. Rather than Freeverb's fixed
// "roomsize" knob, each comb's feedback gain is derived from the requested
// I3DL2 decay time so the tail's RT60 matches the parameter — the property the
// oracle measures.

struct comb_filter {
	std::vector<float> buf;
	int   idx = 0;
	float store = 0.0f;   // one-pole damping state
	float feedback = 0.0f;
	float damp1 = 0.0f, damp2 = 1.0f;

	void set_size(int n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; store = 0.0f; }
	void set_damp(float d) { damp1 = d; damp2 = 1.0f - d; }
	float process(float x) {
		float y = buf[idx];
		store = y * damp2 + store * damp1;      // low-pass the feedback
		buf[idx] = x + store * feedback;
		if (++idx >= static_cast<int>(buf.size())) idx = 0;
		return y;
	}
};

struct allpass_filter {
	std::vector<float> buf;
	int   idx = 0;
	float feedback = 0.5f;

	void set_size(int n) { buf.assign(n > 0 ? n : 1, 0.0f); idx = 0; }
	float process(float x) {
		float b = buf[idx];
		float y = -x + b;
		buf[idx] = x + b * feedback;
		if (++idx >= static_cast<int>(buf.size())) idx = 0;
		return y;
	}
};

static constexpr int kNumCombs = 8;
static constexpr int kNumAllpass = 4;

struct reverb_channel {
	comb_filter    comb[kNumCombs];
	allpass_filter allp[kNumAllpass];
};

struct i3dl2_state {
	reverb_channel ch[2];
	bool  configured = false;
	float wet = 0.0f, dry = 1.0f;
};

// Base Freeverb tunings (samples @ 44100), scaled to the actual rate. The right
// channel adds a small stereo spread for decorrelation.
static constexpr int kCombTune[kNumCombs]   = { 1116,1188,1277,1356,1422,1491,1557,1617 };
static constexpr int kAllpTune[kNumAllpass] = { 556, 441, 341, 225 };
static constexpr int kStereoSpread = 23;

inline float mb_to_linear(double mb) { return static_cast<float>(std::pow(10.0, mb / 2000.0)); }

inline void i3dl2_configure(i3dl2_state& st, const i3dl2_params& p, int sample_rate) {
	const double fs = sample_rate > 0 ? sample_rate : 48000;
	const double scale = fs / 44100.0;
	const double decay = p.decay_time > 0.05f ? p.decay_time : 0.05f;

	// Damping from decay_hf_ratio: ratio<1 => high frequencies decay faster.
	float damp = 0.0f;
	if (p.decay_hf_ratio > 0.0f && p.decay_hf_ratio < 1.0f)
		damp = static_cast<float>((1.0 - p.decay_hf_ratio) * 0.4);
	if (damp < 0.0f) damp = 0.0f;
	if (damp > 0.95f) damp = 0.95f;

	for (int c = 0; c < 2; ++c) {
		for (int i = 0; i < kNumCombs; ++i) {
			int n = static_cast<int>(kCombTune[i] * scale) + (c ? kStereoSpread : 0);
			st.ch[c].comb[i].set_size(n);
			// Schroeder comb decay: g such that the comb decays 60 dB over RT60.
			const double delay_s = double(n) / fs;
			double g = std::pow(10.0, -3.0 * delay_s / decay); // == 10^(-3 * D/RT60)
			if (g > 0.999) g = 0.999;
			st.ch[c].comb[i].feedback = static_cast<float>(g);
			st.ch[c].comb[i].set_damp(damp);
		}
		for (int i = 0; i < kNumAllpass; ++i) {
			int n = static_cast<int>(kAllpTune[i] * scale) + (c ? kStereoSpread : 0);
			st.ch[c].allp[i].set_size(n);
			// Diffusion controls the allpass coefficient (0.5 nominal Freeverb).
			float diff = 0.5f + (p.diffusion / 100.0f - 0.5f) * 0.4f;
			if (diff < 0.05f) diff = 0.05f;
			if (diff > 0.9f) diff = 0.9f;
			st.ch[c].allp[i].feedback = diff;
		}
	}

	// Wet/dry from the late-reverb and room levels (mB -> linear).
	st.wet = mb_to_linear(p.reverb) * mb_to_linear(p.room);
	if (st.wet > 1.0f) st.wet = 1.0f;
	st.dry = mb_to_linear(p.dry_level);
	if (st.dry > 1.0f) st.dry = 1.0f;
	st.configured = true;
}

inline void i3dl2_process(i3dl2_state& st, const i3dl2_params& p, const void* in,
                          void* out, sample_format fmt, int channels, int frames,
                          int sample_rate) {
	if (!st.configured) i3dl2_configure(st, p, sample_rate);

	std::vector<std::vector<float>> ch;
	deinterleave(in, fmt, channels, frames, ch);
	std::vector<std::vector<float>> outc(channels, std::vector<float>(frames));

	const int lch = 0;
	const int rch = channels > 1 ? 1 : 0;

	for (int f = 0; f < frames; ++f) {
		const float inL = ch[lch][f];
		const float inR = ch[rch][f];
		const float mono = (inL + inR) * 0.5f;

		float wetL = 0.0f, wetR = 0.0f;
		for (int i = 0; i < kNumCombs; ++i) {
			wetL += st.ch[0].comb[i].process(mono);
			wetR += st.ch[1].comb[i].process(mono);
		}
		for (int i = 0; i < kNumAllpass; ++i) {
			wetL = st.ch[0].allp[i].process(wetL);
			wetR = st.ch[1].allp[i].process(wetR);
		}
		const float normalize = 1.0f / kNumCombs;
		outc[lch][f] = inL * st.dry + wetL * normalize * st.wet;
		if (channels > 1)
			outc[rch][f] = inR * st.dry + wetR * normalize * st.wet;
	}

	interleave(outc, fmt, out);
}

} // namespace spu_hle

#endif // SPU_HLE_DSP_EFFECTS_HPP
