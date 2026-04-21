# OSC Wire Protocol — Ghost Note

This document is the **authoritative specification** for the OSC/UDP protocol spoken between a client (e.g. the Max external, the web UI bridge, or the JUCE plugin) and the Python inference server ([`server.py`](musical-accompaniment-ldm/server.py) or [`server_CD.py`](musical-accompaniment-ldm/server_CD.py)).

Reverse-engineered from `multi_track/multi_track.cpp` and `musical-accompaniment-ldm/server_CD.py`. Any discrepancy between this document and those two files is a bug — the code is canonical.

---

## 1. Transport

| | |
|---|---|
| Layer | OSC 1.0 over UDP |
| Endianness | Big-endian (network byte order) — *standard OSC* |
| Server → client default port | **8000** |
| Client → server default port | **7000** |
| Max UDP datagram size (server) | 65536 bytes recv buffer, 4 MiB `SO_RCVBUF` |
| Max UDP datagram size (client) | Default 9216 on macOS — **raise to 65535** via `sysctl -w net.inet.udp.maxdgram=65535` |
| Supported OSC arg types | `i` (int32), `f` (float32), `d` (double → coerced to float), `T`/`F` (bool), `s` (string) |

There is **no OSC bundle support**. Each UDP datagram carries exactly one OSC message.

## 2. Audio conventions

| Parameter | Value | Notes |
|---|---:|---|
| Sample rate | **44100 Hz** | Hard-coded in server config |
| Channels | 1 (mono) | Per stem |
| Context window length `T` | **264 600 samples** (6.0 s) | Hard-coded by checkpoint architecture (latent grid 64×64) |
| Stems | `["bass", "drums", "guitar", "piano"]` | Order matters (index = class label) |
| Sample format on wire | `float32`, big-endian | `flatten_prediction.astype('>f4')` on server, `<< float` on client |

## 3. Session lifecycle

```
┌──────── client ────────┐                 ┌──────── server ────────┐
                         │                 │
                         │                 │  socket.bind(7000)
                     ◄───┤  /ready T       │  (emits /ready after bind)
                         │                 │
  /update_package_size ──►                 │
  /update_r            ──►                 │
  /update_fade         ──►                 │
  /w                   ──►                 │
  /predict_instruments ──►                 │
  /load_model          ──►                 │  loads checkpoint
                         │                 │
  /context chunk 0     ──►                 │  accumulates
  /context chunk 1     ──►                 │
    …                    │                 │
  /context chunk N-1   ──►                 │  all chunks in → auto /predict
                         │                 │  inference runs
                     ◄───┤  /bass chunks   │
                     ◄───┤  /drums chunks  │
                     ◄───┤  /guitar chunks │
                     ◄───┤  /piano chunks  │
                         │                 │
                         │ (repeat predict every T·r samples of new audio)
```

`/ready` fires **twice**: once when the server has bound its socket, and again after `/load_model` finishes reading the checkpoint. Clients may either gate inference on the second `/ready` or time out the `/load_model` call at ~60 s. There is still no separate `/model_loaded` reply — the second `/ready` *is* that signal.

> This is a server-side upgrade (v1.1, 2026-04). The original v1.0 behaviour (single `/ready` at bind) still works for clients that ignore duplicate readies.

## 4. Control messages — client → server

All are fire-and-forget. The server does not ACK control messages.

