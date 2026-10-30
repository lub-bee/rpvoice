// voice_chain.h - real-time voice changer DSP chain (header-only, allocation-free at runtime).
//
// C++ port of the validated Python prototype (../rt.py Chain('fast') + ../lab.py PRESETS/mix),
// first shipped in wintest/ and validated on the Windows PC. The RPVoice app extends it with
// editable parameters and click-free smoothing; with default "advanced" values and static
// parameters the output is bit-identical to wintest (checked by --selftest).
//   pre:   Gain +8 dB -> first-order high-pass 70 Hz (hpf)
//   shift: Signalsmith Stretch, block 50 ms / interval 12 ms ("fast"), formant with pitch compensation
//   sub:   second shifter one octave lower, first-order low-pass 1200 Hz (subCutoff), mixed at 0.5 * sub
//   rasp:  amplitude roughness from linearly-interpolated Gaussian noise (70 Hz, raspRate)
//   purr:  ~25 Hz (purrRate) irregular amplitude modulation
//   drive: gentle parallel tanh saturation
//   color: low shelf 150 Hz, peak 350 Hz, peak 3 kHz, high shelf 8 kHz (RBJ / JUCE formulas),
//          JUCE-style chorus (chorusRate/chorusDepth), JUCE Reverb (Freeverb, reverbRoom;
//          note: JUCE applies dry gain x2)
//   post:  JUCE compressor (-18 dB, 2:1, 10/120 ms) -> block-wise peak limiter (0.7) -> clip 0.98
// The "breath" stage is intentionally NOT ported (it sounded harsh).
//
// Smoothing: setParams() only stores a target. Every processed block (<= 256 samples) moves the
// current values toward the target (one-pole, ~30 ms); gain-like values are then linearly
// interpolated per sample inside the block, filter coefficients / shifter settings are updated
// once per block. Bypass cross-fades (10 ms) between the processed and the dry signal.
//
// Threading: construct + prepare() on a non-audio thread (allocates). process(), setParams() and
// setPreset() never allocate; call setParams() from the audio thread.
#pragma once

#include "signalsmith-stretch/signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace voice {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- parameters
struct Params {
	// Character parameters (scaled by the intensity, see mix()).
	float pitch = 0, formant = 1, sub = 0, rasp = 0, purr = 0;
	float low = 0, lowmid = 0, presence = 0, air = 0, drive = 0, chorus = 0, reverb = 0;
	// Shape parameters (constants of the original chain; NOT scaled by the intensity).
	float purrRate = 25;      // Hz, purr modulation rate
	float subCutoff = 1200;   // Hz, low-pass on the sub layer
	float raspRate = 70;      // Hz, rate of the roughness noise points
	float chorusRate = 0.6f;  // Hz, chorus LFO
	float chorusDepth = 0.15f;
	float reverbRoom = 0.5f;  // Freeverb room size 0..1
	float hpf = 70;           // Hz, input high-pass
};

// Metadata for generic code (JSON, smoothing, UI). `neutral` is the value that disables the stage.
struct ParamInfo {
	const char *key;  // JSON key
	float Params::*ptr;
	float min, max, neutral;
	bool scaled;      // interpolated by mix() from neutral to the preset value
};

inline const ParamInfo kParamInfo[] = {
	{"pitch", &Params::pitch, -12, 12, 0, true},
	{"formant", &Params::formant, 0.70f, 1.40f, 1, true},
	{"sub", &Params::sub, 0, 1, 0, true},
	{"rasp", &Params::rasp, 0, 1, 0, true},
	{"purr", &Params::purr, 0, 1, 0, true},
	{"low", &Params::low, -12, 12, 0, true},
	{"lowmid", &Params::lowmid, -12, 12, 0, true},
	{"presence", &Params::presence, -12, 12, 0, true},
	{"air", &Params::air, -12, 12, 0, true},
	{"drive", &Params::drive, 0, 1, 0, true},
	{"chorus", &Params::chorus, 0, 1, 0, true},
	{"reverb", &Params::reverb, 0, 1, 0, true},
	{"purrRate", &Params::purrRate, 10, 40, 25, false},
	{"subCutoff", &Params::subCutoff, 300, 4000, 1200, false},
	{"raspRate", &Params::raspRate, 20, 200, 70, false},
	{"chorusRate", &Params::chorusRate, 0.1f, 3, 0.6f, false},
	{"chorusDepth", &Params::chorusDepth, 0, 0.5f, 0.15f, false},
	{"reverbRoom", &Params::reverbRoom, 0, 1, 0.5f, false},
	{"hpf", &Params::hpf, 20, 200, 70, false},
};
constexpr int kNumParams = int(sizeof(kParamInfo) / sizeof(kParamInfo[0]));

