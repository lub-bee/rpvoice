// ui.cpp - see ui.h. All user-visible text is French.
#include "ui.h"

#include "audio.h"
#include "imgui.h"
#include "presets.h"

#include "../vendor/fonts/roboto_font.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

double nowSec() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// How a parameter is shown in the editor.
enum class Unit { Semis, Pct, PctRel, Db, Hz, Plain };

struct ParamUi {
	const char *key, *label, *tip;
	Unit unit;
	bool toggle;  // has an on/off checkbox
};

const ParamUi kUi[] = {
	{"pitch", "Hauteur", "Décalage de hauteur en demi-tons (négatif = plus grave).", Unit::Semis, true},
	{"formant", "Gorge (formant)", "Taille apparente de la gorge : sous 100 % = plus grande et grave, au-dessus = plus petite et aiguë.", Unit::PctRel, true},
	{"sub", "Couche grave", "Ajoute une voix une octave plus bas, filtrée (épaisseur, carrure).", Unit::Pct, true},
	{"rasp", "Rugosité", "Grain irrégulier sur le volume (voix éraillée).", Unit::Pct, true},
	{"purr", "Ronronnement", "Modulation rapide du volume (ronron félin).", Unit::Pct, true},
	{"low", "Graves 150 Hz", "Plateau grave : rondeur et poids de la voix.", Unit::Db, true},
	{"lowmid", "Bas-médium 350 Hz", "Corps / épaisseur ; trop = voix « en boîte ».", Unit::Db, true},
	{"presence", "Présence 3 kHz", "Clarté et mordant ; trop = agressif.", Unit::Db, true},
	{"air", "Air 8 kHz", "Brillance, souffle ; baisser pour une voix plus sombre.", Unit::Db, true},
	{"drive", "Saturation", "Saturation douce : chaleur, agressivité.", Unit::Pct, true},
	{"chorus", "Doublage (chorus)", "Superpose une copie légèrement décalée : voix multiple, étrange.", Unit::Pct, true},
	{"reverb", "Réverbération", "Ajoute une pièce autour de la voix (crypte, salle).", Unit::Pct, true},
	{"purrRate", "Vitesse du ronron", "Fréquence du ronronnement (défaut 25 Hz).", Unit::Hz, false},
	{"subCutoff", "Filtre couche grave", "Fréquence de coupure de la couche grave (défaut 1200 Hz) : plus bas = plus sourd.", Unit::Hz, false},
	{"raspRate", "Vitesse rugosité", "Vitesse du grain de rugosité (défaut 70 Hz) : plus haut = plus fin.", Unit::Hz, false},
	{"chorusRate", "Vitesse chorus", "Vitesse d'oscillation du doublage (défaut 0,6 Hz).", Unit::Hz, false},
	{"chorusDepth", "Profondeur chorus", "Amplitude du décalage du doublage (défaut 15 %).", Unit::Pct, false},
	{"reverbRoom", "Taille de la pièce", "Taille de la réverbération (défaut 50 %).", Unit::Pct, false},
	{"hpf", "Coupe-bas entrée", "Filtre les très basses fréquences du micro (défaut 70 Hz).", Unit::Hz, false},
};
static_assert(sizeof(kUi) / sizeof(kUi[0]) == voice::kNumParams, "one UI entry per parameter");

struct Group {
	const char *title;
	std::vector<const char *> keys;
	bool advanced;
};

const Group kGroups[] = {
	{"Voix", {"pitch", "formant"}, false},
	{"Texture", {"sub", "rasp", "purr", "purrRate", "drive"}, false},
	{"Couleur (EQ)", {"low", "lowmid", "presence", "air"}, false},
	{"Espace", {"chorus", "reverb"}, false},
	{"Avancé", {"subCutoff", "raspRate", "chorusRate", "chorusDepth", "reverbRoom", "hpf"}, true},
};

float dbToLin(float db) { return std::pow(10.f, db / 20.f); }

bool iequals(const std::string &a, const std::string &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return true;
}

