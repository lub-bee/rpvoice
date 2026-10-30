// main_win.cpp - Windows host: Win32 window + Direct3D 11 (adapted from Dear ImGui's
// example_win32_directx11). GUI subsystem; headless modes attach to the parent console.
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "presets.h"
#include "selftest.h"
#include "ui.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;
IDXGISwapChain *g_swap = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
bool g_occluded = false;
UINT g_resizeW = 0, g_resizeH = 0;
HWND g_hwnd = nullptr;
App *g_app = nullptr;

std::wstring widen(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
	if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}
std::string narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

void createRenderTarget() {
	ID3D11Texture2D *back = nullptr;
	g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
	if (back) {
		g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
	}
}
void cleanupRenderTarget() {
	if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool createDevice(HWND hWnd) {
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL fl;
	const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
	                                            &sd, &g_swap, &g_dev, &fl, &g_ctx);
	if (res == DXGI_ERROR_UNSUPPORTED)  // no GPU driver: software WARP
		res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
		                                    &g_swap, &g_dev, &fl, &g_ctx);
	if (res != S_OK) return false;
	createRenderTarget();
	return true;
}
void cleanupDevice() {
	cleanupRenderTarget();
	if (g_swap) { g_swap->Release(); g_swap = nullptr; }
	if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
	if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg) {
		case WM_SIZE:
			if (wParam == SIZE_MINIMIZED) return 0;
			g_resizeW = UINT(LOWORD(lParam));
			g_resizeH = UINT(HIWORD(lParam));
			return 0;
		case WM_GETMINMAXINFO: {
			auto *mm = reinterpret_cast<MINMAXINFO *>(lParam);
			mm->ptMinTrackSize.x = 600;
			mm->ptMinTrackSize.y = 480;
			return 0;
		}
		case WM_SYSCOMMAND:
			if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // disable ALT application menu
			break;
		case WM_CLOSE:
			if (!g_app || g_app->requestClose()) DestroyWindow(hWnd);
			return 0;  // otherwise the app shows its confirmation dialog
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

const wchar_t kFilter[] = L"Preset RPVoice (*.json)\0*.json\0Tous les fichiers\0*.*\0";

bool openJson(std::string &path) {
	wchar_t buf[MAX_PATH * 4] = L"";
	OPENFILENAMEW of{};
	of.lStructSize = sizeof of;
	of.hwndOwner = g_hwnd;
	of.lpstrFilter = kFilter;
	of.lpstrFile = buf;
	of.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
	of.lpstrTitle = L"Importer un preset";
	of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&of)) return false;
	path = narrow(buf);
	return true;
}

bool saveJson(const std::string &suggested, std::string &path) {
	wchar_t buf[MAX_PATH * 4] = L"";
	std::wstring s = widen(suggested);
	wcsncpy(buf, s.c_str(), sizeof buf / sizeof buf[0] - 1);
	OPENFILENAMEW of{};
	of.lStructSize = sizeof of;
	of.hwndOwner = g_hwnd;
	of.lpstrFilter = kFilter;
	of.lpstrFile = buf;
	of.nMaxFile = DWORD(sizeof buf / sizeof buf[0]);
	of.lpstrTitle = L"Exporter le preset";
	of.lpstrDefExt = L"json";
	of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&of)) return false;
	path = narrow(buf);
	return true;
}

}  // namespace

int main(int, char **) {
	// UTF-8 argv from the wide command line (paths may contain accents).
	int wargc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	std::vector<std::string> args;
	for (int i = 0; i < wargc; ++i) args.push_back(narrow(wargv[i]));
	LocalFree(wargv);
	std::vector<char *> argv;
	for (auto &a : args) argv.push_back(a.data());
	argv.push_back(nullptr);
	if (wargc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {  // GUI subsystem: reuse the caller's console
		std::freopen("CONOUT$", "w", stdout);
		std::freopen("CONOUT$", "w", stderr);
		SetConsoleOutputCP(CP_UTF8);
	}
	bool noAudio = false;
	int rc = runCli(wargc, argv.data(), noAudio);
	if (rc >= 0) return rc;

	ImGui_ImplWin32_EnableDpiAwareness();
	float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
	WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wndProc, 0L, 0L, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"RPVoice", nullptr};
	wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
	wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	RegisterClassExW(&wc);
	g_hwnd = CreateWindowW(wc.lpszClassName, L"RPVoice — changeur de voix", WS_OVERLAPPEDWINDOW, 100, 100,
	                       int(720 * scale), int(640 * scale), nullptr, nullptr, wc.hInstance, nullptr);
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
	if (!createDevice(g_hwnd)) {
		cleanupDevice();
		MessageBoxW(nullptr, L"Impossible d'initialiser Direct3D 11.", L"RPVoice", MB_ICONERROR);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return 1;
	}
	ShowWindow(g_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_dev, g_ctx);

	App app;
	g_app = &app;
	AppOptions opt;
	opt.noAudio = noAudio;
	opt.autoStart = !noAudio;
	opt.scale = scale;
	HostHooks hooks;
	hooks.openJson = openJson;
	hooks.saveJson = saveJson;
	app.init(opt, hooks);
	ImGui::GetStyle().FontScaleDpi = scale;

	bool done = false;
	while (!done) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;
		if (app.quitConfirmed()) {
			DestroyWindow(g_hwnd);  // WM_DESTROY -> WM_QUIT
			g_app = nullptr;
			continue;
		}
		if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
			Sleep(10);
			continue;
		}
		g_occluded = false;
		if (g_resizeW != 0 && g_resizeH != 0) {
			cleanupRenderTarget();
			g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
			g_resizeW = g_resizeH = 0;
			createRenderTarget();
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		app.frame();
		ImGui::Render();
		const float clear[4] = {0.09f, 0.09f, 0.10f, 1};
		g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_ctx->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_occluded = g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED;
	}
	g_app = nullptr;
	app.shutdown();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanupDevice();
	if (IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	return 0;
}
