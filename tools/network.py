#!/usr/bin/env python3
"""Configure test Wi-Fi privately and set the board clock; never print credentials."""
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import time
from board_io import Console, read_credentials

ROOT = Path(__file__).resolve().parent.parent


def connect(console, private, ssid, psk):
    """Reuse a matching live connection; reconnect only when necessary."""
    lines = ["network={", "ssid=" + ssid.hex(), "psk=" + psk, "key_mgmt=WPA-PSK", "}"]
    digest = hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()
    code, text = console.run(
        "sha256sum /etc/wpa_supplicant.conf 2>/dev/null; "
        "iw dev espsta0 link; ip -4 addr show dev espsta0; ip route", timeout=15)
    if (code == 0 and digest + " " in text and "Connected to " in text
            and "inet " in text and "default " in text):
        return
    configuration = "/etc/wpa_supplicant.fx-example.conf"
    code, _ = console.run(f": > {configuration}; chmod 600 {configuration}")
    if code:
        raise RuntimeError("cannot create private Wi-Fi configuration")
    for line in lines:
        code, _ = console.run(f"printf '%s\\n' {shlex.quote(line)} >> {configuration}")
        if code:
            raise RuntimeError("cannot write Wi-Fi configuration")
    # Preserve any existing configuration instead of overwriting user data.
    code, _ = console.run(
        "if [ -e /etc/wpa_supplicant.conf ] && [ ! -e /etc/wpa_supplicant.pre-fx-example ]; then "
        "cp /etc/wpa_supplicant.conf /etc/wpa_supplicant.pre-fx-example; fi; "
        f"cp {configuration} /etc/wpa_supplicant.conf; chmod 600 /etc/wpa_supplicant.conf")
    if code:
        raise RuntimeError("cannot activate private Wi-Fi configuration")
    # The stock interfaces file runs DHCP before WPA in a post-up hook.
    # On this small host that wastes both time and transient memory. Bring
    # up L2, associate first, then make one bounded IPv4 DHCP request.
    code, text = console.run(
        "killall -q wpa_supplicant 2>/dev/null || true; "
        "ip link set espsta0 up && "
        "wpa_supplicant -i espsta0 -c /etc/wpa_supplicant.conf -B",
        timeout=30,
    )
    (private / "example-wifi-connect.txt").write_text(text)
    if code:
        raise RuntimeError("WPA startup failed; details retained privately")
    associated = False
    for _ in range(30):
        code, text = console.run("iw dev espsta0 link", timeout=10)
        (private / "example-wifi-link.txt").write_text(text)
        if code == 0 and "Connected to " in text:
            associated = True
            break
        time.sleep(1)
    if not associated:
        raise RuntimeError("WPA association failed; details retained privately")
    code, text = console.run("udhcpc -i espsta0 -n -q -t 3", timeout=45)
    (private / "example-dhcp.txt").write_text(text)
    if code:
        raise RuntimeError("DHCP failed; details retained privately")


def configure(serial, env_file, fresh_image=False):
    os.umask(0o077)
    credentials = read_credentials(env_file)
    ssid = credentials.get("WIFI_SSID", "").encode()
    password = credentials.get("WIFI_PW", "")
    if not 1 <= len(ssid) <= 32:
        raise SystemExit("WIFI_SSID must contain 1..32 UTF-8 bytes")
    if re.fullmatch(r"[a-fA-F0-9]{64}", password):
        psk = password.lower()
    elif 8 <= len(password.encode()) <= 63:
        psk = hashlib.pbkdf2_hmac("sha1", password.encode(), ssid, 4096, 32).hex()
    else:
        raise SystemExit("WIFI_PW must be a WPA2 passphrase or 64 hexadecimal PSK digits")
    private = ROOT / "private"
    private.mkdir(exist_ok=True, mode=0o700)
    console = Console(serial)
    try:
        code, _ = console.run("unset HISTFILE; stty -echo; umask 077")
        if code:
            raise RuntimeError("could not secure console")
        # Save before applying so an interrupted password change remains recoverable.
        # A host reflash restores the published default password while the private
        # record still names the prior image's password.  --fresh-image makes that
        # commissioning boundary explicit and retains the superseded record.
        access = private / "example-board-access.json"
        if not access.exists() or fresh_image:
            new_password = secrets.token_urlsafe(24)
            pending = private / "example-board-access.pending.json"
            if pending.exists():
                raise RuntimeError("pending access record exists; inspect it before retrying")
            pending.write_text(
                json.dumps({"serial": serial, "root_password": new_password}, indent=2) + "\n"
            )
            pending.chmod(0o600)
            marker = "__FX_PASSWORD_" + secrets.token_hex(6)
            console.send(f"passwd; printf '\\n{marker}=%s\\n' \"$?\"\n".encode())
            console.until(rb"[Nn]ew password:", 10)
            console.send(new_password.encode() + b"\n")
            console.until(rb"[Rr]etype password:", 10)
            console.send(new_password.encode() + b"\n")
            _, match = console.until(marker.encode() + rb"=(\d+)\n", 15)
            if int(match[1]):
                raise RuntimeError("root password change failed; inspect private pending access record")
            if access.exists():
                old = access.read_bytes()
                archive = private / (
                    "example-board-access.pre-reflash-"
                    + hashlib.sha256(old).hexdigest()[:12]
                    + ".json"
                )
                if archive.exists() and archive.read_bytes() != old:
                    raise RuntimeError("private access archive collision")
                if archive.exists():
                    access.unlink()
                else:
                    access.replace(archive)
            pending.replace(access)
            print("Changed development root password; recovery credential saved privately.", flush=True)
        connect(console, private, ssid, psk)
        # The upstream image already carries a compact public CA bundle at this
        # path, but curl's conventional pathname was absent in early images.
        # Provision only the symlink; never disable peer or hostname checks.
        code, _ = console.run(
            "if [ ! -s /etc/ssl/certs/ca-certificates.crt ] && "
            "[ -s /usr/share/ca-certificates/ca-bundle.crt ]; then "
            "mkdir -p /etc/ssl/certs && "
            "ln -sf /usr/share/ca-certificates/ca-bundle.crt "
            "/etc/ssl/certs/ca-certificates.crt; fi"
        )
        if code:
            raise RuntimeError("could not provision the public CA-bundle path")
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        code, _ = console.run("date -u -s " + shlex.quote(timestamp))
        if code:
            raise RuntimeError("could not set clock for TLS")
        # Association/DHCP completion can trail udhcpc's return on this port.
        # Gate on actual interface state instead of parsing client prose.
        network_ready = False
        for _ in range(15):
            code, text = console.run(
                "ip -4 addr show dev espsta0; ip route", timeout=10
            )
            (private / "example-network-state.txt").write_text(text)
            if code == 0 and "inet " in text and "default " in text:
                network_ready = True
                break
            time.sleep(2)
        code, text = console.run(
            "ip -4 addr show dev espsta0; ip route; "
            "test -s /etc/ssl/certs/ca-certificates.crt"
        )
        (private / "example-network-state.txt").write_text(text)
        if code or not network_ready:
            raise RuntimeError("network/CA verification failed; details retained privately")
        print("Wi-Fi has an IPv4 address; clock and CA bundle checked. Network details kept private.")
    finally:
        try:
            console.run("stty echo", timeout=5)
        except Exception:
            pass
        finally:
            console.close()