inline int findParam(const char *key) {
	for (int i = 0; i < kNumParams; ++i) {
		const char *a = kParamInfo[i].key, *b = key;
		while (*a && *a == *b) ++a, ++b;
		if (!*a && !*b) return i;
	}
	return -1;
}

// ---------------------------------------------------------------- presets
struct Preset {
	const char *name;
	Params p;
};

// Same values as lab.py PRESETS ("breath" dropped). Order = keys 1..9. Shape params = defaults.
//                                    pitch formant sub   rasp  purr  low  lowmid pres  air  drive chorus reverb
inline const Preset kPresets[] = {
	{"khajiit-marchand",  {-1.0f, 0.90f, 0.10f, 0.15f, 0.35f, 2.0f, 3.0f, -1.0f, -2.0f, 0.25f, 0.15f, 0.0f}},
	{"khajiit-guerrier",  {-2.5f, 0.86f, 0.35f, 0.45f, 0.25f, 3.0f, 3.5f, 1.5f, -3.0f, 0.55f, 0.10f, 0.0f}},
	{"khajiit-feminin-a", {4.0f, 1.10f, 0.00f, 0.05f, 0.30f, -3.0f, -1.0f, 1.5f, 2.0f, 0.10f, 0.15f, 0.0f}},
	{"khajiit-feminin-b", {5.5f, 1.14f, 0.00f, 0.05f, 0.30f, -4.0f, -1.5f, 2.0f, 2.5f, 0.10f, 0.15f, 0.0f}},
	{"khajiit-feminin-c", {7.0f, 1.18f, 0.00f, 0.05f, 0.30f, -5.0f, -2.0f, 2.0f, 3.0f, 0.10f, 0.15f, 0.0f}},
	{"nord",              {-2.0f, 0.90f, 0.15f, 0.10f, 0.00f, 3.0f, 2.0f, 2.0f, -1.0f, 0.35f, 0.10f, 0.10f}},
	{"orque",             {-4.0f, 0.84f, 0.40f, 0.50f, 0.00f, 4.0f, 4.0f, 2.0f, -4.0f, 0.70f, 0.00f, 0.0f}},
	{"argonien",          {0.5f, 1.05f, 0.05f, 0.30f, 0.00f, -2.0f, -1.0f, 2.5f, 4.0f, 0.30f, 0.25f, 0.0f}},
	{"draugr",            {-5.0f, 0.82f, 0.30f, 0.60f, 0.00f, 2.0f, 0.0f, 0.0f, -2.0f, 0.50f, 0.40f, 0.45f}},
};
constexpr int kNumPresets = int(sizeof(kPresets) / sizeof(kPresets[0]));

inline int findPreset(const char *name) {
	for (int i = 0; i < kNumPresets; ++i) {
		const char *a = kPresets[i].name, *b = name;
		while (*a && *a == *b) ++a, ++b;
		if (!*a && !*b) return i;
	}
	return -1;
}

// lab.mix(): linear from neutral, formant interpolated geometrically. Shape params pass through.
inline Params mix(const Params &t, float k) {
	auto lin = [k](float v) { return v * k; };  // neutral is 0 for every linear parameter
	Params p = t;
	p.pitch = lin(t.pitch);
	p.formant = std::pow(t.formant, k);
	p.sub = lin(t.sub); p.rasp = lin(t.rasp); p.purr = lin(t.purr);
	p.low = lin(t.low); p.lowmid = lin(t.lowmid); p.presence = lin(t.presence); p.air = lin(t.air);
	p.drive = lin(t.drive); p.chorus = lin(t.chorus); p.reverb = lin(t.reverb);
	return p;
}

