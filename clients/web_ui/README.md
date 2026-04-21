# Web UI + OSC↔WS bridge

A browser-friendly front-end for the AI Accompaniment server. Two processes:

- **[`bridge.py`](bridge.py)** — a Python bridge that re-uses [`clients/python_ref/test_client.py`](../python_ref/test_client.py) as its OSC engine and exposes it to the browser over:
  - HTTP **5173** — serves [`index.html`](index.html) and the static assets.
  - WebSocket **5174** — JSON-RPC channel for every interaction (`connect`, `configure`, `load_model`, `reset`, `verbose`, `probe`, `offline`).
- **[`index.html`](index.html)** — a single-page UI. No build step, no framework.

The browser never speaks OSC directly; the bridge is the only thing that talks UDP to the Python inference server. That keeps the browser free from native permissions, and it also means the UI gets the same `/ready`, `/batch_dropped`, and stem-delivery semantics as the reference client with no duplication.

---

## 1. Install

One-time, into the backend venv:

```powershell
.\musical-accompaniment-ldm\.venv\Scripts\python.exe -m pip install websockets
```

(`numpy` and `soundfile` are already installed for the reference client.)

## 2. Run

Start the inference server first (see [`../../README.md#6-quick-start--end-to-end`](../../README.md#6-quick-start--end-to-end)), then:

```powershell
.\musical-accompaniment-ldm\.venv\Scripts\python.exe clients\web_ui\bridge.py
#  http   listening on http://127.0.0.1:5173/
#  ws     listening on ws://127.0.0.1:5174/ws
```

Open <http://127.0.0.1:5173/>.

## 3. UI walkthrough

| Step | Button / field | What it does |
|---|---|---|
| 1 | **Connect bridge** | Opens the WebSocket to `ws://127.0.0.1:5174/ws`. The pill on the top left turns green. |
| 2 | **Connect OSC** (server host + ports) | Tells the bridge to open its two UDP sockets against the server. Second pill → green when the server's `/ready` is received. |
| 3 | **Configure** (r, w, fade, package-size, stems) | Pushes `/update_r`, `/w`, `/update_fade`, `/update_package_size`, `/predict_instruments` to the server. |
| 4 | **Load model** | Sends `/load_model` and waits (up to 60 s) for the second `/ready`. Third pill → green. |
| 5 | **Load audio** *or* **Load bundled test_tone** | Either upload a 44.1 kHz WAV or use the built-in 8 s test tone. |
| 6 | **Run offline** | Runs N sliding windows of width `r·T` and streams results back as base64-encoded float32 WAVs. |
| — | **Probe** | Sends a `/packet_test`; RTT shown in the event log. |
| — | **Reset** / **Verbose** | `/reset 1` and `/verbose 0/1` respectively. |

All events — bridge log lines, every OSC send/recv, per-window progress — stream into the right-hand **event log**. Generated stems land below the log with an `<audio>` preview and a download link each (`data-testid="dl-bass"`, `dl-drums`, `dl-guitar`, `dl-piano`).

## 4. Testing — Playwright

Every interactive element has a stable `data-testid` attribute:

| `data-testid` | Element |
|---|---|
| `btn-connect-bridge` | Connect bridge button |
| `btn-connect-osc` | Connect OSC button |
| `btn-configure` | Configure button |
| `btn-load` | Load model button |
| `btn-load-test` | Load bundled test tone |
| `btn-offline` | Run offline button |
| `btn-probe` | Probe RTT button |
| `btn-reset` | Reset button |
| `btn-verbose` | Verbose toggle |
| `pill-ws` / `pill-osc` / `pill-model` | Status pills (class `on` when green) |
| `dl-bass` / `dl-drums` / `dl-guitar` / `dl-piano` | Download links for generated stems |
| `log` | Event log `<pre>` |

See [`tests/playwright/test_smoke.py`](../../tests/playwright/test_smoke.py) for a working end-to-end test.

## 5. WebSocket JSON-RPC — for other front-ends

Every message is a JSON object `{op, id?, ...}`. Every reply is `{event, ...}`. The ops:

| op | payload | response events |
|---|---|---|
| `connect` | `{host, send_port, recv_port}` | `log`, `ready` (when server sends `/ready`) |
| `configure` | `{r, w, fade, package_size, stems: [bass,drums,guitar,piano]}` | `log` |
| `load_model` | `{}` | `log`, `ready` (on the second `/ready`) |
| `reset` | `{}` | `log` |
| `verbose` | `{enabled: bool}` | `log` |
| `probe` | `{size}` | `probe {rtt_ms}` |
| `offline` | `{wav_b64, max_windows, stems}` | `window {i,n}` per window, `done {stems: {bass: <b64 wav>, …}}` |

`wav_b64` is a base64-encoded 44.1 kHz WAV (any channels — bridge sums to mono).

## 6. Known limits

- **Offline only** — no live microphone streaming yet. For live input use the Max external or the JUCE plugin.
- **One WS client at a time** — the bridge shares one OSC connection; if two tabs connect, the newer one wins.
- **No server-side auth** — bind to `127.0.0.1` only. Don't expose 5173/5174 on a LAN unless you add a proxy.
- **44.1 kHz WAV input required**. `ffmpeg -i in.mp3 -ar 44100 -ac 2 in.wav` if you need to convert.
