// presets.h - voice definitions, preset files (JSON) and app settings persistence.
//
// A preset file holds one voice: every parameter value plus an on/off flag per parameter.
// Schema (version 1) is documented in README.md. Factory presets come from voice::kPresets
// and are read-only; user presets live as one JSON file each in presetDir().
#pragma once

#include "voice_chain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

constexpr int kPresetSchemaVersion = 1;

// One editable voice: values + per-parameter enable flag.
struct VoiceDef {
	voice::Params p;
	bool on[voice::kNumParams];

	VoiceDef() { std::fill(on, on + voice::kNumParams, true); }
	explicit VoiceDef(const voice::Params &params) : p(params) { std::fill(on, on + voice::kNumParams, true); }

	float &value(int i) { return p.*voice::kParamInfo[i].ptr; }
	float value(int i) const { return p.*voice::kParamInfo[i].ptr; }

	// Disabled parameters fall back to neutral, then the intensity mix is applied.
	voice::Params effective(float intensity) const {
		voice::Params e = p;
		for (int i = 0; i < voice::kNumParams; ++i)
			if (!on[i]) e.*voice::kParamInfo[i].ptr = voice::kParamInfo[i].neutral;
		return voice::mix(e, intensity);
	}

	bool operator==(const VoiceDef &o) const {
		for (int i = 0; i < voice::kNumParams; ++i)
			if (value(i) != o.value(i) || on[i] != o.on[i]) return false;
		return true;
	}
	bool operator!=(const VoiceDef &o) const { return !(*this == o); }
};

struct PresetEntry {
	std::string name;
	bool factory = false;
	VoiceDef stored;   // last saved state (factory: the built-in values)
	VoiceDef working;  // what is being edited / played
	fs::path file;     // user presets only
	bool dirty() const { return working != stored; }
};

// UTF-8 <-> path helpers (paths may contain accents on Windows).
fs::path u8path(const std::string &s);
std::string pathU8(const fs::path &p);

fs::path appDir();     // %APPDATA%\RPVoice  |  ~/Library/Application Support/RPVoice
fs::path presetDir();  // appDir()/presets

// JSON (de)serialisation of one preset. fromJson accepts partial files (missing keys = defaults).
std::string presetToJson(const std::string &name, const VoiceDef &v);
bool presetFromJson(const std::string &text, std::string &name, VoiceDef &v, std::string &err);
bool savePresetFile(const fs::path &file, const std::string &name, const VoiceDef &v, std::string &err);
bool loadPresetFile(const fs::path &file, std::string &name, VoiceDef &v, std::string &err);

// Factory presets followed by the user presets found in presetDir() (sorted by name).
std::vector<PresetEntry> loadAllPresets(std::string &warnings);

// File name for a user preset (sanitised, unique in presetDir() unless it is `keep`).
fs::path presetFileFor(const std::string &name, const fs::path &keep = {});

struct Settings {
	std::string inputDevice, outputDevice, monitorDevice;  // device names, empty = default/auto
	bool monitorOn = false;
	float monitorVolume = 0.8f;
	std::string preset = "khajiit-marchand";
	bool presetFactory = true;
	float intensity = 1.0f;
	bool bypass = false;
	float inputGainDb = 0, outputGainDb = 0;
	bool showAdvanced = false;
};

Settings loadSettings();
bool saveSettings(const Settings &s);
