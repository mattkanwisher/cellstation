// SPU DSP HLE — recorded-I/O oracle capture format.
//
// This header is the single source of truth for the binary layout that the
// on-device record hook (patch 0021, cellSpursSpu.cpp) writes and the host-side
// oracle test harness (native/spu-hle/oracle) reads. It is deliberately free of
// any RPCS3 / Android dependency so it compiles both inside the emulator core
// and in a standalone host build.
//
// A capture file is a stream of self-describing records:
//
//   [dsp_cap_file_header]
//   repeated:
//     [dsp_cap_record_header]
//     [param_bytes]   effect parameter blob (one of the *_params below)
//     [in_bytes]      the input audio block the SPU plugin read (MFC GET)
//     [out_bytes]     the output block the *real SPU plugin* wrote (MFC PUT)
//
// The oracle replays (param, input) through the host C++ effect and diffs its
// output against out_bytes. An effect is only trusted when the diff is within
// the per-effect tolerance for every record in a real device capture.
//
// Everything here is little-endian on the host side. The record hook inside the
// emulator converts the SPU's big-endian audio to host order before writing, so
// the harness never has to know PS3 endianness.

#ifndef SPU_HLE_DSP_CAPTURE_HPP
#define SPU_HLE_DSP_CAPTURE_HPP

#include <cstdint>

namespace spu_hle {

// Effect identity. Values are stable on disk — never renumber, only append.
enum class effect_id : uint32_t {
	none    = 0,
	meter   = 1, // SPU-b5522754  mstream_dsp_meter
	filter  = 2, // SPU-2379c584  mstream_dsp_filter   (biquad, single stage)
	para_eq = 3, // SPU-e433af6d  mstream_dsp_para_eq  (biquad, peaking, N stages)
	i3dl2   = 4, // SPU-6783990e  mstream_dsp_i3dl2    (I3DL2 environmental reverb)
	atrac   = 5, // SPU-702a721b  msngSPURS_ATRAC      (ATRAC3+ decode — scaffold only)
};

enum class sample_format : uint32_t {
	f32 = 0, // interleaved 32-bit float, host byte order
	s16 = 1, // interleaved signed 16-bit PCM, host byte order
};

// One capture file header. magic identifies the format+version.
struct dsp_cap_file_header {
	char     magic[8];   // "DSPCAP\0" + version byte
	uint32_t version;    // == kVersion
	uint32_t record_count; // may be 0 if the writer streamed without a final rewind
};

// Per-record header. Fixed size; variable-length blobs follow in file order.
struct dsp_cap_record_header {
	uint32_t effect;        // effect_id
	uint32_t format;        // sample_format
	uint32_t channels;      // interleave count (e.g. 2 for stereo)
	uint32_t frames;        // frames per channel in the input block
	uint32_t sample_rate;   // Hz (e.g. 48000)
	uint32_t param_bytes;   // size of the parameter blob that follows
	uint32_t in_bytes;      // size of the input audio block
	uint32_t out_bytes;     // size of the reference (real-SPU) output block
	uint32_t seq;           // monotonically increasing call index on device
	uint32_t reserved;
};

static constexpr char    kMagic[8] = { 'D','S','P','C','A','P','\0','1' };
static constexpr uint32_t kVersion = 1;

// ---- Per-effect parameter blobs (the param_bytes region) -------------------

static constexpr int kMaxChannels = 8;

// Meter: a peak/RMS level meter with exponential peak-hold decay. The decay
// constant 0.948987f (0x3f72f0d4) was read out of the meter's disassembly
// (docs/spurs/mstream_dsp_meter-disasm.txt, 0x30880/0x30888).
struct meter_params {
	float decay;   // per-block peak-hold multiplier; <=0 disables hold
};

// One biquad stage (RBJ cookbook coefficients, a0 normalized to 1).
struct biquad_coeffs {
	float b0, b1, b2, a1, a2;
};

// filter / para_eq: a cascade of up to kMaxStages biquad stages applied to
// every channel. filter uses 1 stage; para_eq chains several peaking stages.
static constexpr int kMaxStages = 8;
struct biquad_params {
	uint32_t      stages;                 // number of valid entries in coeffs[]
	biquad_coeffs coeffs[kMaxStages];
};

// i3dl2: the standard I3DL2 environmental-reverb property set (subset that the
// host reverb consumes). Units follow the I3DL2 spec: levels in millibels (mB),
// times in seconds, ratios linear.
struct i3dl2_params {
	int32_t room;             // mB, master reverb level      [-10000..0]
	int32_t room_hf;          // mB, reverb level at high freq [-10000..0]
	float   decay_time;       // s, reverb decay time          [0.1..20]
	float   decay_hf_ratio;   // ratio hf/lf decay             [0.1..2]
	int32_t reflections;      // mB, early reflection level    [-10000..1000]
	float   reflections_delay;// s
	int32_t reverb;           // mB, late reverb level         [-10000..2000]
	float   reverb_delay;     // s
	float   diffusion;        // %   [0..100]
	float   density;          // %   [0..100]
	float   hf_reference;     // Hz
	float   dry_level;        // mB->linear applied host-side; kept as mB here
	uint32_t sample_rate;     // Hz (redundant with record header; kept explicit)
};

} // namespace spu_hle

#endif // SPU_HLE_DSP_CAPTURE_HPP