void applyTheme(float scale) {
	ImGuiStyle &st = ImGui::GetStyle();
	ImGui::StyleColorsDark(&st);
	st.WindowRounding = 0;
	st.ChildRounding = 4;
	st.FrameRounding = 4;
	st.GrabRounding = 4;
	st.PopupRounding = 4;
	st.TabRounding = 4;
	st.FramePadding = ImVec2(6, 4);
	st.ItemSpacing = ImVec2(8, 6);
	st.WindowPadding = ImVec2(10, 10);
	st.GrabMinSize = 12;
	ImVec4 *c = st.Colors;
	ImVec4 accent(0.86f, 0.55f, 0.20f, 1.0f);  // warm amber
	c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1);
	c[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1);
	c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 0.98f);
	c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.20f, 1);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.27f, 1);
	c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.26f, 0.24f, 1);
	c[ImGuiCol_SliderGrab] = accent;
	c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.68f, 0.30f, 1);
	c[ImGuiCol_CheckMark] = accent;
	c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.25f, 1);
	c[ImGuiCol_ButtonHovered] = ImVec4(0.32f, 0.29f, 0.25f, 1);
	c[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.33f, 0.18f, 1);
	c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.17f, 1);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.31f, 0.20f, 1);
	c[ImGuiCol_HeaderActive] = ImVec4(0.50f, 0.37f, 0.20f, 1);
	c[ImGuiCol_Tab] = ImVec4(0.18f, 0.18f, 0.20f, 1);
	c[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.31f, 0.20f, 1);
	c[ImGuiCol_TabSelected] = ImVec4(0.36f, 0.27f, 0.17f, 1);
	c[ImGuiCol_PlotHistogram] = ImVec4(0.35f, 0.70f, 0.40f, 1);
	st.ScaleAllSizes(scale);
}

}  // namespace

// ---------------------------------------------------------------- state
struct App::State {
	AppOptions opt;
	HostHooks hooks;
	audio::Engine engine;
	bool audioOk = false;
	std::string audioErr;
	Settings set;
	std::vector<PresetEntry> presets;
	int sel = 0;
	audio::Control lastCtl;
	bool ctlSent = false;
	bool settingsDirty = false;
	double settingsChangedAt = 0;
	audio::Stats stats;
	float inMeter = 0, outMeter = 0;
	std::string msg;
	bool msgErr = false;
	double msgAt = -100;

	// Dialogs.
	enum class NameMode { None, New, Duplicate, Rename, SaveCopy };
	NameMode nameMode = NameMode::None;
	char nameBuf[128] = {};
	std::string nameErr;
	bool openNamePopup = false, openDeletePopup = false, openClosePopup = false;
	enum class PathMode { None, Import, Export };
	PathMode pathMode = PathMode::None;
	char pathBuf[512] = {};
	bool openPathPopup = false;
	bool quit = false;

	PresetEntry &cur() { return presets[size_t(sel)]; }

	void note(const std::string &m, bool err = false) {
		msg = m;
		msgErr = err;
		msgAt = nowSec();
	}
	void touchSettings() {
		settingsDirty = true;
		settingsChangedAt = nowSec();
	}

	int findByName(const std::string &n) const {
		for (size_t i = 0; i < presets.size(); ++i)
			if (iequals(presets[i].name, n)) return int(i);
		return -1;
	}
	std::string uniqueName(const std::string &base) const {
		if (findByName(base) < 0) return base;
		for (int n = 2;; ++n) {
			std::string c = base + " (" + std::to_string(n) + ")";
			if (findByName(c) < 0) return c;
		}
	}

	void select(int i) {
		sel = std::clamp(i, 0, int(presets.size()) - 1);
		set.preset = cur().name;
		set.presetFactory = cur().factory;
		touchSettings();
	}

	// Insert a user preset (sorted after the factory ones), return its index.
	int addUser(PresetEntry e) {
		auto it = presets.begin() + voice::kNumPresets;
		while (it != presets.end() && it->name < e.name) ++it;
		return int(presets.insert(it, std::move(e)) - presets.begin());
	}

	bool saveUser(PresetEntry &e) {
		std::string err;
		if (e.file.empty()) e.file = presetFileFor(e.name);
		if (!savePresetFile(e.file, e.name, e.working, err)) {
			note("Erreur : " + err, true);
			return false;
		}
		e.stored = e.working;
		return true;
	}

	// ---------------------------------------------------------- audio
	std::string outputName() const {
		if (!set.outputDevice.empty()) return set.outputDevice;
		int c = engine.findCableOutput();
		return c >= 0 ? engine.outputs()[size_t(c)].name : std::string();
	}
	void startAudio() {
		if (opt.noAudio || !audioOk) return;
		std::string err;
		audioErr.clear();
		if (!engine.start(set.inputDevice, outputName(), set.monitorOn ? monitorName() : std::string(), err)) {
			audioErr = err;
			note("Erreur audio : " + err, true);
		} else if (!err.empty()) {
			audioErr = err;
			note(err, true);
		}
		ctlSent = false;
	}
	std::string monitorName() const {
		if (!set.monitorDevice.empty()) return set.monitorDevice;
		for (const auto &d : engine.outputs())
			if (d.isDefault) return d.name;
		return engine.outputs().empty() ? std::string() : engine.outputs()[0].name;
	}
	void restartIfRunning() {
		if (engine.running()) startAudio();
	}

