#!/usr/bin/env python3
"""Upload one immutable Wasm artifact to ESP32 Linux without reflashing."""

from __future__ import annotations

import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import secrets
import shlex
import socket
import threading

from board_io import Console


MAX_MODULE_BYTES = 16 * 1024 * 1024
NAME_PATTERN = re.compile(r"[a-z0-9][a-z0-9_-]{0,47}\Z")


def read_module(path: Path) -> bytes:
    """Read a bounded, raw WebAssembly module before opening the board."""
    source = path.resolve(strict=True)
    if not source.is_file() or source.stat().st_size > MAX_MODULE_BYTES:
        raise ValueError("module must be a regular file no larger than 16 MiB")
    data = source.read_bytes()
    if len(data) < 8 or data[:4] != b"\0asm":
        raise ValueError("artifact is not a raw WebAssembly module")
    return data


def deployment_path(storage: str, name: str, digest: str) -> str:
    if storage not in ("ram", "sd"):
        raise ValueError("storage must be ram or sd")
    if not NAME_PATTERN.fullmatch(name):
        raise ValueError("name must match [a-z0-9][a-z0-9_-]{0,47}")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid SHA-256 digest")
    root = "/tmp/microwasm/apps" if storage == "ram" else "/media/sd/microwasm/apps"
    return f"{root}/{name}/{digest}.wasm"


def board_ipv4(console: Console) -> str:
    code, output = console.run("ip -4 addr show dev espsta0")
    match = re.search(r"inet ([0-9.]+)/", output)
    if code or not match:
        raise RuntimeError("connect board Wi-Fi before deploying")
    return match.group(1)


def host_ipv4_for(peer: str) -> str:
    route = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        route.connect((peer, 9))
        return route.getsockname()[0]
    finally:
        route.close()


def load(serial, module, storage="sd", name="app"):
    data = read_module(module)
    digest = hashlib.sha256(data).hexdigest()
    destination = deployment_path(storage, name, digest)
    directory = destination.rsplit("/", 1)[0]
    incoming = destination + ".incoming-" + secrets.token_hex(5)
    current_hash = directory + "/current.sha256"
    current_path = directory + "/current.path"

    console = Console(serial)
    server = None
    try:
        if storage == "sd":
            code, _ = console.run("/usr/bin/fx-sd mount", timeout=30)
            if code:
                raise RuntimeError("board could not mount the microSD card")
        peer = board_ipv4(console)
        host = host_ipv4_for(peer)
        token = secrets.token_hex(16)
        route_path = f"/{token}/{name}.wasm"
        served = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                if self.path != route_path:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/wasm")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                served.set()

            def log_message(self, *_: object) -> None:
                pass

        server = ThreadingHTTPServer((host, 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f"http://{host}:{server.server_port}{route_path}"
        command = (
            f"umask 077; mkdir -p {shlex.quote(directory)} && "
            f"curl -fsS --max-time 240 -o {shlex.quote(incoming)} {shlex.quote(url)} && "
            f"test \"$(wc -c < {shlex.quote(incoming)})\" = {len(data)} && "
            f"test \"$(sha256sum {shlex.quote(incoming)} | cut -d' ' -f1)\" = {digest} && "
            f"mv {shlex.quote(incoming)} {shlex.quote(destination)} && "
            f"printf '%s\\n' {shlex.quote(digest)} > {shlex.quote(current_hash)} && "
            f"printf '%s\\n' {shlex.quote(destination)} > {shlex.quote(current_path)}"
        )
        console.prepare_launch()
        code, _ = console.run(command, timeout=270)
        if code or not served.is_set():
            raise RuntimeError("Wasm transfer or on-device verification failed")
        print(f"Deployed {len(data)} bytes without reflashing")
        print(f"SHA-256: {digest}")
        print(f"Board path: {destination}")
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
        try:
            console.run("stty echo", timeout=5)
        finally:
            console.close()
