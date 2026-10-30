// presets.cpp - see presets.h.
#include "presets.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

fs::path u8path(const std::string &s) {
#ifdef _WIN32
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
	return fs::path(w);
#else
	return fs::path(s);
#endif
}

std::string pathU8(const fs::path &p) {
#ifdef _WIN32
	const std::wstring &w = p.native();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
	return s;
#else
	return p.string();
#endif
}

fs::path appDir() {
#ifdef _WIN32
	const wchar_t *a = _wgetenv(L"APPDATA");
	fs::path base = a ? fs::path(a) : fs::path(L".");
#else
	const char *h = std::getenv("HOME");
	fs::path base = fs::path(h ? h : ".") / "Library" / "Application Support";
#endif
	return base / "RPVoice";
}

fs::path presetDir() { return appDir() / "presets"; }

// Atomic replace of `to` by `from` (std::filesystem::rename may refuse an existing target on Windows).
static bool replaceFile(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
	return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	std::error_code ec;
	fs::rename(from, to, ec);
	return !ec;
#endif
}

// Shortest decimal that rounds back to the same float (keeps the JSON readable: 0.9, not 0.8999999761).
static double tidy(float v) {
	char buf[32];
	for (int prec = 1; prec <= 9; ++prec) {
		std::snprintf(buf, sizeof buf, "%.*g", prec, double(v));
		if (std::strtof(buf, nullptr) == v) return std::strtod(buf, nullptr);
	}
	return double(v);
}

std::string presetToJson(const std::string &name, const VoiceDef &v) {
	json params = json::object(), enabled = json::object();
	for (int i = 0; i < voice::kNumParams; ++i) {
		params[voice::kParamInfo[i].key] = tidy(v.value(i));
		enabled[voice::kParamInfo[i].key] = v.on[i];
	}
	json j = {{"app", "RPVoice"}, {"version", kPresetSchemaVersion}, {"name", name}, {"params", params}, {"enabled", enabled}};
	return j.dump(2) + "\n";
}

bool presetFromJson(const std::string &text, std::string &name, VoiceDef &v, std::string &err) {
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { err = "JSON invalide"; return false; }
	int ver = j.value("version", 0);
	if (ver < 1 || ver > kPresetSchemaVersion) { err = "version de preset non prise en charge"; return false; }
	if (!j.contains("params") || !j["params"].is_object()) { err = "champ \"params\" manquant"; return false; }
	VoiceDef out;
	const json &pa = j["params"];
	for (int i = 0; i < voice::kNumParams; ++i) {
		const voice::ParamInfo &pi = voice::kParamInfo[i];
		auto it = pa.find(pi.key);
		if (it != pa.end() && it->is_number()) out.value(i) = std::clamp(it->get<float>(), pi.min, pi.max);
	}
	if (j.contains("enabled") && j["enabled"].is_object()) {
		const json &en = j["enabled"];
		for (int i = 0; i < voice::kNumParams; ++i) {
			auto it = en.find(voice::kParamInfo[i].key);
			if (it != en.end() && it->is_boolean()) out.on[i] = it->get<bool>();
		}
	}
	name = j.value("name", std::string());
	v = out;
	return true;
}

bool savePresetFile(const fs::path &file, const std::string &name, const VoiceDef &v, std::string &err) {
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	fs::path tmp = file;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) { err = "impossible d'écrire " + pathU8(file); return false; }
		f << presetToJson(name, v);
		if (!f) { err = "erreur d'écriture " + pathU8(file); return false; }
	}
	if (!replaceFile(tmp, file)) { err = "impossible de remplacer " + pathU8(file); return false; }
	return true;
}