| Address | Type tag | Args | Default / Range | Description |
|---|---|---|---|---|
| `/load_model` | `,` | — | — | Load the CD/diffusion checkpoint referenced in `configs/for_server/…yaml` |
| `/predict` | `,` | — | — | Force a prediction cycle (normally auto-triggered by chunk arrival) |
| `/reset` | `,i` | `1` | — | Zero all server-side buffers (context, latent, generated) |
| `/print` | `,T` | `true` | — | Dump `context_audio.wav`, `generated_audio.wav`, and PNG plots to server cwd |
| `/verbose` | `,i` | `0`/`1` | 0 | Enable per-stage latency logs on the server |
| `/update_package_size` | `,i` | size (floats/chunk) | `128 … 16384`, default **5120** | Chunk size in *floats*, not bytes |
| `/update_r` | `,f` | step fraction of T | `0.0 … 1.0`, default **0.25** | Prediction window = `r · T` samples |
| `/w` | `,f` | regime | `-1.0`, `0.0`, or `+1.0` | Retrospective / immediate / lookahead |
| `/update_fade` | `,f` | fade ratio | `0.0 … 1.0`, default **0.02** | Crossfade length = `fade · 44100` samples |
| `/predict_instruments` | `,iiii` | 4× one-hot | e.g. `1 0 0 0` | Which stems to **generate** (others are context). Length must match server's `stem_names` |
| `/packet_test` | `,i f…` | size, `size` × random float | — | Round-trip latency probe — server echoes `/packet_test_response` |

**Wire format (control messages):** standard OSC 1.0:

```
[address:padded string][type_tag:padded string][args:packed BE]
```

- Strings are **null-terminated, then padded with NULs to the next 4-byte boundary**.
- Type tag starts with `,` and lists all arg types.
- Args are packed back-to-back, big-endian, each padded to 4 bytes (except `d` → 8).

## 5. Audio ingress — client → server

The server listens on **only one audio address**: `/context`.

> ⚠ Earlier versions of the Max external supported `send_mode 1` (per-stem context on `/bass`, `/drums`, …). `server_CD.py` does **not** handle this path — only `/context` is routed to `buffer_handler`. New clients must send `send_mode 0` semantics: **sum** all non-predicted stems into a single mono plane before sending.

### Datagram layout

```
address : "/context\0"                      (padded to 12 bytes)
type tag: ",iii" + "f" × N + "\0"           (padded to next 4 bytes)
args    : batch_id   (int32 BE)
          start_index(int32 BE)  ← SAMPLE OFFSET, not chunk index
          total_chunks(int32 BE)
          f_0 … f_{N-1} (float32 BE)
```

Where:

- **`batch_id`** — monotonically increasing per prediction cycle (starts at 1). All chunks belonging to the same prediction **must** share a `batch_id`.
- **`start_index`** — `chunk_idx · package_size` (so first chunk is `0`, second is `package_size`, etc.). The server uses this as an absolute offset within the context buffer: `context_audio[0, start_index:start_index+N]`.
- **`total_chunks`** — total number of chunks the server should expect for this `batch_id`. Auto-predict fires when the server has seen `total_chunks` chunks for the current `batch_id`.
- **`N` floats** — `N == package_size` for all chunks **except possibly the last**, which may be shorter. `T` is not required to be a multiple of `package_size`.

### Chunking rules

- `total_chunks = ceil(T_actual / package_size)` where `T_actual ≤ T`.
- For a full 6 s context at `package_size = 5120`, that's `ceil(264600 / 5120) = 52` chunks.
- Chunks **may be sent out of order** — the server uses `start_index`, not arrival order, to place samples. In practice keep them in order for backpressure simplicity.
- Chunks **must not** interleave between different `batch_id`s — the server resets its per-batch counters on the first chunk that carries a new `batch_id`.
- A **watchdog** on the server fires `predict()` early if chunks stop arriving for longer than ≈ `5 × avg_inter_chunk_gap`. Packet loss will not deadlock the server, but will produce zero-filled gaps in the context.

## 6. Audio egress — server → client

For every stem index `i` marked as `1` in the last `/predict_instruments`, the server emits a sequence of OSC messages on `/bass`, `/drums`, `/guitar`, or `/piano`.

### Datagram layout

```
address : "/<stem>\0"                       (padded)
type tag: ",iii" + "f" × N + "\0"           (padded)
args    : batch_id    (int32 BE)
          chunk_idx   (int32 BE)            ← CHUNK INDEX, not sample offset  (⚠ asymmetric with ingress)
          total_chunks(int32 BE)
          f_0 … f_{N-1} (float32 BE)
```

### Reconstruction

