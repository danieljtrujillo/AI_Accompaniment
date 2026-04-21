"""WebSocket + HTTP bridge between the browser UI and the multi_track OSC server.

Serves:
  - GET /                    → index.html
  - GET /test_tone_8s.wav    → the bundled smoke-test file (if present)
  - WS  /ws                  → control channel (JSON messages)

The bridge wraps the existing :class:`AccompanimentClient` from
``clients/python_ref/test_client.py`` so the browser does not have to speak
OSC. Every command the Python reference supports (connect, probe,
load_model, reset, verbose, predict one hop) is exposed as a JSON RPC.

Wire format (client → bridge):
    {"op": "connect",  "server_host": "127.0.0.1", "server_port": 7000,
                       "recv_port": 8000}
    {"op": "configure", "r": 0.25, "w": 1, "fade": 0.1, "package_size": 5120,
                        "stems": ["bass", "drums"]}
    {"op": "load_model"}
    {"op": "reset"}
    {"op": "verbose",  "on": true}
    {"op": "probe",    "size": 1024}
    {"op": "predict",  "audio_b64": "<base64 float32 mono @44100>"}
    {"op": "offline",  "audio_b64": "...", "max_windows": 1}

Wire format (bridge → client) — always JSON:
    {"ev": "log",     "msg": "..."}
    {"ev": "ready",   "ok": true}
    {"ev": "probe",   "rtt_ms": 1.9}
    {"ev": "stem",    "name": "bass",  "samples": [...]}   # float list
    {"ev": "window",  "index": 0, "total": 1}
    {"ev": "done",    "files": {"bass": "...b64 WAV...", ...}}
    {"ev": "error",   "msg": "..."}
"""

from __future__ import annotations

import asyncio
import base64
import io
import json
import logging
import os
import sys
import threading
import wave
from pathlib import Path
from typing import Optional

import numpy as np

# -- make the Python reference client importable ------------------------------
_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(_ROOT / "clients" / "python_ref"))
from test_client import (  # type: ignore  # noqa: E402
    AccompanimentClient,
    OscClient,
    T_SAMPLES,
    SR,
    DEFAULT_R,
    DEFAULT_W,
    DEFAULT_FADE,
    DEFAULT_PACKAGE_SIZE,
    STEM_NAMES,
)

import websockets
from websockets.asyncio.server import ServerConnection, serve as ws_serve


log = logging.getLogger("bridge")


# -----------------------------------------------------------------------------
# Session — one browser tab's view of the OSC server
# -----------------------------------------------------------------------------