// ---------------------------------------------------------------- building blocks
struct OnePole {  // JUCE makeFirstOrderHighPass / LowPass, TDF2
	double b0 = 1, b1 = 0, a1 = 0, s = 0;
	void highpass(double f, double sr) {
		double n = std::tan(kPi * f / sr);
		b0 = 1 / (1 + n); b1 = -b0; a1 = (n - 1) / (n + 1);
	}
	void lowpass(double f, double sr) {
		double n = std::tan(kPi * f / sr);
		b0 = n / (1 + n); b1 = b0; a1 = (n - 1) / (n + 1);
	}
	inline float operator()(float x) {
		double y = b0 * x + s;
		s = b1 * x - a1 * y;
		return float(y);
	}
};

struct Biquad {  // RBJ cookbook == JUCE IIR::Coefficients, TDF2
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
	void set(double B0, double B1, double B2, double A0, double A1, double A2) {
		b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
	}
	void lowShelf(double sr, double f, double q, double db) {
		double A = std::sqrt(std::pow(10.0, db / 20)), am = A - 1, ap = A + 1;
		double w = 2 * kPi * f / sr, c = std::cos(w), beta = std::sin(w) * std::sqrt(A) / q;
		set(A * (ap - am * c + beta), A * 2 * (am - ap * c), A * (ap - am * c - beta),
		    ap + am * c + beta, -2 * (am + ap * c), ap + am * c - beta);
	}
	void highShelf(double sr, double f, double q, double db) {
		double A = std::sqrt(std::pow(10.0, db / 20)), am = A - 1, ap = A + 1;
		double w = 2 * kPi * f / sr, c = std::cos(w), beta = std::sin(w) * std::sqrt(A) / q;
		set(A * (ap + am * c + beta), A * -2 * (am + ap * c), A * (ap + am * c - beta),
		    ap - am * c + beta, 2 * (am - ap * c), ap - am * c - beta);
	}
	void peak(double sr, double f, double q, double db) {
		double A = std::sqrt(std::pow(10.0, db / 20));
		double w = 2 * kPi * f / sr, c = std::cos(w), alpha = std::sin(w) / (2 * q);
		set(1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
	}
	inline float operator()(float x) {
		double y = b0 * x + s1;
		s1 = b1 * x - a1 * y + s2;
		s2 = b2 * x - a2 * y;
		return float(y);
	}
};

struct Compressor {  // juce::dsp::Compressor (peak ballistics)
	double thr = 0, ratioInvM1 = 0, cteA = 0, cteR = 0, env = 0;
	void prepare(double sr, double thrDb, double ratio, double attMs, double relMs) {
		thr = std::pow(10.0, thrDb / 20);
		ratioInvM1 = 1 / ratio - 1;
		double e = -2 * kPi * 1000 / sr;
		cteA = std::exp(e / attMs);
		cteR = std::exp(e / relMs);
	}
	inline float operator()(float x) {
		double v = std::fabs(x), c = v > env ? cteA : cteR;
		env = v + c * (env - v);
		double g = env < thr ? 1.0 : std::pow(env / thr, ratioInvM1);
		return float(g * x);
	}
};

struct Chorus {  // juce::dsp::Chorus, mono, feedback 0, linear-interpolated delay, linear dry/wet
	std::vector<float> buf;
	int w = 0;
	double sr = 48000, phase = 0, rate = 0.6, depth = 0.15, centreMs = 12, mix = 0;
	void prepare(double s) {
		sr = s;
		buf.assign(size_t(sr * 0.1) + 4, 0.f);  // 100 ms, plenty for centre + modulation
	}
	inline float operator()(float x) {
		buf[w] = x;
		double dMs = centreMs - 20.0 * 0.5 * depth * std::sin(phase);  // maxDelayModulation 20 ms
		phase += 2 * kPi * rate / sr;
		if (phase > 2 * kPi) phase -= 2 * kPi;
		double d = std::max(1.0, dMs) * sr / 1000;
		int n = int(buf.size());
		int di = int(d);
		double f = d - di;
		int i0 = w - di, i1 = i0 - 1;
		if (i0 < 0) i0 += n;
		if (i1 < 0) i1 += n;
		double wet = buf[i0] * (1 - f) + buf[i1] * f;
		if (++w >= n) w = 0;
		return float(x * (1 - mix) + wet * mix);
	}
};

struct Reverb {  // juce::Reverb::processMono (Freeverb), damping 0.5, width 1
	static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
	static constexpr int kAll[4] = {556, 441, 341, 225};
	std::vector<float> comb[8], all[4];
	int ci[8] = {}, ai[4] = {};
	float last[8] = {};
	float fb = 0.84f, damp = 0.2f, wet = 0, dry = 2;
	void prepare(double sr) {
		for (int k = 0; k < 8; ++k) comb[k].assign(size_t(kComb[k] * sr / 44100), 0.f), ci[k] = 0, last[k] = 0;
		for (int k = 0; k < 4; ++k) all[k].assign(size_t(kAll[k] * sr / 44100), 0.f), ai[k] = 0;
	}
	void setParams(float roomSize, float wetLevel, float dryLevel) {
		fb = roomSize * 0.28f + 0.7f;
		damp = 0.5f * 0.4f;
		wet = wetLevel * 3.0f;  // wetScaleFactor 3, width 1 -> wetGain1 = wet
		dry = dryLevel * 2.0f;  // dryScaleFactor 2 (pedalboard Reverb(dry=1) doubles the signal)
	}
	inline float operator()(float x) {
		float in = x * 0.015f, o = 0;
		if (wet != 0) {
			for (int k = 0; k < 8; ++k) {
				float v = comb[k][ci[k]];
				last[k] = v * (1 - damp) + last[k] * damp;
				comb[k][ci[k]] = in + last[k] * fb;
				if (++ci[k] >= int(comb[k].size())) ci[k] = 0;
				o += v;
			}
			for (int k = 0; k < 4; ++k) {
				float bv = all[k][ai[k]];
				all[k][ai[k]] = o + bv * 0.5f;
				if (++ai[k] >= int(all[k].size())) ai[k] = 0;
				o = bv - o;
			}
		}
		return o * wet + x * dry;
	}
};

struct SmoothNoise {  // rt.SmoothNoise: Gaussian points joined linearly
	uint64_t st = 0x9E3779B97F4A7C15ull;
	int step = 1, pos = 0;
	float a = 0, b = 0;
	bool haveSpare = false;
	float spare = 0;
	inline double uniform() {  // xorshift64*, (0,1]
		st ^= st >> 12; st ^= st << 25; st ^= st >> 27;
		return double((st * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0) + 1e-300;
	}
	inline float gauss() {
		if (haveSpare) { haveSpare = false; return spare; }
		double r = std::sqrt(-2 * std::log(uniform())), t = 2 * kPi * uniform();
		spare = float(r * std::sin(t)); haveSpare = true;
		return float(r * std::cos(t));
	}
	void prepare(double sr, double rateHz, uint64_t seed) {
		st = seed ? seed : 1; step = int(sr / rateHz); pos = 0; a = 0; b = gauss();
	}
	inline float operator()() {
		if (pos >= step) { a = b; b = gauss(); pos = 0; }
		float v = a + (b - a) * float(pos) / float(step);
		++pos;
		return v;
	}
};


// ---------------------------------------------------------------- the chain
class Chain {
public:
	using Stretch = signalsmith::stretch::SignalsmithStretch<float>;
	static constexpr int kMaxBlock = 256;       // process() is fed in chunks of at most this size
	static constexpr double kSmoothSec = 0.03;  // parameter smoothing time constant
	static constexpr double kBypassSec = 0.01;  // bypass cross-fade duration

