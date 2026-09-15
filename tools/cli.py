"""Prepare, load, and execute fx on a Seeed reTerminal E1001 (N32R8)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from board_io import Console, read_credentials

ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / "build/reterminal-e1001-image/linux-reterminal-e1001-fx-16m.bin"
PRIVATE = ROOT / "private"


def serial_port(explicit=None):
    if explicit or os.environ.get("ESP32_SERIAL"):
        return explicit or os.environ["ESP32_SERIAL"]
    candidates = sorted(Path("/dev/serial/by-id").glob("*"))
    if not candidates:
        candidates = sorted(Path("/dev").glob("ttyUSB*")) + sorted(Path("/dev").glob("ttyACM*"))
    distinct = {p.resolve(): str(p) for p in candidates}
    if len(distinct) != 1:
        raise RuntimeError("Connect one board, or select it with --serial /dev/ttyUSB0")
    return next(iter(distinct.values()))


def login(serial, fresh=False):
    password = "changeme123"
    access = PRIVATE / "example-board-access.json"
    if not fresh and access.exists():
        record = json.loads(access.read_text())
        if Path(record["serial"]).resolve() != Path(serial).resolve():
            raise RuntimeError("Saved login belongs to a different board; select its serial port")
        password = record["root_password"]
    console = Console(serial)
    try:
        console.send(b"\n")
        _, match = console.until(rb"login:|(?:^|\n)# ", 60)
        if b"login:" in match[0]:
            console.send(b"root\n")
            console.until(rb"Password:", 10)
            console.send(password.encode() + b"\n")
            console.until(rb"(?:^|\n)# ", 15)
        code, _ = console.run("unset HISTFILE; stty -echo; umask 077", 10)
        if code:
            raise RuntimeError("Could not initialize board console")
    finally:
        console.close()


def esptool(serial, *args, capture=False):
    from build import mounted
    arguments = [mounted(a) if isinstance(a, Path) else str(a) for a in args]
    command = ["podman", "run", "--rm", "--userns=keep-id", "--group-add", "keep-groups",
               "--network", "none", "--device", f"{Path(serial).resolve()}:/dev/esp32",
               "--volume", f"{ROOT}:/work", "--entrypoint", "/usr/local/bin/esptool",
               "localhost/esp32-fx-linux-builder:bookworm",
               "--chip", "esp32s3", "--port", "/dev/esp32", "--baud", "460800",
               *(a.replace("-", "_") if a in {"flash-id", "read-flash", "write-flash", "verify-flash"}
                 else "--" + a[2:].replace("-", "_") if a in {"--flash-mode", "--flash-freq", "--flash-size"}
                 else a for a in arguments)]
    result = subprocess.run(command, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None)
    return result.stdout or ""


def validate_image(image=IMAGE):
    manifest = json.loads((image.parent / "manifest.json").read_text())
    data = image.read_bytes()
    if len(data) != 16*1024*1024 or hashlib.sha256(data).hexdigest() != manifest["files"][image.name]["sha256"]:
        raise RuntimeError("Image size/hash mismatch; rerun prep")


def install(serial, env_file):
    validate_image()
    PRIVATE.mkdir(mode=0o700, exist_ok=True)
    # Probe once before a backup or write. Device identity stays in private state.
    probe = esptool(serial, "flash-id", capture=True)
    if not re.search(r"Detected flash size:\s*32\s*MB", probe, re.I):
        raise RuntimeError("This image requires an ESP32-S3 with 32 MiB flash")
    identity = re.search(r"MAC:\s*([0-9a-f:]{17})", probe, re.I)
    if not identity:
        raise RuntimeError("esptool did not return a device identity")
    backup = PRIVATE / ("backup-" + identity[1].replace(":", "").lower() + ".bin")
    digest_file = backup.with_suffix(".sha256")
    if backup.exists():
        if not digest_file.exists() or backup.stat().st_size != 32*1024*1024:
            raise RuntimeError("Existing backup is incomplete; inspect private recovery files")
        if hashlib.sha256(backup.read_bytes()).hexdigest() != digest_file.read_text().strip():
            raise RuntimeError("Recovery backup checksum mismatch")
    else:
        print("Saving and verifying a private 32 MiB recovery backup…", flush=True)
        esptool(serial, "read-flash", "0", "0x2000000", backup, capture=True)
        if backup.stat().st_size != 32*1024*1024:
            raise RuntimeError("Incomplete device backup")
        esptool(serial, "verify-flash", "0", backup, capture=True)
        digest_file.write_text(hashlib.sha256(backup.read_bytes()).hexdigest() + "\n")
    print("Installing the lower 16 MiB; onboard /etc and /home are replaced.", flush=True)
    esptool(serial, "write-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", "0", IMAGE, capture=True)
    esptool(serial, "verify-flash", "0", IMAGE, capture=True)
    time.sleep(5)
    login(serial, fresh=True)
    # The image has a fresh default login; rotate it when networking is configured.
    if env_file.exists():
        import network
        network.configure(serial, env_file, fresh_image=True)
    else:
        old = PRIVATE / "example-board-access.json"
        if old.exists():
            old.rename(PRIVATE / ("access-before-install-" + str(time.time_ns()) + ".json"))
    print("Loaded and verified. Next: ./esp32-fx execute --demo")


def execute(args, serial):
    login(serial)
    if args.example:
        import network
        import example
        network.configure(serial, args.env_file)
        example.execute(serial, args.env_file, args.timeout)
        return
    if not args.demo and not args.module:
        import network
        network.configure(serial, args.env_file)
    console = Console(serial)
    key = ""
    try:
        code, _ = console.run("unset HISTFILE; stty -echo; umask 077")
        if code:
            raise RuntimeError("Could not secure console")
        if args.module:
            from deploy import NAME_PATTERN
            if not NAME_PATTERN.fullmatch(args.module):
                raise RuntimeError("Module name must use lowercase letters, digits, _ or -")
            base = "/tmp/microwasm/apps" if args.storage == "ram" else "/media/sd/microwasm/apps"
            directory = f"{base}/{args.module}"
            mount = "/usr/bin/fx-sd mount && " if args.storage == "sd" else ""
            command = (mount + f"digest=$(cat {directory}/current.sha256) && "
                       "test \"${#digest}\" -eq 64 && "
                       "case \"$digest\" in *[!0-9a-f]*) false;; *) true;; esac && "
                       f"module={directory}/$digest.wasm && "
                       "test \"$(sha256sum \"$module\" | cut -d' ' -f1)\" = \"$digest\" && "
                       "FX_WAMR_LINEAR_MEMORY_ADDRESS=0x3df00000 "
                       "FX_WAMR_NATIVE_STREAM_ADDRESS=0x3def0000 "
                       "AI_GATEWAY_API_KEY=offline-module FX_MODEL=fixture/wasm "
                       "FX_WAMR_FIXTURE_TEXT='Standalone Wasm execution' "
                       "FX_WAMR_MAX_PAGES=16 /usr/bin/fx-wamr \"$module\"")
        else:
            prompt = args.prompt or "Show the commissioning status."
            if not args.demo:
                values = read_credentials(args.env_file)
                key = values.get("AI_GATEWAY_API_KEY") or values.get("VERCEL_API")
                model = values.get("FX_MODEL") or values.get("VERCEL_MODEL")
                if not key or not model:
                    raise RuntimeError(".env needs AI_GATEWAY_API_KEY and FX_MODEL")
                code, _ = console.run("export AI_GATEWAY_API_KEY=" + shlex.quote(key)
                                      + " FX_MODEL=" + shlex.quote(model)
                                      + " FX_WAMR_MAX_PAGES=16 FX_WAMR_LOCAL_MODEL_CATALOG=1"
                                      + " FX_WAMR_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt")
                if code:
                    raise RuntimeError("Provider configuration failed")
            command = ("/usr/bin/fx-demo " if args.demo else "/usr/bin/fx ") + shlex.quote(prompt)
        console.prepare_launch()
        code, output = console.run(command, timeout=args.timeout)
        if key:
            output = output.replace(key, "[redacted]")
        output = re.sub(r"(?:^|\n)__FX_CMD_[0-9a-f]+=\d+\n", "\n", output)
        output = re.sub(r"(?m)^#\s*$", "", output)
        print(output.strip())
        if code:
            raise RuntimeError(f"Board execution exited with status {code}")
    finally:
        try:
            console.run("unset AI_GATEWAY_API_KEY FX_MODEL FX_WAMR_MAX_PAGES "
                        "FX_WAMR_LOCAL_MODEL_CATALOG FX_WAMR_CA_BUNDLE; stty echo", timeout=10)
        finally:
            console.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prep", help="fetch tools, build, and test the board image")
    prep.add_argument("--host-only", action="store_true", help="build and test on workstation only")
    load = commands.add_parser("load", help="install the host image, or upload a Wasm file")
    load.add_argument("wasm", nargs="?", type=Path, help="optional module: writes RAM/SD, never NOR flash")
    load.add_argument("--name", default="app")
    load.add_argument("--storage", choices=("ram", "sd"), default="sd")
    execute_parser = commands.add_parser("execute", help="run fx and refresh e-paper")
    execute_parser.add_argument("prompt", nargs="?")
    mode = execute_parser.add_mutually_exclusive_group()
    mode.add_argument("--demo", action="store_true", help="offline provider fixture")
    mode.add_argument("--example", action="store_true", help="run and verify the coding example")
    mode.add_argument("--module", help="run a named RAM/SD module")
    execute_parser.add_argument("--storage", choices=("ram", "sd"), default="sd")
    execute_parser.add_argument("--timeout", type=int, default=420)
    for command in (load, execute_parser):
        command.add_argument("--serial", help="auto-detected when one USB board is connected")
        command.add_argument("--env-file", type=Path, default=ROOT / ".env")
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        if args.command == "prep":
            import build
            build.prep(args.host_only)
        elif args.command == "load":
            serial = serial_port(args.serial)
            if args.wasm:
                import deploy
                deploy.read_module(args.wasm)  # Validate before board access.
                login(serial)
                deploy.load(serial, args.wasm, args.storage, args.name)
            else:
                install(serial, args.env_file)
        else:
            execute(args, serial_port(args.serial))
    except (RuntimeError, ValueError, OSError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            # Commands can contain private device paths. Keep them out of error text.
            print(f"Command failed (exit {error.returncode}); operation stopped.", file=sys.stderr)
            if args.command == "prep":
                print("Build details: build/prep.log", file=sys.stderr)
        else:
            print(str(error), file=sys.stderr)
        return 1
    return 0