	audio::Control control() const {
		audio::Control c;
		c.params = presets[size_t(sel)].working.effective(set.intensity);
		c.bypass = set.bypass;
		c.inGain = dbToLin(set.inputGainDb);
		c.outGain = dbToLin(set.outputGainDb);
		c.monitorGain = set.monitorVolume;
		return c;
	}
	void pushControl() {
		audio::Control c = control();
		bool same = ctlSent && std::memcmp(&c.params, &lastCtl.params, sizeof c.params) == 0 && c.bypass == lastCtl.bypass &&
		            c.inGain == lastCtl.inGain && c.outGain == lastCtl.outGain && c.monitorGain == lastCtl.monitorGain;
		if (!same) {
			engine.setControl(c);
			engine.setInitialControl(c);
			lastCtl = c;
			ctlSent = true;
		}
	}

	// ---------------------------------------------------------- UI parts
	void drawPresetList();
	void drawMaster();
	void drawEditor();
	void drawAudio();
	void drawStatus(float height);
	void drawModals();
	bool paramRow(int idx);
	void doSave();
	void beginName(NameMode m);
	void commitName();
	void doImport(const std::string &path);
	void doExport(const std::string &path);
	void beginImport();
	void beginExport();
};

App::App() : s_(new State) {}
App::~App() = default;

void App::init(const AppOptions &opt, const HostHooks &hooks) {
	State &s = *s_;
	s.opt = opt;
	s.hooks = hooks;

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;  // state is persisted in settings.json
	ImFontConfig fc;
	fc.FontDataOwnedByAtlas = false;
	io.Fonts->AddFontFromMemoryCompressedTTF(roboto_compressed_data, int(roboto_compressed_size), 16.0f, &fc);
	applyTheme(opt.scale);

	s.set = loadSettings();
	std::string warn;
	s.presets = loadAllPresets(warn);
	if (!warn.empty()) s.note("Presets ignorés : " + warn, true);
	int i = s.findByName(s.set.preset);
	s.sel = i >= 0 ? i : 0;

	if (!opt.noAudio) {
		std::string err;
		s.audioOk = s.engine.init(err);
		if (!s.audioOk) s.note("Erreur audio : " + err, true);
	}
	s.pushControl();
	if (opt.autoStart) s.startAudio();
}

bool App::requestClose() {
	for (const auto &p : s_->presets)
		if (p.dirty()) {
			s_->openClosePopup = true;
			return false;
		}
	return true;
}

bool App::quitConfirmed() const { return s_->quit; }

void App::shutdown() {
	s_->engine.stop();
	saveSettings(s_->set);
}

