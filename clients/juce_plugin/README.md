# Ghost Note — CUE Standalone App + VST3 Plugin

## Overview

This directory contains CUE, the JUCE 8 client for Ghost Note. `CUE` stands for `Collaborative Unscripted Ensemble`. It builds as a standalone desktop application and as a VST3 plugin from the same source tree.

CUE captures a 6-second mono context window from the host, streams it over OSC to the [inference server](../../musical-accompaniment-ldm/server_CD.py), and plays back the four predicted stems plus a dry mix on separate output buses. It implements the [OSC protocol v1.1](../../PROTOCOL.md) spoken by [`server_CD.py`](../../musical-accompaniment-ldm/server_CD.py).

CUE and the MAX/MSP external in [`multi_track/`](../../multi_track/) target different use cases: CUE is the DAW-integrated and standalone path, the MAX external is retained for compatibility with the original patch-based workflow from the paper.

The directory provides:

- a standalone application for local use without a DAW
- a VST3 plugin for DAW-based use
- a 5-bus output layout for bass, drums, guitar, piano, and a dry mix of the input

---

## 1. Bus layout

| Bus | Direction | Name | Channels |
|-----|-----------|------|---------:|
| Main in | input  | `Input` | 2 (stereo) |
| Out 0 | output | `Bass` | 2 (stereo) |
| Out 1 | output | `Drums` | 2 (stereo) |
| Out 2 | output | `Guitar` | 2 (stereo) |
| Out 3 | output | `Piano` | 2 (stereo) |
| Out 4 | output | `Mix (dry)` | 2 (stereo) |

Server output is mono; plugin duplicates L = R on each stem bus. Context is mixed to mono by averaging L + R.

## 2. Timing model

- **Context window T**: 6.0 s = 264 600 samples @ 44.1 kHz (fixed by checkpoint).
- **Hop size**: `r · T` samples. Default `r = 0.25` → 66 150 samples (≈ 1.5 s). Every hop, the plugin snapshots its ring buffer, begins a new `batch_id`, and streams 52 chunks of 5120 floats to the server.
- **Latency**: The server returns `r · T + fade · 44100` ≈ 67 032 samples per cycle. You'll hear the first accompaniment bar ~6 s after input starts (one full context fill) plus ~0.5–2 s inference time.

No PDC is reported — this is generative, not corrective, so host delay compensation doesn't apply.

## 3. Sample rate

**Host must run at 44.1 kHz for v1.** The plugin asserts this in `prepareToPlay` and zeroes its outputs if the rate is wrong. Resampling support is a v2 item — see [TODO](#todo).

## 4. Prerequisites

- CMake ≥ 3.22
- A C++20 compiler (MSVC 2022, clang-cl, or Xcode 15)
- Git
- [JUCE](https://github.com/juce-framework/JUCE) 8.x — pulled in as a submodule (see [Build](#5-build))
- Python server running (see [../../README.md#quick-start](../../README.md#quick-start))

## 5. Build

```powershell
# From repo root:
cd clients/juce_plugin

# First time only — fetch JUCE 8.0.4 (not a submodule in this workspace, just a plain clone)
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git JUCE

# Configure + build (VS 2022)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target CUE_VST3 CUE_Standalone
```

> On a clean workspace the bundled CMake from VS 2022 Community works fine:
> `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

Artefacts produced:

| Target | Path | Size |
|---|---|---|
| VST3 | `build/CUE_artefacts/Release/VST3/CUE.vst3` | ~5 MB |
| Standalone | `build/CUE_artefacts/Release/Standalone/CUE.exe` | ~6 MB |

Copy the VST3 to your host's plugin path, e.g. `%CommonProgramFiles%\VST3` on Windows.

## 6. Using it in Reaper

1. Start the server:
   ```powershell
   cd musical-accompaniment-ldm
   .\.venv\Scripts\python.exe server_CD.py --serverport 7000 --clientport 8000 --server_ip 127.0.0.1
   ```
2. In Reaper, make sure **Project rate** is 44.1 kHz (`Project → Settings → Sample rate`).
3. Insert `CUE` on a track carrying your live input.
4. In the plugin UI, click **Connect** (defaults: server `127.0.0.1:7000`, listen `8000`), then **Load Model**.
5. Route the plugin's extra output buses to new tracks (Reaper: right-click plugin → *Pin connections*).

## 7. Troubleshooting

| Symptom | Likely cause |
|---|---|
| All outputs silent, UI shows "waiting for /ready" | Server not running or firewalled on UDP 8000 |
| UI shows "SR mismatch: 48000" | Host project is at 48 kHz — change to 44.1 kHz |
| Stems stutter / gaps | Inference takes longer than `r · T` — raise `r` or use the CD (distilled) model, not the full diffusion one |
| `/batch_dropped` toasts in UI | Same as above — you're pushing batches faster than the server can consume |

## 8. TODO

- [ ] libsamplerate / `juce::Interpolators` resampling so the plugin accepts any host SR
- [ ] Save/restore server address and model choice in plugin state
- [ ] Optional MIDI control for the 4 one-hot predict flags
- [ ] Graceful reconnect on dropped UDP
- [ ] AU / AAX builds

## 9. Layout

```
clients/juce_plugin/
├── CMakeLists.txt
├── README.md  (you're here)
├── .gitignore
├── JUCE/                       (plain clone — not committed; see §5)
└── Source/
    ├── ProtocolConstants.h     — single source of truth, mirrors PROTOCOL.md
    ├── ContextRingBuffer.h     — lock-free mono context accumulator
    ├── StemOutputBuffer.h      — per-stem egress FIFO consumed by processBlock
    ├── OscBridge.h/.cpp        — OSC send/receive on a background thread
    ├── PluginProcessor.h/.cpp  — AudioProcessor
    └── PluginEditor.h/.cpp     — minimal UI (connect / load model / per-stem meters)
```
