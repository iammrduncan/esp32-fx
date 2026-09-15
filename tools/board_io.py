#!/usr/bin/env python3
"""Shared no-reset serial console and bounded download transport."""
import base64
import fcntl
import hashlib
import os
from pathlib import Path
import re
import secrets
import select
import shlex
import termios
import time

DOWNLOAD_ROOTS = (
    "/tmp/fx-run/",
    "/tmp/fx-coding/",
    "/home/root/fx-coding/",
    "/media/sd/fx-coding/",
)


def download_root(source):
    """Return the admitted evidence root for one canonical absolute path."""
    if "//" in source or "/./" in source or "/../" in source:
        raise ValueError("download source is not canonical")
    for root in DOWNLOAD_ROOTS:
        if source.startswith(root) and len(source) > len(root):
            return root[:-1]
    raise ValueError("download source is outside an admitted evidence root")


class Console:
    def __init__(self, serial):
        self.fd = os.open(serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = attrs[1] = attrs[3] = 0
            attrs[2] &= ~(termios.HUPCL | termios.PARENB | termios.CSTOPB | termios.CSIZE)
            attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
            attrs[4] = attrs[5] = termios.B115200
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except BaseException:
            os.close(self.fd)
            raise

    def close(self):
        os.close(self.fd)

    def send(self, data):
        deadline = time.monotonic() + max(30, len(data) / 500)
        while data:
            if time.monotonic() > deadline:
                raise TimeoutError("serial write timeout")
            if select.select([], [self.fd], [], 0.1)[1]:
                # Keep below UART line speed while flash/filesystem writes run.
                count = os.write(self.fd, data[:32])
                data = data[count:]
                time.sleep(0.03)

    def until(self, pattern, timeout):
        received = bytearray()
        self.last_output = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if select.select([self.fd], [], [], 0.1)[0]:
                received.extend(os.read(self.fd, 4096))
                if len(received) > 2 * 1024 * 1024:
                    raise RuntimeError("serial output exceeds capture limit")
                cleaned = bytes(received).replace(b"\r", b"")
                self.last_output = cleaned
                match = re.search(pattern, cleaned)
                if match:
                    return cleaned.decode("utf8", "replace"), match
        # Deliberately do not include raw received data in exceptions.
        raise TimeoutError("serial completion timeout; inspect board state before retrying")

    def run(self, command, timeout=60):
        marker = "__FX_CMD_" + secrets.token_hex(8)
        self.send((command + "\nprintf '\\n" + marker + "=%s\\n' \"$?\"\n").encode())
        text, match = self.until(rb"(?:^|\n)" + marker.encode() + rb"=(\d+)\n", timeout)
        return int(match[1]), text

    def prepare_launch(self):
        """Reclaim caches before the NOMMU ELF loader needs contiguous RAM."""
        code, _ = self.run("sync && printf '3\\n' > /proc/sys/vm/drop_caches", timeout=30)
        if code:
            raise RuntimeError("could not prepare board memory for process launch")

    def download(self, source, maximum=1024 * 1024):
        """Download one bounded regular artifact with end-to-end SHA-256."""
        download_root(source)
        if maximum < 1 or maximum > 1024 * 1024:
            raise ValueError("download maximum must be 1..1048576 bytes")
        quoted = shlex.quote(source)
        code, _ = self.run(
            f"test ! -L {quoted} && test -f {quoted} && "
            f"test \"$(wc -c < {quoted})\" -le {maximum}",
            timeout=20,
        )
        if code:
            raise RuntimeError("download source is absent, nonregular, or too large")
        begin = "__FX_DOWNLOAD_" + secrets.token_hex(8)
        end = "__FX_DOWNLOAD_END_" + secrets.token_hex(8)
        command = (
            f"size=$(wc -c < {quoted}) && "
            f"digest=$(sha256sum {quoted} | cut -d' ' -f1) && "
            f"printf '\\n{begin} %s %s\\n' \"$size\" \"$digest\" && "
            f"base64 {quoted} && printf '{end}\\n'"
        )
        self.send((command + "\n").encode())
        wire, _ = self.until(
            rb"(?:^|\n)" + end.encode() + rb"\n",
            max(30, maximum // 500),
        )
        cleaned = wire.encode().replace(b"\r", b"")
        match = re.search(
            rb"(?:^|\n)" + begin.encode()
            + rb" ([0-9]+) ([0-9a-f]{64})\n(.*?)"
            + end.encode() + rb"\n",
            cleaned,
            re.DOTALL,
        )
        if not match:
            raise RuntimeError("download framing failed")
        encoded = b"".join(match[3].splitlines())
        try:
            data = base64.b64decode(encoded, validate=True)
        except ValueError as exc:
            raise RuntimeError("download base64 validation failed") from exc
        if len(data) != int(match[1]):
            raise RuntimeError("download size validation failed")
        if hashlib.sha256(data).hexdigest().encode() != match[2]:
            raise RuntimeError("download digest validation failed")
        return data


def read_credentials(path):
    """Read literal dotenv values; no shell expansion, command execution, or logging."""
    values = {}
    for raw in Path(path).read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        name, sep, value = line.partition("=")
        name, value = name.strip(), value.strip()
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or name in values:
            raise ValueError("invalid or duplicate dotenv field")
        if value.startswith(("'", '"')):
            parsed = shlex.split(value, comments=True)
            if len(parsed) != 1:
                raise ValueError("invalid quoted dotenv value")
            value = parsed[0]
        if "\x00" in value or "\n" in value or "\r" in value:
            raise ValueError("multiline credential values are unsupported")
        values[name] = value
    return values