class Session:
    """Thin async-friendly wrapper around AccompanimentClient."""

    def __init__(self, ws: ServerConnection, loop: asyncio.AbstractEventLoop):
        self.ws = ws
        self.loop = loop
        self.osc: Optional[OscClient] = None
        self.client: Optional[AccompanimentClient] = None

    # --- outgoing ------------------------------------------------------------

    def emit_sync(self, ev: str, **kw) -> None:
        """Thread-safe emit from any worker thread."""
        msg = json.dumps({"ev": ev, **kw})
        asyncio.run_coroutine_threadsafe(self.ws.send(msg), self.loop)

    async def emit(self, ev: str, **kw) -> None:
        await self.ws.send(json.dumps({"ev": ev, **kw}))

    # --- lifecycle -----------------------------------------------------------

    def connect(self, server_host: str, server_port: int, recv_port: int) -> None:
        if self.osc is not None:
            try: self.osc.close()
            except Exception: pass
        osc = OscClient(server_host=server_host, server_port=server_port,
                        recv_host="0.0.0.0", recv_port=recv_port)
        self.osc = osc
        self.client = AccompanimentClient(osc)
        # Pipe /ready into the UI
        osc.on("/ready", lambda addr, *a: self.emit_sync("ready", ok=True))
        self.emit_sync("log", msg=f"connected to {server_host}:{server_port} ← recv {recv_port}")

    def close(self) -> None:
        if self.osc is not None:
            try: self.osc.close()
            except Exception: pass

    # --- operations ----------------------------------------------------------

    def configure(self, r: float, w: int, fade: float, package_size: int,
                  stems: list) -> None:
        assert self.client is not None
        self.client.r = r
        self.client.w = w
        self.client.fade = fade
        self.client.package_size = package_size
        self.client.stems_to_predict = tuple(stems)
        self.client.configure_server()
        self.emit_sync("log", msg=f"configure: r={r} w={w} fade={fade} pkg={package_size} stems={stems}")

    def load_model(self) -> None:
        assert self.client is not None
        self.client.arm_ready()
        self.client.load_model()
        self.emit_sync("log", msg="load_model: request sent")

    def reset(self) -> None:
        assert self.client is not None
        self.client.reset()
        self.emit_sync("log", msg="reset: sent /reset to server")

    def verbose(self, on: bool) -> None:
        assert self.client is not None
        self.client.verbose(on)
        self.emit_sync("log", msg=f"verbose: {on}")

    def probe(self, size: int) -> None:
        assert self.client is not None
        rtt = self.client.packet_probe(size=size, timeout=3.0)
        self.emit_sync("probe", rtt_ms=rtt if rtt is not None else -1.0,
                       size=size)

    def offline(self, audio: np.ndarray, max_windows: int) -> None:
        """Sliding-window inference over `audio` (mono float32 @44100)."""
        assert self.client is not None
        r = self.client.r
        step = int(T_SAMPLES * r)
        n_windows = max(1, (len(audio) - T_SAMPLES) // step + 1)
        n_windows = min(n_windows, max_windows)

        self.emit_sync("log", msg=f"sliding {n_windows} window(s)  T={T_SAMPLES/SR:.1f}s  step={step/SR:.2f}s")

        # Accumulators per stem
        out: dict = {s: [] for s in self.client.stems_to_predict}

        for w in range(n_windows):
            win_end = T_SAMPLES + w * step
            hop = audio[win_end - step: win_end]
            if len(hop) < step:
                hop = np.pad(hop, (0, step - len(hop)))
            self.emit_sync("window", index=w, total=n_windows)
            stems = self.client.predict(hop.astype(np.float32), timeout=60.0)
            for name, arr in stems.items():
                out[name].append(arr)
                self.emit_sync("log",
                               msg=f"  window {w+1}/{n_windows}  {name}={len(arr)} samples  peak={float(np.max(np.abs(arr))):.3f}")

        # Concatenate & encode each stem as base64 WAV
        files = {}
        for name, pieces in out.items():
            if not pieces:
                continue
            concat = np.concatenate(pieces).astype(np.float32)
            buf = io.BytesIO()
            with wave.open(buf, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(SR)
                pcm = np.clip(concat, -1.0, 1.0)
                wf.writeframes((pcm * 32767.0).astype("<i2").tobytes())
            files[name] = base64.b64encode(buf.getvalue()).decode("ascii")
        self.emit_sync("done", files=files)


# -----------------------------------------------------------------------------
# WebSocket handler
# -----------------------------------------------------------------------------

async def _ws_handler(ws: ServerConnection) -> None:
    loop = asyncio.get_running_loop()
    sess = Session(ws, loop)
    log.info("ws connected")
    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except Exception as e:
                await sess.emit("error", msg=f"bad json: {e}")
                continue
            op = msg.get("op")
            try:
                await _dispatch(sess, op, msg)
            except Exception as e:
                await sess.emit("error", msg=f"{op}: {e}")
    finally:
        sess.close()
        log.info("ws closed")


async def _dispatch(sess: Session, op: str, msg: dict) -> None:
    if op == "connect":
        sess.connect(msg.get("server_host", "127.0.0.1"),
                     int(msg.get("server_port", 7000)),
                     int(msg.get("recv_port", 8000)))
    elif op == "configure":
        sess.configure(float(msg.get("r", DEFAULT_R)),
                       int(msg.get("w", DEFAULT_W)),
                       float(msg.get("fade", DEFAULT_FADE)),
                       int(msg.get("package_size", DEFAULT_PACKAGE_SIZE)),
                       list(msg.get("stems", ["bass", "drums"])))
    elif op == "load_model":
        sess.load_model()
    elif op == "reset":
        sess.reset()
    elif op == "verbose":
        sess.verbose(bool(msg.get("on", True)))
    elif op == "probe":
        # Probe is synchronous + waits — run off-loop
        await asyncio.to_thread(sess.probe, int(msg.get("size", 1024)))
    elif op == "offline":
        raw_audio = base64.b64decode(msg["audio_b64"])
        audio = np.frombuffer(raw_audio, dtype=np.float32).copy()
        await asyncio.to_thread(sess.offline, audio, int(msg.get("max_windows", 1)))
    else:
        await sess.emit("error", msg=f"unknown op: {op}")


# -----------------------------------------------------------------------------
# Static file server (HTTP) — separate tiny thread, no framework dependency
# -----------------------------------------------------------------------------

import http.server
import functools

_STATIC_DIR = Path(__file__).resolve().parent
_WAV_DIR    = _ROOT


class _StaticHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.debug("http %s - %s", self.address_string(), fmt % args)

    def translate_path(self, path):
        # Route /test_tone_8s.wav from workspace root; everything else from web_ui/
        p = path.split("?", 1)[0].lstrip("/")
        if p == "test_tone_8s.wav":
            return str(_WAV_DIR / "test_tone_8s.wav")
        if p == "" or p == "index.html":
            return str(_STATIC_DIR / "index.html")
        return str(_STATIC_DIR / p)


def _run_http(port: int) -> None:
    handler = functools.partial(_StaticHandler, directory=str(_STATIC_DIR))
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as httpd:
        log.info("http  listening on http://127.0.0.1:%d/", port)
        httpd.serve_forever()


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

async def _main(http_port: int, ws_port: int) -> None:
    threading.Thread(target=_run_http, args=(http_port,), daemon=True).start()
    # Raise frame size to handle ~1-2 MB audio uploads and ~64 MB stem responses
    async with ws_serve(_ws_handler, "127.0.0.1", ws_port,
                        max_size=128 * 1024 * 1024):
        log.info("ws    listening on ws://127.0.0.1:%d/ws", ws_port)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    logging.basicConfig(
        level=os.environ.get("BRIDGE_LOG", "INFO"),
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--http-port", type=int, default=5173)
    ap.add_argument("--ws-port",   type=int, default=5174)
    args = ap.parse_args()
    try:
        asyncio.run(_main(args.http_port, args.ws_port))
    except KeyboardInterrupt:
        pass
