// selftest.cpp - headless verification modes (see selftest.h). Console output is English/French
// mix on purpose: these modes are for development, not for the end user.
#include "selftest.h"

#include "audio.h"
#include "miniaudio/miniaudio.h"
#include "presets.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

bool readWav(const char *path, std::vector<float> &in) {
	ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 1, audio::kSampleRate);
	ma_decoder dec;
	if (ma_decoder_init_file(path, &dc, &dec) != MA_SUCCESS) return false;
	float buf[4096];
	for (;;) {
		ma_uint64 got = 0;
		ma_decoder_read_pcm_frames(&dec, buf, 4096, &got);
		if (got == 0) break;
		in.insert(in.end(), buf, buf + got);
	}
	ma_decoder_uninit(&dec);
	return true;
}

bool writeWav(const char *path, const std::vector<float> &out) {
	ma_encoder_config ec = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, audio::kSampleRate);
	ma_encoder enc;
	if (ma_encoder_init_file(path, &ec, &enc) != MA_SUCCESS) return false;
	ma_encoder_write_pcm_frames(&enc, out.data(), out.size(), nullptr);
	ma_encoder_uninit(&enc);
	return true;
}

struct Metrics {
	double rms = 0, peak = 0, maxStep = 0;
	size_t nans = 0, jumps = 0;
};

Metrics measure(const std::vector<float> &out) {
	Metrics m;
	double ss = 0;
	for (size_t i = 0; i < out.size(); ++i) {
		float v = out[i];
		if (!std::isfinite(v)) { ++m.nans; continue; }
		ss += double(v) * v;
		m.peak = std::max(m.peak, double(std::fabs(v)));
		if (i) {
			double d = std::fabs(double(v) - out[i - 1]);
			m.maxStep = std::max(m.maxStep, d);
			if (d > 0.3) ++m.jumps;
		}
	}
	m.rms = std::sqrt(ss / double(std::max<size_t>(1, out.size())));
	return m;
}

// "0"/"original" -> bypass, factory name / 1..9, or a preset .json file.
bool resolveVoice(const char *arg, std::string &label, VoiceDef &v, bool &bypass) {
	bypass = false;
	if (!std::strcmp(arg, "0") || !std::strcmp(arg, "original")) {
		bypass = true;
		label = "original";
		return true;
	}
	int i = voice::findPreset(arg);
	char *end = nullptr;
	long n = std::strtol(arg, &end, 10);
	if (i < 0 && end && !*end && n >= 1 && n <= voice::kNumPresets) i = int(n - 1);
	if (i >= 0) {
		label = voice::kPresets[i].name;
		v = VoiceDef(voice::kPresets[i].p);
		return true;
	}
	std::string err;
	if (loadPresetFile(u8path(arg), label, v, err)) return true;
	std::fprintf(stderr, "preset inconnu: %s (%s)\n", arg, err.c_str());
	return false;
}

}  // namespace

