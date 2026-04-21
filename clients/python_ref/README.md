# Reference Python client

A from-scratch, stdlib-only (+ `numpy`, `soundfile`) implementation of `PROTOCOL.md v1.0`. Doubles as:

- **Executable protocol spec** — if this works, the protocol doc is right.
- **Smoke-test harness** — feed a WAV, get per-stem WAVs out, without Max, without a browser, without a DAW.
- **Reference for the web + JUCE ports** — every byte on the wire is built here explicitly.

## Install

```powershell
# from repo root
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install numpy soundfile
```

## Run

Assumes the Python inference server is already running on `127.0.0.1:7000`
(see [../../README.md#6-quick-start--end-to-end](../../README.md#6-quick-start--end-to-end)).

### Round-trip probe (no model, just pings the server)

```powershell
python clients\python_ref\test_client.py probe --server-host 127.0.0.1
```

You should see 5 round-trip times printed. If they time out, the server isn't listening or a firewall is in the way.

### Offline WAV → stems

```powershell
python clients\python_ref\test_client.py offline `
    --input my_song.wav `
    --out-dir out\ `
    --stems bass drums `
    --r 0.25 --w 0 `
    --package-size 5120 `
    --max-windows 4 `
    --verbose
```

Outputs `out\my_song__bass.wav` and `out\my_song__drums.wav`.

Input **must** be 44.1 kHz. Resample first if needed:

```powershell
ffmpeg -i my_song.mp3 -ar 44100 -ac 2 my_song.wav
```

## What it does, step by step

1. Opens two UDP sockets: send → `(host, 7000)`, recv ← `(0.0.0.0, 8000)`.
2. Waits for `/ready T` from the server (5–10 s timeout; non-fatal for `probe`, fatal for `offline`).
3. Pushes params: `/update_package_size`, `/update_r`, `/w`, `/update_fade`, `/predict_instruments`.
4. `/reset`, then **arms a fresh `/ready` event** before sending `/load_model`. Blocks on that second `/ready` for up to 60 s (the server now emits it once the checkpoint is fully read — see [`PROTOCOL.md §3`](../../PROTOCOL.md#3-session-lifecycle)).
5. Sends one `/packet_test` as a warm-up round-trip.
6. For each 6-second window (hop `r·T`):
   - Sums input to mono.
   - Chunks at `package_size=5120` floats/datagram.
   - Sends chunks on `/context` with `(batch_id, start_sample_index, total_chunks, floats…)`. Exactly `step = int(T·r)` fresh samples per batch — **not** a full `T` window.
   - Blocks on a `threading.Event` until every predicted stem has received `total_chunks` chunks.
   - Overlap-adds the result into the output timeline at offset `win_start + w·r·T`.
7. Writes float32 WAVs per stem.

## Limits

- No live audio I/O — this is the smoke test. Live mic/playback happens in the web UI and the JUCE plugin.
- Only `send_mode=0` semantics (`/context`) — per-stem ingress isn't wired in `server_CD.py`. See `PROTOCOL.md §5`.
