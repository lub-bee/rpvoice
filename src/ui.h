// ui.h - the RPVoice Dear ImGui interface (platform-agnostic; hosts are main_win.cpp / main_mac.cpp).
#pragma once

#include <memory>
#include <string>

// Optional native services provided by the host.
struct HostHooks {
	// Native file dialogs (Windows). When null the UI shows a path text field instead.
	bool (*openJson)(std::string &path) = nullptr;
	bool (*saveJson)(const std::string &suggestedName, std::string &path) = nullptr;
};

struct AppOptions {
	bool noAudio = false;    // never open audio devices (screenshots / tests)
	bool autoStart = false;  // start audio at launch (Windows); the mac build waits for the button
	float scale = 1.0f;      // DPI scale (sizes; fonts are scaled by the host via style.FontScaleDpi)
};

class App {
public:
	App();
	~App();
	// Call after ImGui::CreateContext(): loads fonts + theme, presets, settings, audio.
	void init(const AppOptions &opt, const HostHooks &hooks);
	void frame();          // between ImGui::NewFrame() and ImGui::Render()
	bool requestClose();   // true = may close now; false = a confirmation dialog was opened
	bool quitConfirmed() const;
	void shutdown();       // saves settings, stops audio

private:
	struct State;
	std::unique_ptr<State> s_;
};