// ---------------------------------------------------------------- frame
void App::frame() {
	State &s = *s_;
	s.stats = s.engine.poll();
	// Meters: peak hold with ~20 dB/s fall.
	float dt = ImGui::GetIO().DeltaTime, fall = std::pow(10.f, -1.0f * dt);
	s.inMeter = std::max(s.stats.inPeak, s.inMeter * fall);
	s.outMeter = std::max(s.stats.outPeak, s.outMeter * fall);

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::Begin("##main", nullptr,
	             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
	                 ImGuiWindowFlags_NoBringToFrontOnFocus);

	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) s.doSave();

	float em = ImGui::GetFontSize();
	float statusH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("presets", ImVec2(em * 13, -statusH), ImGuiChildFlags_Borders);
	s.drawPresetList();
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("right", ImVec2(0, -statusH));
	s.drawMaster();
	if (ImGui::BeginTabBar("tabs")) {
		if (ImGui::BeginTabItem("Éditeur de voix")) {
			ImGui::BeginChild("editor", ImVec2(0, 0), ImGuiChildFlags_None);
			s.drawEditor();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Audio")) {
			ImGui::BeginChild("audio", ImVec2(0, 0), ImGuiChildFlags_None);
			s.drawAudio();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::EndChild();
	s.drawStatus(statusH);
	s.drawModals();
	ImGui::End();

	s.pushControl();
	if (s.settingsDirty && nowSec() - s.settingsChangedAt > 1.0) {
		saveSettings(s.set);
		s.settingsDirty = false;
	}
}

// ---------------------------------------------------------------- presets panel
void App::State::drawPresetList() {
	ImGui::TextDisabled("PRESETS");
	float em = ImGui::GetFontSize();
	float buttonsH = (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y) * 4 + 4;
	ImGui::BeginChild("list", ImVec2(0, -buttonsH));
	auto item = [&](int i) {
		PresetEntry &p = presets[size_t(i)];
		std::string label = p.name + (p.dirty() ? " *" : "") + "##p" + std::to_string(i);
		if (ImGui::Selectable(label.c_str(), i == sel)) select(i);
		if (p.factory) ImGui::SetItemTooltip("Preset d'usine (lecture seule) : Ctrl+S en crée une copie.");
		else ImGui::SetItemTooltip("%s", pathU8(p.file).c_str());
	};
	ImGui::SeparatorText("Usine");
	for (int i = 0; i < int(presets.size()); ++i)
		if (presets[size_t(i)].factory) item(i);
	ImGui::SeparatorText("Mes presets");
	bool any = false;
	for (int i = 0; i < int(presets.size()); ++i)
		if (!presets[size_t(i)].factory) item(i), any = true;
	if (!any) ImGui::TextDisabled("(aucun)");
	ImGui::EndChild();

	PresetEntry &c = cur();
	float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
	ImVec2 bs(w, 0);
	if (ImGui::Button("Nouveau", bs)) beginName(NameMode::New);
	ImGui::SetItemTooltip("Nouveau preset à partir des réglages actuels.");
	ImGui::SameLine();
	if (ImGui::Button("Dupliquer", bs)) beginName(NameMode::Duplicate);
	ImGui::SetItemTooltip("Copie du preset sélectionné (version enregistrée).");
	ImGui::BeginDisabled(c.factory);
	if (ImGui::Button("Renommer", bs)) beginName(NameMode::Rename);
	ImGui::SameLine();
	if (ImGui::Button("Supprimer", bs)) openDeletePopup = true;
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!c.dirty() && !c.factory);
	if (ImGui::Button(c.factory ? "Enregistrer copie" : "Enregistrer", ImVec2(-FLT_MIN, 0))) doSave();
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Ctrl+S");
	if (ImGui::Button("Importer…", bs)) beginImport();
	ImGui::SameLine();
	if (ImGui::Button("Exporter…", bs)) beginExport();
	(void)em;
}

void App::State::doSave() {
	PresetEntry &c = cur();
	if (c.factory) {
		beginName(NameMode::SaveCopy);
		return;
	}
	if (saveUser(c)) note("Preset « " + c.name + " » enregistré.");
}

void App::State::beginName(NameMode m) {
	nameMode = m;
	nameErr.clear();
	std::string base = cur().name;
	std::string sug = m == NameMode::Rename ? base : uniqueName(m == NameMode::New ? base + " (perso)" : base + " (copie)");
	if (m == NameMode::SaveCopy) sug = uniqueName(base + " (perso)");
	std::snprintf(nameBuf, sizeof nameBuf, "%s", sug.c_str());
	openNamePopup = true;
}

void App::State::commitName() {
	std::string n(nameBuf);
	while (!n.empty() && n.back() == ' ') n.pop_back();
	while (!n.empty() && n.front() == ' ') n.erase(n.begin());
	if (n.empty()) { nameErr = "Le nom ne peut pas être vide."; return; }
	int other = findByName(n);
	if (other >= 0 && !(nameMode == NameMode::Rename && other == sel)) { nameErr = "Ce nom existe déjà."; return; }

	PresetEntry &c = cur();
	if (nameMode == NameMode::Rename) {
		PresetEntry e = c;
		fs::path old = e.file;
		e.name = n;
		e.file = presetFileFor(n, old);
		std::string err;
		if (!savePresetFile(e.file, e.name, e.stored, err)) { nameErr = err; return; }
		std::error_code ec;
		if (e.file != old) fs::remove(old, ec);
		presets.erase(presets.begin() + sel);
		select(addUser(e));
		note("Preset renommé en « " + n + " ».");
	} else {
		PresetEntry e;
		e.name = n;
		e.factory = false;
		e.working = nameMode == NameMode::Duplicate ? c.stored : c.working;
		if (nameMode != NameMode::Duplicate) c.working = c.stored;  // the edits move to the new preset
		if (!saveUser(e)) { nameErr = "Impossible d'enregistrer."; return; }
		select(addUser(e));
		note("Preset « " + n + " » créé.");
	}
	nameMode = NameMode::None;
	ImGui::CloseCurrentPopup();
}

void App::State::beginImport() {
	if (hooks.openJson) {
		std::string p;
		if (hooks.openJson(p)) doImport(p);
		return;
	}
	pathMode = PathMode::Import;
	std::snprintf(pathBuf, sizeof pathBuf, "%s", pathU8(u8path(std::getenv("HOME") ? std::getenv("HOME") : "") / "Downloads/").c_str());
	openPathPopup = true;
}