int runSelftest(const char *inPath, const char *outPath, const char *presetArg, float intensity) {
	std::string label;
	VoiceDef v;
	bool bypass;
	if (!resolveVoice(presetArg, label, v, bypass)) return 1;
	std::vector<float> in;
	if (!readWav(inPath, in)) {
		std::fprintf(stderr, "cannot read %s\n", inPath);
		return 1;
	}
	auto chain = std::make_unique<voice::Chain>();
	chain->prepare(audio::kSampleRate);
	chain->setParams(bypass ? voice::Params{} : v.effective(intensity), bypass, true);
	std::vector<float> out(in.size());
	auto t0 = std::chrono::steady_clock::now();
	for (size_t i = 0; i < in.size(); i += audio::kPeriodFrames) {
		int n = int(std::min<size_t>(audio::kPeriodFrames, in.size() - i));
		chain->process(in.data() + i, out.data() + i, n);
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (!writeWav(outPath, out)) {
		std::fprintf(stderr, "cannot write %s\n", outPath);
		return 1;
	}
	Metrics m = measure(out);
	double dur = double(in.size()) / audio::kSampleRate;
	std::printf("selftest preset=%s intensity=%.2f samples=%zu (%.1f s)\n", label.c_str(), intensity, out.size(), dur);
	std::printf("rms=%.5f peak=%.5f nan=%zu jumps>0.3=%zu signalsmith_latency=%.1f ms cpu=%.1f%% realtime sub_running=%d\n",
	            m.rms, m.peak, m.nans, m.jumps, chain->latencySamples() * 1000.0 / audio::kSampleRate,
	            100.0 * secs / dur, int(chain->subRunning_()));
	return 0;
}

int runPresetTest() {
	int fails = 0;
	auto check = [&](bool ok, const std::string &what) {
		std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
		if (!ok) ++fails;
	};
	fs::path dir = fs::temp_directory_path() / "rpvoice-preset-test";
	std::error_code ec;
	fs::remove_all(dir, ec);
	fs::create_directories(dir, ec);

	for (int i = 0; i < voice::kNumPresets; ++i) {
		VoiceDef a(voice::kPresets[i].p);
		if (i == 2) { a.on[1] = false; a.value(12) = 31.5f; }  // a disabled flag + a shape param
		fs::path f = dir / (std::string(voice::kPresets[i].name) + ".json");
		std::string err, name;
		bool saved = savePresetFile(f, voice::kPresets[i].name, a, err);
		VoiceDef b;
		bool loaded = saved && loadPresetFile(f, name, b, err);
		check(loaded && b == a && name == voice::kPresets[i].name, std::string("round-trip ") + voice::kPresets[i].name + (err.empty() ? "" : " (" + err + ")"));
	}
	// Name with accents and characters that are invalid in file names.
	{
		VoiceDef a(voice::kPresets[0].p);
		a.value(0) = -3.3f;
		std::string name = "Élise à l'épée: \"forte\"";
		fs::path f = dir / u8path("Élise.json");
		std::string err, back;
		VoiceDef b;
		bool ok = savePresetFile(f, name, a, err) && loadPresetFile(f, back, b, err);
		check(ok && back == name && b == a, "round-trip accented name");
	}
	// Partial / future files.
	{
		std::string name, err;
		VoiceDef v;
		bool ok = presetFromJson(R"({"version":1,"name":"x","params":{"pitch":3,"formant":9}})", name, v, err);
		check(ok && v.p.pitch == 3 && v.p.formant == 1.40f && v.p.purrRate == 25, "partial file: defaults + clamping");
		check(!presetFromJson(R"({"version":99,"params":{}})", name, v, err), "rejects unknown version");
		check(!presetFromJson("not json", name, v, err), "rejects invalid JSON");
	}
	fs::remove_all(dir, ec);
	std::printf("%s (%d failure%s)\n", fails ? "PRESET TEST FAILED" : "PRESET TEST OK", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

// --test-smooth in.wav out.wav : automation stress test (preset switches, sub toggles, bypass,
// intensity jumps, EQ jumps) rendered with smoothing; prints the click metrics next to a
// static render of the same input so they can be compared.
int runSmoothTest(const char *inPath, const char *outPath) {
	std::vector<float> in;
	if (!readWav(inPath, in)) {
		std::fprintf(stderr, "cannot read %s\n", inPath);
		return 1;
	}
	auto run = [&](bool automate, bool snap, std::vector<float> &out) {
		auto chain = std::make_unique<voice::Chain>();
		chain->prepare(audio::kSampleRate);
		VoiceDef g(voice::kPresets[1].p);  // khajiit-guerrier (has sub)
		chain->setParams(g.effective(1), false, true);
		out.assign(in.size(), 0);
		const size_t P = audio::kPeriodFrames, sec = audio::kSampleRate;
		for (size_t i = 0, step = 0; i < in.size(); i += P) {
			if (automate && i / (sec / 2) != step) {  // every 0.5 s
				step = i / (sec / 2);
				VoiceDef v = g;
				bool bypass = false;
				float k = 1;
				switch (step % 8) {
					case 1: v.on[2] = false; break;                       // sub off (shifter stops)
					case 2: break;                                         // sub back on (warm-up)
					case 3: bypass = true; break;                          // bypass
					case 4: v = VoiceDef(voice::kPresets[4].p); break;     // feminin-c, sub 0
					case 5: k = 0.2f; break;                               // intensity jump
					case 6: v.value(5) = 12; v.value(8) = -12; v.value(13) = 3000; break;  // EQ + cutoff
					case 7: v = VoiceDef(voice::kPresets[8].p); break;     // draugr (reverb)
					default: break;
				}
				chain->setParams(bypass ? v.effective(k) : v.effective(k), bypass, snap);
			}
			int n = int(std::min<size_t>(P, in.size() - i));
			chain->process(in.data() + i, out.data() + i, n);
		}
	};
	std::vector<float> st, sm, sn;
	run(false, false, st);
	run(true, false, sm);
	run(true, true, sn);
	writeWav(outPath, sm);
	Metrics a = measure(st), b = measure(sm), c = measure(sn);
	std::printf("static    : rms=%.5f peak=%.5f maxStep=%.5f jumps>0.3=%zu nan=%zu\n", a.rms, a.peak, a.maxStep, a.jumps, a.nans);
	std::printf("automated : rms=%.5f peak=%.5f maxStep=%.5f jumps>0.3=%zu nan=%zu (smoothed)\n", b.rms, b.peak, b.maxStep, b.jumps, b.nans);
	std::printf("automated : rms=%.5f peak=%.5f maxStep=%.5f jumps>0.3=%zu nan=%zu (snap, no smoothing)\n", c.rms, c.peak, c.maxStep, c.jumps, c.nans);
	return b.nans ? 1 : 0;
}

int runCli(int argc, char **argv, bool &noAudio) {
	noAudio = false;
	if (argc >= 5 && !std::strcmp(argv[1], "--selftest"))
		return runSelftest(argv[2], argv[3], argv[4], argc >= 6 ? float(std::atof(argv[5])) : 1.0f);
	if (argc == 2 && !std::strcmp(argv[1], "--test-presets")) return runPresetTest();
	if (argc == 4 && !std::strcmp(argv[1], "--test-smooth")) return runSmoothTest(argv[2], argv[3]);
	if (argc == 1) return -1;
	if (argc == 2 && !std::strcmp(argv[1], "--no-audio")) {
		noAudio = true;
		return -1;
	}
	std::printf("Usage :\n"
	            "  RPVoice                       interface graphique\n"
	            "  RPVoice --no-audio            interface sans audio\n"
	            "  RPVoice --selftest in.wav out.wav <preset|0|fichier.json> [intensite]\n"
	            "  RPVoice --test-presets        test aller-retour JSON des presets\n"
	            "  RPVoice --test-smooth in.wav out.wav   test de clics (automation)\n");
	return 2;
}
