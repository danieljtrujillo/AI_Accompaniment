"""
Reference Python client for the AI Accompaniment server.

Implements PROTOCOL.md v1.0 from scratch using only stdlib + numpy + soundfile.
No python-osc dependency — we hand-build OSC datagrams so this file also
serves as an executable form of the protocol spec.

Usage (offline smoke test):
    python test_client.py offline --input song.wav --out-dir out/ \
        --stems bass drums --server-host 127.0.0.1

    # Defaults to generating bass and drums from the mixture of guitar+piano.

Usage (round-trip probe only, no model inference):
    python test_client.py probe --server-host 127.0.0.1

Author: generated alongside the master README / PROTOCOL.md
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time
from pathlib import Path
from queue import Queue, Empty
from typing import Dict, List, Optional, Tuple

import numpy as np

try:
    import soundfile as sf
except ImportError:  # pragma: no cover
    sys.stderr.write("soundfile required: pip install soundfile\n")
    raise


# -----------------------------------------------------------------------------
# Protocol constants (see PROTOCOL.md §2)
# -----------------------------------------------------------------------------

SR = 44100
T_SAMPLES = 264_600          # 6.0 s context window
STEM_NAMES = ("bass", "drums", "guitar", "piano")
DEFAULT_PACKAGE_SIZE = 5120  # floats per UDP chunk
DEFAULT_R = 0.25             # fraction of T to predict
DEFAULT_W = 0                # -1 retrospective | 0 immediate | +1 lookahead
DEFAULT_FADE = 0.02          # crossfade fraction of SR


# -----------------------------------------------------------------------------
# Minimal OSC 1.0 encoder / decoder (big-endian)
# -----------------------------------------------------------------------------

def _pad4(b: bytes) -> bytes:
    """Null-pad to next 4-byte boundary."""
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def _osc_string(s: str) -> bytes:
    return _pad4(s.encode("utf-8") + b"\x00")


def osc_encode(address: str, *args) -> bytes:
    """Encode an OSC message. Supports int (i), float (f), bool (T/F), str (s)."""
    tag = ","
    payload = b""
    for a in args:
        if isinstance(a, bool):
            tag += "T" if a else "F"
        elif isinstance(a, int):
            tag += "i"
            payload += struct.pack(">i", a)
        elif isinstance(a, float):
            tag += "f"
            payload += struct.pack(">f", a)
        elif isinstance(a, str):
            tag += "s"
            payload += _osc_string(a)
        else:
            raise TypeError(f"Unsupported OSC arg type: {type(a).__name__}")
    return _osc_string(address) + _osc_string(tag) + payload


def osc_encode_chunk(address: str, batch_id: int, middle_int: int,
                     total_chunks: int, floats: np.ndarray) -> bytes:
    """Encode an audio chunk message (fast path — avoids the generic encoder)."""
    n = len(floats)
    tag = "," + "i" * 3 + "f" * n
    header = struct.pack(">iii", batch_id, middle_int, total_chunks)
    body = floats.astype(">f4").tobytes()
    return _osc_string(address) + _osc_string(tag) + header + body


def osc_decode(data: bytes) -> Tuple[str, List]:
    """Minimal OSC decoder — returns (address, args). Same type subset as encoder."""
    end = data.index(b"\x00", 0)
    address = data[:end].decode("utf-8")
    offset = (end + 4) & ~3

    end = data.index(b"\x00", offset)
    tag = data[offset:end].decode("utf-8")
    offset = (end + 4) & ~3

    args: List = []
    for t in tag[1:]:  # skip leading ','
        if t == "i":
            args.append(struct.unpack_from(">i", data, offset)[0]); offset += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", data, offset)[0]); offset += 4
        elif t == "d":
            args.append(struct.unpack_from(">d", data, offset)[0]); offset += 8
        elif t == "T":
            args.append(True)
        elif t == "F":
            args.append(False)
        elif t == "s":
            e2 = data.index(b"\x00", offset)
            args.append(data[offset:e2].decode("utf-8"))
            offset = (e2 + 4) & ~3
        else:
            # Unknown type — bail cleanly
            break
    return address, args


def osc_decode_chunk(data: bytes) -> Tuple[str, int, int, int, np.ndarray]:
    """Fast-path decoder for audio chunks. Returns (address, batch_id, chunk_idx_or_start, total_chunks, floats)."""
    end = data.index(b"\x00", 0)
    address = data[:end].decode("utf-8")
    offset = (end + 4) & ~3

    # Skip the type tag
    end = data.index(b"\x00", offset)
    offset = (end + 4) & ~3

    batch_id, mid, total_chunks = struct.unpack_from(">iii", data, offset)
    offset += 12
    n = (len(data) - offset) // 4
    floats = np.frombuffer(data[offset:offset + n * 4], dtype=">f4").astype(np.float32)
    return address, batch_id, mid, total_chunks, floats


# -----------------------------------------------------------------------------
# OSC client — manages two sockets (send + recv) and a background RX thread
# -----------------------------------------------------------------------------

class OscClient:
    """Bidirectional OSC/UDP client.

    - `sock_send` sends to (server_host, server_port)
    - `sock_recv` is bound to (recv_host, recv_port) and spawns a RX thread
    """

    def __init__(self,
                 server_host: str = "127.0.0.1",
                 server_port: int = 7000,
                 recv_host: str = "0.0.0.0",
                 recv_port: int = 8000):
        self.server_addr = (server_host, server_port)

        self.sock_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Give the kernel a generous send buffer — 52+ chunks/batch at 20 KB each
        try:
            self.sock_send.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
        except OSError:
            pass

        self.sock_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock_recv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock_recv.bind((recv_host, recv_port))

        self.handlers: Dict[str, list] = {}
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._rx_thread.start()

    # --- public API -----------------------------------------------------------

    def send(self, address: str, *args) -> None:
        self.sock_send.sendto(osc_encode(address, *args), self.server_addr)

    def send_chunk(self, address: str, batch_id: int, middle_int: int,
                   total_chunks: int, floats: np.ndarray) -> None:
        self.sock_send.sendto(
            osc_encode_chunk(address, batch_id, middle_int, total_chunks, floats),
            self.server_addr,
        )

    def on(self, address: str, fn) -> None:
        """Register a handler. Signature: fn(address:str, *args)."""
        self.handlers.setdefault(address, []).append(fn)

    def close(self) -> None:
        self._running = False
        try: self.sock_send.close()
        except Exception: pass
        try: self.sock_recv.close()
        except Exception: pass

    # --- internals ------------------------------------------------------------

    def _rx_loop(self) -> None:
        while self._running:
            try:
                data, _ = self.sock_recv.recvfrom(65536)
            except OSError:
                return
            try:
                # Peek address only — dispatch to chunk-aware or generic path
                end = data.index(b"\x00", 0)
                address = data[:end].decode("utf-8", errors="replace")
            except ValueError:
                continue
            fns = self.handlers.get(address, [])
            if not fns:
                continue
            if address in {"/bass", "/drums", "/guitar", "/piano",
                           "/packet_test_response"}:
                try:
                    addr, batch_id, mid, total, floats = osc_decode_chunk(data)
                except Exception as e:
                    print(f"[rx] bad chunk on {address}: {e}", file=sys.stderr)
                    continue
                for fn in fns:
                    fn(addr, batch_id, mid, total, floats)
            else:
                try:
                    addr, args = osc_decode(data)
                except Exception as e:
                    print(f"[rx] bad packet on {address}: {e}", file=sys.stderr)
                    continue
                for fn in fns:
                    fn(addr, *args)


# -----------------------------------------------------------------------------
# High-level accompaniment client
# -----------------------------------------------------------------------------

class AccompanimentClient:
    """Implements the predict() cycle end-to-end: send context, collect stems."""

    def __init__(self, osc: OscClient, *,
                 package_size: int = DEFAULT_PACKAGE_SIZE,
                 r: float = DEFAULT_R,
                 w: float = DEFAULT_W,
                 fade: float = DEFAULT_FADE,
                 stems_to_predict: Tuple[str, ...] = ("bass", "drums")):
        self.osc = osc
        self.package_size = package_size
        self.r = r
        self.w = w
        self.fade = fade
        self.stems_to_predict = stems_to_predict

        self._ready = threading.Event()
        self._batch_id = 0
        self._responses: Dict[int, Dict[str, Dict[int, np.ndarray]]] = {}
        self._response_totals: Dict[int, Dict[str, int]] = {}
        self._batch_done_events: Dict[int, threading.Event] = {}
        self._lock = threading.Lock()

        osc.on("/ready", self._on_ready)
        for s in STEM_NAMES:
            osc.on(f"/{s}", self._on_stem_chunk)
        osc.on("/batch_dropped", self._on_batch_dropped)
        osc.on("/packet_test_response", self._on_packet_test_response)

        self._probe_event = threading.Event()
        self._probe_result: Optional[Tuple[int, int]] = None  # (size, floats_received)

    # --- setup ---------------------------------------------------------------

    def wait_ready(self, timeout: float = 5.0) -> bool:
        return self._ready.wait(timeout)

    def arm_ready(self) -> None:
        """Clear the /ready event so the next wait_ready blocks for a fresh signal."""
        self._ready.clear()

    def configure_server(self) -> None:
        """Push current params to the server."""
        self.osc.send("/update_package_size", int(self.package_size))
        self.osc.send("/update_r", float(self.r))
        self.osc.send("/w", float(self.w))
        self.osc.send("/update_fade", float(self.fade))
        # One-hot vector (length = len(STEM_NAMES))
        flags = [1 if s in self.stems_to_predict else 0 for s in STEM_NAMES]
        self.osc.send("/predict_instruments", *flags)

    def load_model(self) -> None:
        self.osc.send("/load_model")

    def reset(self) -> None:
        self.osc.send("/reset", 1)

    def verbose(self, on: bool) -> None:
        self.osc.send("/verbose", 1 if on else 0)

    # --- predict one window --------------------------------------------------

    def predict(self, context_mono: np.ndarray, timeout: float = 30.0
                ) -> Dict[str, np.ndarray]:
        """Send one hop of new context audio and block until predicted stems arrive.

        Protocol note (verified against multi_track.cpp, not the earlier draft
        of PROTOCOL.md): the server-side `context_audio` is stateful — each
        /context batch overwrites only the newest `r*T` samples at offset
        `T*(1-(w+2)*r)`. So callers must send exactly `step = int(T*r)` samples
        per predict(), NOT the full T-sample window. The server keeps the older
        samples from previous batches.

        `context_mono` must be length `step` (int(T_SAMPLES * r)), float32 mono.
        For the default r=0.25 that's 66150 samples.

        Returns a dict {stem_name: float32_array}.
        """
        step = int(T_SAMPLES * self.r)
        if context_mono.shape != (step,):
            raise ValueError(
                f"context must be shape ({step},) [= int(T*r) = int({T_SAMPLES}*{self.r})], "
                f"got {context_mono.shape}")
        context_mono = context_mono.astype(np.float32, copy=False)

        with self._lock:
            self._batch_id += 1
            batch_id = self._batch_id
            self._responses[batch_id] = {s: {} for s in self.stems_to_predict}
            self._response_totals[batch_id] = {}
            ev = threading.Event()
            self._batch_done_events[batch_id] = ev

        # Chunk and send on /context — only the `step`-sample hop, not full T
        total_chunks = (step + self.package_size - 1) // self.package_size
        for chunk_idx in range(total_chunks):
            start = chunk_idx * self.package_size
            end = min(start + self.package_size, step)
            floats = context_mono[start:end]
            self.osc.send_chunk(
                "/context",
                batch_id=batch_id,
                middle_int=start,            # ingress: SAMPLE OFFSET within the hop
                total_chunks=total_chunks,
                floats=floats,
            )
            # Brief pacing — matches server comment about remote stability
            time.sleep(0.00005)

        if not ev.wait(timeout):
            raise TimeoutError(
                f"batch {batch_id}: predicted stems did not arrive within {timeout}s")

        with self._lock:
            parts = self._responses.pop(batch_id)
            totals = self._response_totals.pop(batch_id)
            self._batch_done_events.pop(batch_id, None)

        result = {}
        for stem, chunks in parts.items():
            n_chunks = totals[stem]
            pieces = [chunks[i] for i in range(n_chunks)]
            result[stem] = np.concatenate(pieces) if pieces else np.zeros(0, dtype=np.float32)
        return result

    # --- packet round-trip probe --------------------------------------------

    def packet_probe(self, size: int = 1024, timeout: float = 2.0
                     ) -> Optional[float]:
        """Send /packet_test; return RTT in ms or None on timeout."""
        self._probe_event.clear()
        self._probe_result = None
        payload = np.random.rand(size).astype(np.float32)
        # /packet_test takes: ,i f... (size, then floats)
        args_dgram = osc_encode("/packet_test", int(size), *payload.tolist())
        t0 = time.perf_counter()
        self.osc.sock_send.sendto(args_dgram, self.osc.server_addr)
        if not self._probe_event.wait(timeout):
            return None
        t1 = time.perf_counter()
        return (t1 - t0) * 1000.0

    # --- handlers ------------------------------------------------------------

    def _on_ready(self, address, *args):
        print(f"[rx] {address} {args}")
        self._ready.set()

    def _on_stem_chunk(self, address, batch_id, chunk_idx, total_chunks, floats):
        stem = address.lstrip("/")
        with self._lock:
            buf = self._responses.get(batch_id)
            if buf is None or stem not in buf:
                # late chunk for a completed batch — drop
                return
            buf[stem][chunk_idx] = floats
            self._response_totals[batch_id][stem] = total_chunks
            # Is this batch done? (all target stems, all chunks)
            all_done = True
            for s in self.stems_to_predict:
                t = self._response_totals[batch_id].get(s)
                if t is None or len(buf[s]) < t:
                    all_done = False; break
            if all_done:
                ev = self._batch_done_events.get(batch_id)
                if ev: ev.set()

    def _on_batch_dropped(self, address, batch_id):
        print(f"[rx] /batch_dropped {batch_id} — server was still busy")
        with self._lock:
            ev = self._batch_done_events.get(batch_id)
            if ev: ev.set()  # unblock predict() with whatever we have

    def _on_packet_test_response(self, address, batch_id_or_size, mid, total_chunks, floats):
        # Server packs (size:i, floats...) — our fast path will decode it as
        # (batch_id, mid, total_chunks, floats). batch_id_or_size actually IS size.
        self._probe_result = (batch_id_or_size, len(floats))
        self._probe_event.set()


# -----------------------------------------------------------------------------
# Offline driver: read a WAV, run the sliding-window loop, write per-stem WAVs
# -----------------------------------------------------------------------------

def offline_session(
    input_wav: Path,
    out_dir: Path,
    *,
    server_host: str,
    server_port: int,
    recv_port: int,
    stems_to_predict: Tuple[str, ...],
    r: float,
    w: float,
    fade: float,
    package_size: int,
    max_windows: Optional[int],
    verbose: bool,
) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    # Load + resample to 44.1 k mono
    audio, sr = sf.read(str(input_wav), always_2d=True)
    if sr != SR:
        raise SystemExit(
            f"Input WAV is {sr} Hz; server requires {SR} Hz. "
            f"Resample first: ffmpeg -i {input_wav} -ar {SR} -ac 1 resampled.wav")
    # Sum to mono
    mono = audio.mean(axis=1).astype(np.float32)
    print(f"loaded {input_wav.name}: {len(mono)} samples ({len(mono)/SR:.2f} s)")

    step = int(T_SAMPLES * r)
    n_windows = (len(mono) - T_SAMPLES) // step + 1
    if max_windows: n_windows = min(n_windows, max_windows)
    if n_windows < 1:
        raise SystemExit(f"Audio too short — need at least {T_SAMPLES/SR:.1f}s, got {len(mono)/SR:.2f}s")
    print(f"sliding over {n_windows} windows (T={T_SAMPLES/SR}s, step={step/SR}s)")

    osc = OscClient(server_host=server_host, server_port=server_port,
                    recv_host="0.0.0.0", recv_port=recv_port)
    client = AccompanimentClient(osc,
                                 package_size=package_size, r=r, w=w, fade=fade,
                                 stems_to_predict=stems_to_predict)

    try:
        print(f"waiting for /ready from {server_host}:{server_port} …")
        if not client.wait_ready(timeout=10.0):
            print("WARN: no /ready received — server may already be running; continuing anyway")

        if verbose:
            client.verbose(True)

        client.configure_server()
        client.reset()
        # Arm a fresh /ready event before sending /load_model. The server emits a
        # second /ready on the client port once the checkpoint is fully loaded
        # (see server_CD.py::load_network). This is critical: while the load is
        # in flight, the server silently drops every incoming /context chunk via
        # `if _loading: continue`, so we MUST wait for the post-load /ready
        # before streaming any audio.
        client.arm_ready()
        client.load_model()
        print("waiting for /ready post-load (up to 60 s) …")
        if not client.wait_ready(timeout=60.0):
            print("WARN: no post-load /ready — server may not signal it; proceeding")
        else:
            print("server reports ready")

        # Probe round-trip before pushing real audio
        rtt = client.packet_probe(size=1024, timeout=3.0)
        if rtt is not None:
            print(f"round-trip probe: {rtt:.1f} ms for 1024 floats")

        # Output buffers, one per predicted stem
        out_len = (n_windows - 1) * step + int(T_SAMPLES * r) + int(fade * SR)
        outputs: Dict[str, np.ndarray] = {s: np.zeros(out_len, dtype=np.float32)
                                          for s in stems_to_predict}

        for i in range(n_windows):
            win_start = i * step
            # predict() expects exactly `step` samples — the new hop of mixture
            # audio since the last batch. Server keeps context state across
            # batches, so we only ever send the newest r*T samples.
            hop_mono = mono[win_start + T_SAMPLES - step : win_start + T_SAMPLES]
            if len(hop_mono) < step:
                hop_mono = np.pad(hop_mono, (0, step - len(hop_mono)))
            t0 = time.perf_counter()
            stems = client.predict(hop_mono, timeout=60.0)
            t1 = time.perf_counter()
            print(f"  window {i+1}/{n_windows}  " +
                  "  ".join(f"{s}={len(v)}" for s, v in stems.items()) +
                  f"  ({(t1-t0)*1000:.0f} ms)")

            # Place predicted stems at the correct offset in the output timeline.
            # Per PROTOCOL.md §6: length = r*T + fade*SR; starts at win_start + w*r*T.
            place_start = win_start + int(w * r * T_SAMPLES)
            for stem, arr in stems.items():
                a = place_start
                b = min(place_start + len(arr), len(outputs[stem]))
                if a < 0 or a >= len(outputs[stem]):
                    continue
                # Simple overlap-add — relies on the server-side fade-in at chunk[0]
                outputs[stem][a:b] += arr[:b-a]

        # Write WAVs
        for stem, arr in outputs.items():
            path = out_dir / f"{input_wav.stem}__{stem}.wav"
            # Peak-normalize only if clipping
            peak = float(np.max(np.abs(arr))) if arr.size else 0.0
            if peak > 1.0:
                arr = arr / peak
            sf.write(str(path), arr, SR, subtype="FLOAT")
            print(f"wrote {path}  ({len(arr)} samples, peak={peak:.3f})")

    finally:
        osc.close()


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(description="AI Accompaniment reference OSC client")
    p.add_argument("--server-host", default="127.0.0.1")
    p.add_argument("--server-port", type=int, default=7000)
    p.add_argument("--recv-port", type=int, default=8000)
    sub = p.add_subparsers(dest="cmd", required=True)

    po = sub.add_parser("offline", help="Process a WAV file window-by-window")
    po.add_argument("--input", type=Path, required=True)
    po.add_argument("--out-dir", type=Path, default=Path("out"))
    po.add_argument("--stems", nargs="+", default=["bass", "drums"],
                    choices=list(STEM_NAMES))
    po.add_argument("--r", type=float, default=DEFAULT_R)
    po.add_argument("--w", type=float, default=DEFAULT_W)
    po.add_argument("--fade", type=float, default=DEFAULT_FADE)
    po.add_argument("--package-size", type=int, default=DEFAULT_PACKAGE_SIZE)
    po.add_argument("--max-windows", type=int, default=None,
                    help="Stop after N windows (debug)")
    po.add_argument("--verbose", action="store_true")

    sub.add_parser("probe", help="Round-trip latency probe, no model inference")

    args = p.parse_args()

    if args.cmd == "offline":
        offline_session(
            input_wav=args.input,
            out_dir=args.out_dir,
            server_host=args.server_host,
            server_port=args.server_port,
            recv_port=args.recv_port,
            stems_to_predict=tuple(args.stems),
            r=args.r, w=args.w, fade=args.fade,
            package_size=args.package_size,
            max_windows=args.max_windows,
            verbose=args.verbose,
        )
    elif args.cmd == "probe":
        osc = OscClient(server_host=args.server_host,
                        server_port=args.server_port,
                        recv_host="0.0.0.0", recv_port=args.recv_port)
        client = AccompanimentClient(osc)
        try:
            if client.wait_ready(timeout=5.0):
                print("/ready received")
            else:
                print("no /ready — server might already be up; probing anyway")
            for i in range(5):
                rtt = client.packet_probe(size=1024, timeout=2.0)
                if rtt is None:
                    print(f"probe {i+1}: TIMEOUT")
                else:
                    print(f"probe {i+1}: {rtt:.1f} ms")
                time.sleep(0.2)
        finally:
            osc.close()


if __name__ == "__main__":
    main()