static bool readFile(const fs::path &file, std::string &out) {
	std::ifstream f(file, std::ios::binary);
	if (!f) return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool loadPresetFile(const fs::path &file, std::string &name, VoiceDef &v, std::string &err) {
	std::string text;
	if (!readFile(file, text)) { err = "impossible de lire " + pathU8(file); return false; }
	if (!presetFromJson(text, name, v, err)) { err = pathU8(file.filename()) + " : " + err; return false; }
	if (name.empty()) name = pathU8(file.stem());
	return true;
}

std::vector<PresetEntry> loadAllPresets(std::string &warnings) {
	std::vector<PresetEntry> out;
	for (int i = 0; i < voice::kNumPresets; ++i) {
		PresetEntry e;
		e.name = voice::kPresets[i].name;
		e.factory = true;
		e.stored = e.working = VoiceDef(voice::kPresets[i].p);
		out.push_back(e);
	}
	std::vector<PresetEntry> user;
	std::error_code ec;
	fs::path dir = presetDir();
	fs::create_directories(dir, ec);
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_regular_file() || it->path().extension() != ".json") continue;
		PresetEntry e;
		std::string err;
		if (!loadPresetFile(it->path(), e.name, e.stored, err)) { warnings += err + "\n"; continue; }
		e.working = e.stored;
		e.file = it->path();
		user.push_back(e);
	}
	std::sort(user.begin(), user.end(), [](const PresetEntry &a, const PresetEntry &b) { return a.name < b.name; });
	out.insert(out.end(), user.begin(), user.end());
	return out;
}

fs::path presetFileFor(const std::string &name, const fs::path &keep) {
	std::string base;
	for (unsigned char c : name) {
		if (c < 32 || std::strchr("<>:\"/\\|?*", c)) base += '_';
		else base += char(c);
	}
	while (!base.empty() && (base.back() == ' ' || base.back() == '.')) base.pop_back();
	if (base.empty()) base = "preset";
	fs::path dir = presetDir();
	for (int n = 1;; ++n) {
		std::string stem = n == 1 ? base : base + " (" + std::to_string(n) + ")";
		fs::path p = dir / u8path(stem + ".json");
		std::error_code ec;
		if (p == keep || !fs::exists(p, ec)) return p;
	}
}

static fs::path settingsFile() { return appDir() / "settings.json"; }

Settings loadSettings() {
	Settings s;
	std::string text;
	if (!readFile(settingsFile(), text)) return s;
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return s;
	s.inputDevice = j.value("inputDevice", s.inputDevice);
	s.outputDevice = j.value("outputDevice", s.outputDevice);
	s.monitorDevice = j.value("monitorDevice", s.monitorDevice);
	s.monitorOn = j.value("monitorOn", s.monitorOn);
	s.monitorVolume = std::clamp(j.value("monitorVolume", s.monitorVolume), 0.f, 1.f);
	s.preset = j.value("preset", s.preset);
	s.presetFactory = j.value("presetFactory", s.presetFactory);
	s.intensity = std::clamp(j.value("intensity", s.intensity), 0.f, 1.f);
	s.bypass = j.value("bypass", s.bypass);
	s.inputGainDb = std::clamp(j.value("inputGainDb", s.inputGainDb), -24.f, 24.f);
	s.outputGainDb = std::clamp(j.value("outputGainDb", s.outputGainDb), -24.f, 12.f);
	s.showAdvanced = j.value("showAdvanced", s.showAdvanced);
	return s;
}

bool saveSettings(const Settings &s) {
	json j = {{"version", 1},
	          {"inputDevice", s.inputDevice},
	          {"outputDevice", s.outputDevice},
	          {"monitorDevice", s.monitorDevice},
	          {"monitorOn", s.monitorOn},
	          {"monitorVolume", tidy(s.monitorVolume)},
	          {"preset", s.preset},
	          {"presetFactory", s.presetFactory},
	          {"intensity", tidy(s.intensity)},
	          {"bypass", s.bypass},
	          {"inputGainDb", tidy(s.inputGainDb)},
	          {"outputGainDb", tidy(s.outputGainDb)},
	          {"showAdvanced", s.showAdvanced}};
	std::error_code ec;
	fs::create_directories(appDir(), ec);
	fs::path tmp = settingsFile();
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f << j.dump(2) << "\n";
		if (!f) return false;
	}
	return replaceFile(tmp, settingsFile());
}