	// Allocates: call outside the audio thread.
	void prepare(double sampleRate, double blockMs = 50, double intervalMs = 12) {
		sr = sampleRate;
		for (Stretch *s : {&main_, &sub_}) s->configure(1, int(sr * blockMs / 1000), int(sr * intervalMs / 1000));
		preHp.highpass(70, sr);
		subLp.lowpass(1200, sr);
		comp.prepare(sr, -18, 2, 10, 120);
		chorus.prepare(sr);
		chorus.rate = 0.6;
		chorus.depth = 0.15;
		reverb.prepare(sr);
		noise.prepare(sr, 70, 7);
		limG = 1; t = 0; purrOffset = 0;
		subRunning = true; subWarm = 0;
		applied = Params{};  // shape constants above == Params defaults
		appliedValid = false;
		setParams(Params{}, true, true);
	}

	// Factory preset (-1 = dry voice), applied instantly. No allocation. Kept for --selftest.
	void setPreset(int preset, float intensity) {
		bool d = preset < 0 || preset >= kNumPresets;
		setParams(d ? Params{} : mix(kPresets[preset].p, intensity), d, true);
	}

	// New target (already mixed with the intensity). bypass = dry voice. snap = jump without ramp.
	// No allocation; call from the audio thread.
	void setParams(const Params &target, bool bypass, bool snap = false) {
		tgt = target;
		dryTgt = bypass ? 1.f : 0.f;
		if (snap) {
			cur = prev = tgt;
			dryCur = dryPrev = dryTgt;
			applyBlockParams();
		}
	}

