# RPVoice v1 — real-time voice changer (Windows)

Desktop voice changer for role-play (Skyrim RP). Mic → voice chain → VB-Cable ("CABLE Input"),
the game / Discord / Audacity uses "CABLE Output" as microphone. Optional monitor output to a
headset. UI in French (end user), code and docs in English.

The DSP is `src/voice_chain.h`, extended from `../wintest/voice_chain.h` (validated on the
Windows PC). With static parameters and default "advanced" values it is **bit-identical** to
wintest (see Verification).

## Build

Requirements on the Mac: `brew install mingw-w64 glfw`.

```
./build.sh win    # dist/RPVoice.exe   (x86_64, -static, -mwindows, stripped)
./build.sh mac    # dist/RPVoice-mac   (GLFW + OpenGL 3.2, local UI testing)
./build.sh        # both
```

Objects go to `build/<target>/` (rebuilt when the source or a `src/*.h` changes).
The exe imports only Windows system DLLs: kernel32, user32, gdi32, shell32, comdlg32, d3d11,
d3dcompiler_47, dwmapi and the Universal CRT `api-ms-win-crt-*` (part of Windows 10/11; wintest
used the same CRT).

## Command line

```
RPVoice                                   GUI (Windows: audio starts automatically)
RPVoice --no-audio                        GUI, never opens an audio device
RPVoice --selftest in.wav out.wav <preset|0|file.json> [intensity]   offline render + stats
RPVoice --test-presets                    preset JSON round-trip test (temp dir only)
RPVoice --test-smooth in.wav out.wav      parameter-automation click test
RPVoice-mac --no-audio --screenshot x.bmp  (mac only) render 30 frames, dump framebuffer, exit
```

On Windows the headless modes attach to the parent console (`AttachConsole`), so run them from
`cmd` / PowerShell. The Mac build never starts audio by itself (feedback risk): use `--no-audio`
or the "Démarrer l'audio" button with headphones.

## Architecture

```
src/voice_chain.h    DSP chain (header-only, no allocation after prepare())
src/audio.{h,cpp}    miniaudio engine: duplex device + optional monitor device, stats
src/presets.{h,cpp}  VoiceDef (values + on/off flags), preset JSON files, settings.json
src/ui.{h,cpp}       Dear ImGui interface (platform-agnostic)
src/selftest.cpp     headless modes + command-line parsing
src/main_win.cpp     Win32 + Direct3D 11 host, native file dialogs (GetOpen/SaveFileNameW)
src/main_mac.cpp     GLFW + OpenGL 3 host (import/export use a path text field)
vendor/              imgui v1.92.9b, miniaudio, signalsmith-stretch + linear, nlohmann/json 3.12.0,
                     Roboto Regular (Apache 2.0) embedded compressed in fonts/roboto_font.h
```

**Audio thread.** The duplex callback (48 kHz mono, 256-frame period, WASAPI shared on Windows)
reads the latest `Control` block from a lock-free triple buffer (UI → audio), applies the input
gain (ramped), runs `Chain::process()`, applies the output gain (ramped), updates peak meters and
writes the result × monitor volume into a miniaudio `ma_pcm_rb` (SPSC lock-free ring, 250 ms).
The monitor device's callback reads it, waits for a 512-frame cushion, drops excess beyond
~3× the cushion (the two device clocks drift) and counts underruns. Changing a device stops and
re-opens everything (`Engine::start`); the chain is re-prepared outside the audio thread.

