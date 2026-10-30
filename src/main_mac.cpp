// main_mac.cpp - macOS host for local testing: GLFW + OpenGL 3.2 core.
// Audio does not start automatically on the Mac (feedback risk with mic + speakers).
#define GL_SILENCE_DEPRECATION
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "selftest.h"
#include "ui.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// --screenshot out.bmp (dev aid): render a few frames, dump the framebuffer as a BMP, exit.
static bool writeBmp(const char *path, int w, int h, const std::vector<unsigned char> &rgba) {
	FILE *f = std::fopen(path, "wb");
	if (!f) return false;
	int row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
	unsigned char hd[54] = {'B', 'M'};
	auto put = [&](int off, int v) { for (int i = 0; i < 4; ++i) hd[off + i] = (unsigned char)(v >> (8 * i)); };
	put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h);
	hd[26] = 1; hd[28] = 24;
	std::fwrite(hd, 1, 54, f);
	const unsigned char zero[3] = {0, 0, 0};
	for (int y = 0; y < h; ++y) {  // GL rows are bottom-up, like BMP
		for (int x = 0; x < w; ++x) {
			const unsigned char *p = &rgba[size_t(y * w + x) * 4];
			unsigned char bgr[3] = {p[2], p[1], p[0]};
			std::fwrite(bgr, 1, 3, f);
		}
		std::fwrite(zero, 1, size_t(pad), f);
	}
	std::fclose(f);
	return true;
}

int main(int argc, char **argv) {
	const char *shot = nullptr;
	std::vector<char *> args(argv, argv + argc);
	for (size_t i = 1; i + 1 < args.size(); ++i)
		if (!std::strcmp(args[i], "--screenshot")) { shot = args[i + 1]; args.erase(args.begin() + long(i), args.begin() + long(i) + 2); break; }
	bool noAudio = false;
	int rc = runCli(int(args.size()), args.data(), noAudio);
	if (rc >= 0) return rc;

	glfwSetErrorCallback([](int e, const char *d) { std::fprintf(stderr, "GLFW error %d: %s\n", e, d); });
	if (!glfwInit()) return 1;
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	GLFWwindow *win = glfwCreateWindow(720, 640, "RPVoice — changeur de voix", nullptr, nullptr);
	if (!win) return 1;
	glfwSetWindowSizeLimits(win, 600, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 150");

	App app;
	AppOptions opt;
	opt.noAudio = noAudio;
	opt.autoStart = false;
	app.init(opt, HostHooks{});

	int frames = 0;
	while (!app.quitConfirmed()) {
		glfwPollEvents();
		if (glfwWindowShouldClose(win)) {
			if (app.requestClose()) break;
			glfwSetWindowShouldClose(win, GLFW_FALSE);
		}
		if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
			glfwWaitEventsTimeout(0.1);
			continue;
		}
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		app.frame();
		ImGui::Render();
		int w, h;
		glfwGetFramebufferSize(win, &w, &h);
		glViewport(0, 0, w, h);
		glClearColor(0.09f, 0.09f, 0.10f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		if (shot && ++frames == 30) {
			std::vector<unsigned char> px(size_t(w) * h * 4);
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			std::printf(writeBmp(shot, w, h, px) ? "screenshot %s (%dx%d)\n" : "cannot write %s\n", shot, w, h);
			break;
		}
		glfwSwapBuffers(win);
	}
	app.shutdown();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}