	// Signalsmith latency in samples (input + output), same for both shifters.
	int latencySamples() const { return main_.inputLatency() + main_.outputLatency(); }
	bool subRunning_() const { return subRunning; }

	// Any n; internally split into chunks of <= kMaxBlock (the limiter works per chunk).
	void process(const float *in, float *out, int n) {
		while (n > 0) {
			int m = std::min(n, kMaxBlock);
			processBlock(in, out, m);
			in += m; out += m; n -= m;
		}
	}

private:
	static float lerp(float a, float b, float f) { return a + (b - a) * f; }

	// One step of the per-block smoothing: prev = value at block start, cur = value at block end.
	void smooth(int n) {
		prev = cur;
		dryPrev = dryCur;
		float a = float(1 - std::exp(-n / (sr * kSmoothSec)));
		for (const ParamInfo &pi : kParamInfo) {
			float &c = cur.*pi.ptr, g = tgt.*pi.ptr;
			if (c == g) continue;
			c += (g - c) * a;
			if (std::fabs(g - c) <= 1e-4f * (1 + std::fabs(g))) c = g;
		}
		float step = float(n / (sr * kBypassSec));
		dryCur = dryTgt > dryCur ? std::min(dryTgt, dryCur + step) : std::max(dryTgt, dryCur - step);
	}

	// Sub shifter gating (CPU): stop it when the sub layer is silent; on re-enable, reset it and
	// keep its gain at 0 until its latency has elapsed, then the normal smoothing fades it in.
	void gateSub(int n) {
		if (!subRunning) {
			if (tgt.sub <= 0) { cur.sub = prev.sub = 0; return; }
			sub_.reset();
			subRunning = true;
			subWarm = latencySamples();
		} else if (tgt.sub <= 0 && cur.sub <= 0 && prev.sub <= 0) {
			subRunning = false;
			return;
		}
		if (subWarm > 0) {
			subWarm -= n;
			cur.sub = prev.sub = 0;
		}
	}

	// Per-block updates of coefficients / settings that are not interpolated per sample.
	void applyBlockParams() {
		const Params &c = cur;
		bool f = !appliedValid;
		if (f || c.pitch != applied.pitch) {
			main_.setTransposeSemitones(c.pitch);
			sub_.setTransposeSemitones(c.pitch - 12);
		}
		if (f || c.formant != applied.formant) {
			main_.setFormantFactor(c.formant, true);
			sub_.setFormantFactor(c.formant, true);
		}
		if (f || c.low != applied.low) ls.lowShelf(sr, 150, 0.7071067811865476, c.low);
		if (f || c.lowmid != applied.lowmid) pk1.peak(sr, 350, 0.9, c.lowmid);
		if (f || c.presence != applied.presence) pk2.peak(sr, 3000, 1.0, c.presence);
		if (f || c.air != applied.air) hs.highShelf(sr, 8000, 0.7071067811865476, c.air);
		// Shape constants: only touched when they move away from what prepare() set.
		if (c.hpf != applied.hpf) preHp.highpass(c.hpf, sr);
		if (c.subCutoff != applied.subCutoff) subLp.lowpass(c.subCutoff, sr);
		if (c.raspRate != applied.raspRate) noise.step = std::max(1, int(sr / std::max(1.f, c.raspRate)));
		if (c.chorusRate != applied.chorusRate) chorus.rate = c.chorusRate;
		if (c.chorusDepth != applied.chorusDepth) chorus.depth = c.chorusDepth;
		if (c.reverbRoom != applied.reverbRoom) reverb.fb = c.reverbRoom * 0.28f + 0.7f;
		if (c.purrRate != applied.purrRate) {
			// Keep the purr phase continuous: 2pi*r0*t + off0 == 2pi*r1*t + off1.
			double tt = double(t) / sr;
			purrOffset = std::fmod(purrOffset + 2 * kPi * (double(applied.purrRate) - double(c.purrRate)) * tt, 2 * kPi);
		}
		applied = c;
		appliedValid = true;
	}