void App::State::beginExport() {
	std::string file = pathU8(presetFileFor(cur().name).filename());
	if (hooks.saveJson) {
		std::string p;
		if (hooks.saveJson(file, p)) doExport(p);
		return;
	}
	pathMode = PathMode::Export;
	std::snprintf(pathBuf, sizeof pathBuf, "%s", pathU8(u8path(std::getenv("HOME") ? std::getenv("HOME") : "") / "Desktop" / u8path(file)).c_str());
	openPathPopup = true;
}

void App::State::doImport(const std::string &path) {
	PresetEntry e;
	std::string err;
	if (!loadPresetFile(u8path(path), e.name, e.stored, err)) {
		note("Import impossible : " + err, true);
		return;
	}
	e.name = uniqueName(e.name);
	e.working = e.stored;
	if (!saveUser(e)) return;
	select(addUser(e));
	note("Preset « " + e.name + " » importé.");
}

void App::State::doExport(const std::string &path) {
	fs::path p = u8path(path);
	if (p.extension() != ".json") p += ".json";
	std::string err;
	if (!savePresetFile(p, cur().name, cur().working, err)) note("Export impossible : " + err, true);
	else note("Exporté : " + pathU8(p));
}

// ---------------------------------------------------------------- master
void App::State::drawMaster() {
	ImGuiStyle &st = ImGui::GetStyle();
	float em = ImGui::GetFontSize();
	PresetEntry &c = cur();
	ImGui::Text("%s", c.name.c_str());
	ImGui::SameLine();
	if (c.factory) ImGui::TextDisabled("(usine)");
	if (c.dirty()) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "• modifié");
		ImGui::SetItemTooltip("Modifications non enregistrées (Ctrl+S pour enregistrer).");
	}

	// Intensity: big slider.
	float pct = set.intensity * 100;
	ImGui::PushFont(nullptr, st.FontSizeBase * 1.3f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * opt.scale, 8 * opt.scale));
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 24 * opt.scale);
	float bypassW = em * 9;
	ImGui::SetNextItemWidth(-bypassW - st.ItemSpacing.x);
	if (ImGui::SliderFloat("##intensity", &pct, 0, 100, "Intensité %.0f %%", ImGuiSliderFlags_AlwaysClamp)) {
		set.intensity = pct / 100;
		touchSettings();
	}
	ImGui::PopStyleVar(2);
	ImGui::SetItemTooltip("De la voix neutre (0 %%) au preset complet (100 %%). Ctrl+clic pour saisir une valeur.");
	ImGui::SameLine();
	bool by = set.bypass;
	if (by) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.18f, 0.15f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.25f, 0.20f, 1));
	}
	if (ImGui::Button(by ? "VOIX ORIGINALE" : "Voix originale", ImVec2(-FLT_MIN, ImGui::GetItemRectSize().y))) {
		set.bypass = !set.bypass;
		touchSettings();
	}
	if (by) ImGui::PopStyleColor(2);
	ImGui::PopFont();
	ImGui::SetItemTooltip("Bypass : envoie votre voix sans effet (cliquer à nouveau pour revenir).");

	// Gains + meters.
	float half = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x) / 2;
	auto meter = [&](const char *label, float lin, float width) {
		float db = lin > 1e-6f ? 20 * std::log10(lin) : -90.f;
		float f = std::clamp((db + 60) / 60, 0.f, 1.f);
		char ov[48];
		if (db > -89) std::snprintf(ov, sizeof ov, "%s  %.0f dB", label, db);
		else std::snprintf(ov, sizeof ov, "%s  -inf", label);
		ImVec4 col = db > -1 ? ImVec4(0.85f, 0.25f, 0.2f, 1) : db > -9 ? ImVec4(0.85f, 0.7f, 0.25f, 1) : ImVec4(0.30f, 0.65f, 0.38f, 1);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
		ImGui::ProgressBar(f, ImVec2(width, 0), ov);
		ImGui::PopStyleColor();
	};
	ImGui::SetNextItemWidth(half);
	if (ImGui::SliderFloat("##ing", &set.inputGainDb, -24, 24, "Gain entrée %+.1f dB", ImGuiSliderFlags_AlwaysClamp)) touchSettings();
	ImGui::SetItemTooltip("Gain appliqué au micro avant les effets. Double-clic : 0 dB.");
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) set.inputGainDb = 0;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(half);
	if (ImGui::SliderFloat("##outg", &set.outputGainDb, -24, 12, "Gain sortie %+.1f dB", ImGuiSliderFlags_AlwaysClamp)) touchSettings();
	ImGui::SetItemTooltip("Volume envoyé au jeu (après les effets). Double-clic : 0 dB.");
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) set.outputGainDb = 0;
	meter("Entrée", inMeter, half);
	ImGui::SameLine();
	meter("Sortie", outMeter, half);
	ImGui::Spacing();
}

