// selftest.h - headless modes (no window, no live audio).
#pragma once

// --selftest in.wav out.wav <preset|0|file.json> [intensity]
int runSelftest(const char *inPath, const char *outPath, const char *presetArg, float intensity);
// --test-presets : JSON round-trip of every factory preset + edge cases (temp directory only).
int runPresetTest();
// --test-smooth in.wav out.wav : parameter automation click test.
int runSmoothTest(const char *inPath, const char *outPath);

// Command line: returns an exit code for headless modes, or -1 to start the GUI.
// GUI flags: --no-audio (never open audio devices). Prints usage on unknown arguments.
int runCli(int argc, char **argv, bool &noAudio);
