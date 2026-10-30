#!/bin/bash
# Build RPVoice for Windows (mingw-w64 cross, fully static, GUI subsystem) and/or macOS
# (native clang + GLFW from Homebrew, for local UI testing).
# Usage: ./build.sh [win|mac|all]   (default: all). Outputs: dist/RPVoice.exe, dist/RPVoice-mac.
set -euo pipefail
cd "$(dirname "$0")"
target=${1:-all}
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

INC=(-Isrc -Ivendor -Ivendor/imgui -Ivendor/imgui/backends -Ivendor/signalsmith)
# No SIGNALSMITH_USE_* define: Signalsmith uses its built-in generic FFT (see README, Performance).
FLAGS=(-std=c++17 -O2 -DNDEBUG -Wall -Wextra)
COMMON=(src/ui.cpp src/audio.cpp src/presets.cpp src/selftest.cpp src/miniaudio_impl.cpp
	vendor/imgui/imgui.cpp vendor/imgui/imgui_draw.cpp vendor/imgui/imgui_tables.cpp vendor/imgui/imgui_widgets.cpp)

# compile <cxx> <objdir> <extra flags...> -- <sources...>; parallel, stops on the first error.
compile() {
	local cxx=$1 dir=$2; shift 2
	local extra=()
	while [[ $1 != -- ]]; do extra+=("$1"); shift; done; shift
	mkdir -p "$dir"
	local pids=() objs=()
	for src in "$@"; do
		local obj="$dir/$(basename "${src%.cpp}").o"
		objs+=("$obj")
		# Rebuild when the source or any project header is newer than the object.
		if [[ ! -f $obj || $src -nt $obj || -n $(find src -name '*.h' -newer "$obj" | head -1) ]]; then
			"$cxx" "${FLAGS[@]}" "${extra[@]}" "${INC[@]}" -c "$src" -o "$obj" &
			pids+=($!)
			while (( $(jobs -rp | wc -l) >= jobs )); do sleep 0.2; done
		fi
	done
	local ok=1
	for p in "${pids[@]+"${pids[@]}"}"; do wait "$p" || ok=0; done
	(( ok )) || { echo "compile failed ($dir)"; exit 1; }
	OBJS=("${objs[@]}")
}

mkdir -p dist

if [[ $target == win || $target == all ]]; then
	command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "missing mingw: brew install mingw-w64"; exit 1; }
	compile x86_64-w64-mingw32-g++ build/win -DNOMINMAX -D_WIN32_WINNT=0x0A00 -- "${COMMON[@]}" src/main_win.cpp \
		vendor/imgui/backends/imgui_impl_win32.cpp vendor/imgui/backends/imgui_impl_dx11.cpp
	x86_64-w64-mingw32-g++ "${OBJS[@]}" -o dist/RPVoice.exe -mwindows -static -static-libgcc -static-libstdc++ \
		-ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lgdi32 -lcomdlg32 -lshell32 -limm32 -lole32 -lwinmm -luuid
	x86_64-w64-mingw32-strip dist/RPVoice.exe
	echo "built dist/RPVoice.exe ($(du -h dist/RPVoice.exe | cut -f1))"
fi

if [[ $target == mac || $target == all ]]; then
	BREW=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
	[[ -f $BREW/include/GLFW/glfw3.h ]] || { echo "missing GLFW: brew install glfw"; exit 1; }
	compile clang++ build/mac -I"$BREW/include" -- "${COMMON[@]}" src/main_mac.cpp \
		vendor/imgui/backends/imgui_impl_glfw.cpp vendor/imgui/backends/imgui_impl_opengl3.cpp
	clang++ "${OBJS[@]}" -o dist/RPVoice-mac -L"$BREW/lib" -lglfw \
		-framework OpenGL -framework Cocoa -framework IOKit \
		-framework CoreFoundation -framework CoreAudio -framework AudioToolbox -lpthread
	echo "built dist/RPVoice-mac"
fi