// ---------------------------------------------------------------- editor
bool App::State::paramRow(int idx) {
	const voice::ParamInfo &pi = voice::kParamInfo[idx];
	const ParamUi &u = kUi[idx];
	PresetEntry &c = cur();
	VoiceDef &w = c.working;
	bool changed = false;
	ImGui::PushID(idx);
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	if (u.toggle) {
		if (ImGui::Checkbox("##on", &w.on[idx])) changed = true;
		ImGui::SetItemTooltip(w.on[idx] ? "Actif (décocher pour désactiver cet effet)" : "Désactivé");
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::AlignTextToFramePadding();
	bool differs = w.value(idx) != c.stored.value(idx) || w.on[idx] != c.stored.on[idx];
	if (differs) ImGui::TextColored(ImVec4(1, 0.75f, 0.4f, 1), "%s", u.label);
	else ImGui::TextUnformatted(u.label);
	ImGui::SetItemTooltip("%s", u.tip);
	ImGui::TableSetColumnIndex(2);
	ImGui::BeginDisabled(u.toggle && !w.on[idx]);
	ImGui::SetNextItemWidth(-FLT_MIN);
	float scale = (u.unit == Unit::Pct || u.unit == Unit::PctRel) ? 100.f : 1.f;
	const char *fmt = "%.2f";
	switch (u.unit) {
		case Unit::Semis: fmt = "%+.1f demi-tons"; break;
		case Unit::Pct: fmt = "%.0f %%"; break;
		case Unit::PctRel: fmt = "%.0f %%"; break;
		case Unit::Db: fmt = "%+.1f dB"; break;
		case Unit::Hz: fmt = pi.max <= 5 ? "%.2f Hz" : "%.0f Hz"; break;
		case Unit::Plain: break;
	}
	float disp = w.value(idx) * scale;
	if (ImGui::SliderFloat("##v", &disp, pi.min * scale, pi.max * scale, fmt, ImGuiSliderFlags_AlwaysClamp)) {
		w.value(idx) = std::clamp(disp / scale, pi.min, pi.max);
		changed = true;
	}
	ImGui::SetItemTooltip("%s\nCtrl+clic pour saisir une valeur.", u.tip);
	ImGui::EndDisabled();
	ImGui::TableSetColumnIndex(3);
	ImGui::BeginDisabled(!differs);
	if (ImGui::SmallButton("R")) {
		w.value(idx) = c.stored.value(idx);
		w.on[idx] = c.stored.on[idx];
		changed = true;
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Revenir à la valeur du preset");
	ImGui::PopID();
	return changed;
}

void App::State::drawEditor() {
	PresetEntry &c = cur();
	for (const Group &g : kGroups) {
		ImGuiTreeNodeFlags fl = g.advanced ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
		if (g.advanced) ImGui::SetNextItemOpen(set.showAdvanced, ImGuiCond_Always);
		bool open = ImGui::CollapsingHeader(g.title, fl);
		if (g.advanced && open != set.showAdvanced) {
			set.showAdvanced = open;
			touchSettings();
		}
		if (!open) continue;
		if (ImGui::BeginTable(g.title, 4, ImGuiTableFlags_SizingFixedFit)) {
			float em = ImGui::GetFontSize();
			ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
			ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, em * 9);
			ImGui::TableSetupColumn("slider", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, em * 1.4f);
			for (const char *k : g.keys) paramRow(voice::findParam(k));
			ImGui::EndTable();
		}
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(!c.dirty());
	if (ImGui::Button("Tout rétablir")) c.working = c.stored;
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Annule toutes les modifications non enregistrées de ce preset.");
}

// ---------------------------------------------------------------- audio settings
void App::State::drawAudio() {
	if (opt.noAudio) {
		ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Audio désactivé (lancé avec --no-audio).");
	} else if (!audioOk) {
		ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Audio indisponible.");
	}
	bool changed = false;
	float em = ImGui::GetFontSize();
	auto combo = [&](const char *label, const char *id, std::string &value, const std::vector<audio::DeviceInfo> &list,
	                 const char *defLabel) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::SameLine(em * 11);
		ImGui::SetNextItemWidth(-FLT_MIN);
		bool found = value.empty();
		for (const auto &d : list) found |= d.name == value;
		std::string preview = value.empty() ? defLabel : (found ? value : value + " (introuvable)");
		bool ch = false;
		if (ImGui::BeginCombo(id, preview.c_str())) {
			if (ImGui::Selectable(defLabel, value.empty())) value.clear(), ch = true;
			for (const auto &d : list) {
				std::string l = d.name + (d.isDefault ? "  [défaut système]" : "");
				if (ImGui::Selectable(l.c_str(), d.name == value)) value = d.name, ch = true;
			}
			ImGui::EndCombo();
		}
		return ch;
	};

	ImGui::SeparatorText("Périphériques");
	if (combo("Micro (entrée)", "##in", set.inputDevice, engine.inputs(), "Micro par défaut du système")) changed = true;
	int cable = engine.findCableOutput();
	std::string autoOut = cable >= 0 ? "Auto : " + engine.outputs()[size_t(cable)].name : std::string("Auto : sortie par défaut");
	if (combo("Sortie (vers le jeu)", "##out", set.outputDevice, engine.outputs(), autoOut.c_str())) changed = true;
	if (cable < 0 && audioOk)
		ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "« CABLE Input » introuvable : VB-Cable est-il installé ?");
	else
		ImGui::TextDisabled("Choisir « CABLE Input », puis « CABLE Output » comme micro dans le jeu.");

	ImGui::SeparatorText("Retour casque (monitor)");
	if (ImGui::Checkbox("M'entendre dans un casque", &set.monitorOn)) changed = true;
	ImGui::SetItemTooltip("Envoie aussi la voix transformée vers un second périphérique (votre casque).");
	ImGui::BeginDisabled(!set.monitorOn);
	if (combo("Casque", "##mon", set.monitorDevice, engine.outputs(), "Sortie par défaut du système")) changed = true;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Volume casque");
	ImGui::SameLine(em * 11);
	ImGui::SetNextItemWidth(-FLT_MIN);
	float mv = set.monitorVolume * 100;
	if (ImGui::SliderFloat("##monvol", &mv, 0, 100, "%.0f %%", ImGuiSliderFlags_AlwaysClamp)) {
		set.monitorVolume = mv / 100;
		touchSettings();
	}
	ImGui::EndDisabled();
	if (set.monitorOn && monitorName() == outputName() && !outputName().empty())
		ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Le casque et la sortie sont le même périphérique.");

	ImGui::SeparatorText("État");
	ImGui::BeginDisabled(opt.noAudio || !audioOk);
	if (engine.running()) {
		if (ImGui::Button("Arrêter l'audio")) engine.stop();
		ImGui::SameLine();
		if (ImGui::Button("Redémarrer")) startAudio();
	} else if (ImGui::Button("Démarrer l'audio")) {
		startAudio();
	}
	ImGui::SameLine();
	if (ImGui::Button("Actualiser la liste")) {
		bool was = engine.running();
		engine.stop();
		engine.refreshDevices();
		if (was) startAudio();
	}
	ImGui::EndDisabled();
	if (!audioErr.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", audioErr.c_str());
	if (engine.running()) {
		const audio::StreamInfo &in = engine.info();
		auto rateTxt = [](unsigned r) { return r == audio::kSampleRate ? "ok" : "rééchantillonné"; };
		ImGui::Text("Micro : %s", in.inName.c_str());
		ImGui::TextDisabled("   %u Hz (%s) — tampon %.1f ms", in.inRate, rateTxt(in.inRate), in.inMs);
		ImGui::Text("Sortie : %s", in.outName.c_str());
		ImGui::TextDisabled("   %u Hz (%s) — tampon %.1f ms", in.outRate, rateTxt(in.outRate), in.outMs);
		if (engine.monitorRunning()) {
			ImGui::Text("Casque : %s", in.monitorName.c_str());
			ImGui::TextDisabled("   %u Hz (%s) — sous-alimentations %u", in.monitorRate, rateTxt(in.monitorRate), stats.monitorUnderruns);
		}
		if (in.resampled())
			ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Rééchantillonné : réglez le périphérique sur 48 000 Hz dans Windows pour un son optimal.");
		ImGui::Text("Latence estimée : ~%.0f ms", in.totalMs);
		ImGui::TextDisabled("   moteur voix %.1f + micro %.1f + sortie %.1f + tampon %.1f ms (min. plausible ~%.0f ms)",
		                    in.engineMs, in.inMs, in.outMs, in.bufferMs, in.minMs);
	} else {
		ImGui::TextDisabled("Audio arrêté.");
	}
	if (changed) {
		touchSettings();
		restartIfRunning();
	}
}

// ---------------------------------------------------------------- status bar
void App::State::drawStatus(float) {
	ImGui::Separator();
	if (engine.running()) {
		ImGui::Text("Latence ~%.0f ms", engine.info().totalMs);
		ImGui::SameLine(0, 20);
		ImGui::Text("CPU moy. %.0f %% / pointe %.0f %%", stats.cpuAvg * 100, stats.cpuPeak * 100);
		ImGui::SetItemTooltip("Temps de calcul de la voix / durée d'un bloc audio, sur la dernière seconde :\n"
		                      "moy. = moyenne, pointe = pire bloc. Au-delà de 100 %% le son décroche.");
		ImGui::SameLine(0, 20);
		ImVec4 gc = stats.glitches ? ImVec4(1, 0.6f, 0.3f, 1) : ImGui::GetStyle().Colors[ImGuiCol_Text];
		ImGui::TextColored(gc, "Glitchs %u", stats.glitches + stats.late);
		ImGui::SetItemTooltip("Trous dans le flux audio : %u ; blocs calculés trop lentement : %u", stats.glitches, stats.late);
	} else {
		ImGui::TextDisabled(opt.noAudio ? "Audio désactivé" : "Audio arrêté");
	}
	if (nowSec() - msgAt < 6 && !msg.empty()) {
		ImGui::SameLine(0, 20);
		if (msgErr) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", msg.c_str());
		else ImGui::TextDisabled("%s", msg.c_str());
	}
}

// ---------------------------------------------------------------- dialogs
void App::State::drawModals() {
	float em = ImGui::GetFontSize();
	if (openNamePopup) { ImGui::OpenPopup("Nom du preset"); openNamePopup = false; }
	if (openDeletePopup) { ImGui::OpenPopup("Supprimer le preset"); openDeletePopup = false; }
	if (openPathPopup) { ImGui::OpenPopup(pathMode == PathMode::Import ? "Importer" : "Exporter"); openPathPopup = false; }
	if (openClosePopup) { ImGui::OpenPopup("Quitter"); openClosePopup = false; }
	ImVec2 center = ImGui::GetMainViewport()->GetCenter();

	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Nom du preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		const char *t = nameMode == NameMode::Rename ? "Nouveau nom :" : nameMode == NameMode::SaveCopy
		                                                                     ? "Les presets d'usine sont en lecture seule.\nEnregistrer une copie sous le nom :"
		                                                                     : "Nom du nouveau preset :";
		ImGui::TextUnformatted(t);
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(em * 20);
		bool enter = ImGui::InputText("##name", nameBuf, sizeof nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
		if (!nameErr.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", nameErr.c_str());
		if (ImGui::Button("OK", ImVec2(em * 6, 0)) || enter) commitName();
		ImGui::SameLine();
		if (ImGui::Button("Annuler", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			nameMode = NameMode::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Supprimer le preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Supprimer définitivement « %s » ?", cur().name.c_str());
		ImGui::TextDisabled("%s", pathU8(cur().file).c_str());
		if (ImGui::Button("Supprimer", ImVec2(em * 6, 0))) {
			std::error_code ec;
			std::string n = cur().name;
			if (!cur().factory) {
				fs::remove(cur().file, ec);
				presets.erase(presets.begin() + sel);
				select(std::min(sel, int(presets.size()) - 1));
				note("Preset « " + n + " » supprimé.");
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Annuler", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	for (const char *title : {"Importer", "Exporter"}) {
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(pathMode == PathMode::Import ? "Chemin du fichier .json à importer :" : "Enregistrer le preset sous :");
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(em * 28);
			bool enter = ImGui::InputText("##path", pathBuf, sizeof pathBuf, ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("OK", ImVec2(em * 6, 0)) || enter) {
				if (pathMode == PathMode::Import) doImport(pathBuf);
				else doExport(pathBuf);
				pathMode = PathMode::None;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Annuler", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				pathMode = PathMode::None;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Quitter", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Des presets ont des modifications non enregistrées :");
		bool factoryDirty = false;
		for (const auto &p : presets)
			if (p.dirty()) {
				ImGui::BulletText("%s%s", p.name.c_str(), p.factory ? " (usine : perdu)" : "");
				factoryDirty |= p.factory;
			}
		if (factoryDirty) ImGui::TextDisabled("Pour garder un preset d'usine modifié, utilisez « Enregistrer copie ».");
		if (ImGui::Button("Enregistrer et quitter")) {
			bool ok = true;
			for (auto &p : presets)
				if (p.dirty() && !p.factory) ok &= saveUser(p);
			if (ok) quit = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Quitter sans enregistrer")) {
			quit = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Annuler") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