	void processBlock(const float *in, float *out, int n) {
		smooth(n);
		gateSub(n);
		applyBlockParams();

		for (int i = 0; i < n; ++i) x[i] = preHp(in[i] * 2.5118864315095801f);  // +8 dB, HPF 70 Hz
		float *xi = x, *yo = y, *so = s;
		main_.process(&xi, n, &yo, n);
		if (subRunning) sub_.process(&xi, n, &so, n);
		if (dryPrev >= 1 && dryCur >= 1) {  // fully bypassed
			post(x, out, n);
			return;
		}
		const float invN = 1.f / float(n);
		const double purrW = 2 * kPi * double(cur.purrRate);
		for (int i = 0; i < n; ++i) {
			float fi = float(i) * invN;
			float sub = lerp(prev.sub, cur.sub, fi), rasp = lerp(prev.rasp, cur.rasp, fi);
			float purr = lerp(prev.purr, cur.purr, fi), drive = lerp(prev.drive, cur.drive, fi);
			float v = y[i];
			if (sub > 0) v += 0.5f * sub * subLp(s[i]);
			if (rasp > 0) v *= 1 + 0.12f * rasp * std::clamp(noise(), -2.f, 2.f);
			double tt = double(t + i) / sr;
			if (purr > 0) {
				double wob = std::sin(purrW * tt + purrOffset + 2 * std::sin(2 * kPi * 0.7 * tt));
				v = float(v * (1 - 0.2 * purr + 0.2 * purr * wob));
			}
			if (drive > 0) {
				float k = 1 + 2 * drive;
				v = (1 - 0.3f * drive) * v + 0.3f * drive * std::tanh(k * v) / k;
			}
			v = hs(pk2(pk1(ls(v))));
			chorus.mix = 0.5 * lerp(prev.chorus, cur.chorus, fi);
			reverb.wet = 0.35f * lerp(prev.reverb, cur.reverb, fi) * 3.0f;
			v = reverb(chorus(v));
			float d = lerp(dryPrev, dryCur, fi);
			y[i] = d > 0 ? v * (1 - d) + x[i] * d : v;
		}
		t += n;
		post(y, out, n);
	}

	void post(float *v, float *out, int n) {
		float pk = 0;
		for (int i = 0; i < n; ++i) {
			v[i] = comp(v[i]);
			pk = std::max(pk, std::fabs(v[i]));
		}
		// Block-wise peak limiter: instant attack, slow release (x1.02 per block), linear ramp.
		float target = std::min(1.f, 0.7f / (pk + 1e-9f));
		float g = target < limG ? target : std::min(target, limG * 1.02f);
		for (int i = 0; i < n; ++i) {
			float gi = n > 1 ? limG + (g - limG) * float(i) / float(n - 1) : g;
			out[i] = std::clamp(v[i] * gi, -0.98f, 0.98f);
		}
		limG = g;
	}

	double sr = 48000;
	Stretch main_, sub_;
	OnePole preHp, subLp;
	Biquad ls, pk1, pk2, hs;
	Chorus chorus;
	Reverb reverb;
	Compressor comp;
	SmoothNoise noise;
	Params tgt, cur, prev, applied;
	bool appliedValid = false;
	float dryTgt = 1, dryCur = 1, dryPrev = 1;
	bool subRunning = true;
	int subWarm = 0;
	double purrOffset = 0;
	float limG = 1;
	int64_t t = 0;
	float x[kMaxBlock], y[kMaxBlock], s[kMaxBlock];
};

}  // namespace voice