- Predicted stem length **per cycle** = `r · T + fade · 44100` samples. For `r=0.25, fade=0.02` that's `66150 + 882 = 67032` samples.
- A short fade-in curve (`headroom_samples = fade · 44100` samples, linear 0→1) is already baked into the first `headroom_samples` of the stream.
- `total_chunks = ceil(67032 / package_size)` → 14 chunks at `package_size=5120`.
- To reassemble: `out[chunk_idx * package_size : chunk_idx * package_size + N] = floats`.

## 7. Server-emitted control messages

| Address | Type tag | Args | Fires when |
|---|---|---|---|
| `/ready` | `,T` | `true` | (1) Server has bound its UDP socket (at startup); (2) `/load_model` has finished reading the checkpoint |
| `/batch_dropped` | `,i` | `batch_id` | Inference was already running for an earlier batch when a new one arrived |
| `/packet_test_response` | `,i f…` | `size`, `size` × random float | Reply to `/packet_test` |

There is intentionally **no** `/server_predicted` message on `server_CD.py` — completion is signalled implicitly by the stem chunks arriving. (The Max external treats the last chunk of the last stem as "done" via chunk counting.)

> The original Max external README mentions `/server_predicted <bool>`. That message exists in older server builds and in `server.py`, but the CD server uses the chunk-counting scheme above.

## 8. Error handling

The server never sends error messages back. Detectable client-side:

| Condition | Client detection |
|---|---|
| Server not running | No `/ready` within timeout; control messages sent but no response |
| Wrong protocol version / corrupt OSC | `[UDP] unknown address:` printed on server stdout; no reply |
| Packet loss during ingress | Server watchdog fires anyway; silence appears in the generated window |
| Concurrent prediction dropped | `/batch_dropped <batch_id>` arrives |
| Checkpoint missing | Server raises on `/load_model`, then stays silent; subsequent `/predict` will error in server stdout |

**Client resilience checklist:**
1. Gate audio sends behind a received `/ready`.
2. Timeout `/load_model` at ≥ 60 s (large `.ckpt` files over cold cache).
3. Treat missing stem chunks for 2 s after last chunk as "batch lost" and recover on the next window.
4. Never send chunks for a batch whose previous cycle is still in flight — the server will `/batch_dropped` it.

## 9. Reference implementations

| Language | Location | Notes |
|---|---|---|
| C++ / Max SDK | [multi_track/multi_track.cpp](multi_track/multi_track.cpp) | Original; uses oscpack |
| Python | [clients/python_ref/test_client.py](clients/python_ref/test_client.py) | WAV-file smoke test (offline). Doubles as this doc's executable spec. |
| Python (bridge) | [clients/web_ui/bridge.py](clients/web_ui/bridge.py) | Wraps the Python reference client behind HTTP+WS so browsers can drive the protocol |
| JavaScript (browser) | [clients/web_ui/index.html](clients/web_ui/index.html) | Uses the bridge above; no direct OSC in the browser |
| C++ / JUCE | [clients/juce_plugin/](clients/juce_plugin/) | VST3 + Standalone; uses `juce::OSCSender` / `juce::OSCReceiver`. Constants mirrored in `Source/ProtocolConstants.h`. |

## 10. Version

Spec version: **1.1** (2026-04-21). Matches `server_CD.py` from this repo.

### Changelog

- **1.1 (2026-04-21):**
  - `/ready` now fires a second time after `/load_model` finishes. Old clients continue to work.
  - `/reset` now also clears the server's per-batch counters (`batch_id`, `auto_chunks_received`, `auto_chunks_expected`, watchdog), allowing a reconnecting client to start its `batch_id` counter from scratch without being treated as a duplicate.
  - Server-side shape bug fix in `predict()`: audio writes now use the actual prediction length `flatten_prediction.shape[0]` instead of the computed `n_needed`, eliminating intermittent `tensor size must match` crashes when `r·T + fade·SR` wasn't a multiple of `package_size`.
- **1.0 (2026-04-20):** Initial spec.