**Parameters and smoothing.** `voice::Params` = the 12 character parameters (scaled by the
intensity through `mix()`, unchanged from lab.py) + 7 shape parameters that were constants in
wintest (purr rate, sub low-pass cutoff, rasp rate, chorus rate/depth, reverb room size, input
high-pass), not scaled by the intensity. `Chain::setParams()` only stores a target. Each processed
block (≤ 256 samples) moves the current values toward the target with a one-pole (~30 ms);
gain-like values (sub, rasp, purr, drive, chorus mix, reverb wet) are linearly interpolated per
sample inside the block; EQ coefficients, cutoffs and the Signalsmith pitch/formant are updated
once per block; the purr oscillator keeps its phase continuous when its rate changes. Bypass
("Voix originale") cross-fades 10 ms to the dry signal (dry still goes through the +8 dB / HPF /
compressor / limiter like wintest's preset 0).

**On/off flags.** A disabled parameter is replaced by its neutral value (0, formant 1) before the
intensity mix. Shape parameters have no on/off flag (they only shape an effect that has one).

**Presets.** Factory presets = the 9 `voice::kPresets` (read-only; editing one in memory is
allowed, "Enregistrer copie"/Ctrl+S saves it as a user preset). User presets: one JSON file each in
`%APPDATA%\RPVoice\presets\` (mac: `~/Library/Application Support/RPVoice/presets/`). Each preset
keeps a stored and a working copy, so switching presets is instant and does not lose unsaved edits
(shown with `*`); quitting with unsaved edits asks for confirmation. Files are written to `.tmp`
then atomically replaced.

**Settings.** `settings.json` next to the `presets` folder: devices (by name), monitor on/volume,
selected preset, intensity, bypass, input/output gain, "Avancé" open state. Saved ~1 s after a
change and on exit.

## Preset schema (version 1)

```json
{
  "app": "RPVoice",
  "version": 1,
  "name": "khajiit-marchand (perso)",
  "params": {
    "pitch": -1, "formant": 0.9, "sub": 0.1, "rasp": 0.15, "purr": 0.35,
    "low": 2, "lowmid": 3, "presence": -1, "air": -2, "drive": 0.25, "chorus": 0.15, "reverb": 0,
    "purrRate": 25, "subCutoff": 1200, "raspRate": 70, "chorusRate": 0.6, "chorusDepth": 0.15,
    "reverbRoom": 0.5, "hpf": 70
  },
  "enabled": { "pitch": true, "formant": true, "...": true }
}
```

| key | unit / range | neutral | meaning |
|---|---|---|---|
| pitch | semitones −12..12 | 0 | Signalsmith transpose |
| formant | factor 0.70..1.40 (UI shows %) | 1 | formant factor (pitch-compensated) |
| sub, rasp, purr, drive, chorus, reverb | 0..1 (UI %) | 0 | effect amounts |
| low, lowmid, presence, air | dB −12..12 | 0 | shelf 150 Hz, peak 350 Hz, peak 3 kHz, shelf 8 kHz |
| purrRate | Hz 10..40 | 25 | purr modulation rate |
| subCutoff | Hz 300..4000 | 1200 | sub layer low-pass |
| raspRate | Hz 20..200 | 70 | roughness noise rate |
| chorusRate | Hz 0.1..3 | 0.6 | chorus LFO |
| chorusDepth | 0..0.5 | 0.15 | chorus modulation depth |
| reverbRoom | 0..1 | 0.5 | Freeverb room size |
| hpf | Hz 20..200 | 70 | input high-pass |

Loading is tolerant: missing keys take the defaults above (`enabled` defaults to true), values are
clamped to the ranges, unknown keys are ignored, `version` > 1 is rejected. `name` missing → file
name. Floats are written with the shortest decimal that round-trips to the same `float`, so
save → load is exact (`--test-presets`).

## Verification (done on the Mac)

- `--selftest rec/take4.wav … khajiit-marchand 1.0`: rms 0.05360, peak 0.43680, 0 NaN, 0 jumps —
  output WAV **byte-identical** to `wintest --selftest` (also checked: feminin-c, draugr, original
  at 0.6).
- `--test-presets`: 13/13 pass (9 factory round-trips, accented name, partial file, bad version,
  invalid JSON).
- `--test-smooth`: preset switches / sub toggle / bypass / intensity and EQ jumps every 0.5 s:
  max sample step 0.101 smoothed vs 0.179 without smoothing (static render: 0.110); 0 jumps > 0.3.

## Performance

wintest showed ~42 % CPU on the Windows PC (to confirm which figure: wintest prints the *peak*
callback load, not the average, and Task Manager shows something else). In RPVoice the status bar
shows **CPU moy.** = sum of callback processing time / sum of period durations over the last
second, and **pointe** = worst single callback (processing time / its period) over the last
second. Above 100 % a callback misses its deadline.

Likely causes, in order:
1. **Two Signalsmith instances.** The sub layer is a second full STFT shifter. Done (trivial fix):
   it is no longer processed while the sub layer is off/0 (target and ramp at 0). On re-enable it
   is `reset()`, and its gain stays at 0 for its latency (50 ms) before the normal ramp fades it
   in, so no stale audio or click. Offline: original-voice selftest 1.3 % → 0.5 % realtime,
   feminin-c (sub 0) 1.9 % → 1.1 %.
2. **Generic non-SIMD FFT.** No `SIGNALSMITH_USE_*` backend is defined, so Signalsmith uses its
   portable FFT. Options: `SIGNALSMITH_USE_PFFFT` (vendor pffft, SSE) on Windows, or IPP.
   `-O2` only, no `-march`; `-O3 -msse4.2/-mavx2` is a cheap experiment.
3. Peak vs average: 50 ms block / 12 ms interval means the STFT work is bursty (one big FFT every
   ~2.25 callbacks), so the *peak* load is naturally much higher than the average. Signalsmith's
   `splitComputation` option spreads it across callbacks (adds one interval of latency).
4. The sub shifter could share the main analysis (one STFT, two resyntheses) — larger change.

## Known limitations

- Monitor output adds its own buffering (~10–20 ms) and drifts relative to the main output; the
  monitor callback drops samples to keep it bounded (tiny click on resync, rare).
- Devices are remembered by name; if two devices share a name, the first wins.
- The Mac host has no native file dialog (path text field).
