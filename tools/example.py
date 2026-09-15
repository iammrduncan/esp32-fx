#!/usr/bin/env python3
"""Run and independently verify the live fx coding example on the ESP32-S3."""

import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import time

from board_io import Console, read_credentials
from tool_trace import audit_tool_trace, memory_summary


ROOT = Path(__file__).resolve().parent.parent
SD_ROOT = "/media/sd/fx-coding"
ARTIFACTS = (
    "preflight.txt",
    "preflight.rc",
    "before.txt",
    "after.txt",
    "stdout.txt",
    "stderr.txt",
    "memory-before.txt",
    "memory-after.txt",
    "dmesg-tail.txt",
    "storage.txt",
    "identity.txt",
    "battery.before.sh",
    "battery.after.sh",
    "SPEC.after.md",
    "test.after.sh",
    "fx.rc",
    "before.rc",
    "after.rc",
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def integer_file(data):
    text = data.decode("ascii", "strict").strip()
    if not re.fullmatch(r"[0-9]{1,3}", text):
        raise ValueError("invalid target exit-code artifact")
    return int(text)


def execute(serial, env_file, timeout=420):
    if timeout < 60 or timeout > 900:
        raise ValueError("--timeout must be 60..900 seconds")

    os.umask(0o077)
    credentials = read_credentials(env_file)
    key = credentials.get("AI_GATEWAY_API_KEY") or credentials.get("VERCEL_API")
    model = credentials.get("FX_MODEL") or credentials.get("VERCEL_MODEL")
    if not key or not model:
        raise SystemExit(
            "env file needs AI_GATEWAY_API_KEY/VERCEL_API and FX_MODEL/VERCEL_MODEL"
        )
    if not re.fullmatch(r"[A-Za-z0-9._/:-]{1,256}", model):
        raise SystemExit("model identifier contains unsupported characters")

    run_id = time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3)
    board = f"{SD_ROOT}/{run_id}"
    jail = board + "/jail"
    output = ROOT / "private" / ("example-board-" + run_id)
    output.mkdir(parents=True, mode=0o700)
    console = Console(serial)
    started = time.monotonic()
    downloaded = {}
    missing_artifacts = []
    leaked = False
    mounts_prepared = False
    fx_code = None
    failure = None

    def run(command, timeout=60):
        code, _ = console.run(command, timeout=timeout)
        if code:
            raise RuntimeError("target command failed; private board artifacts retained")

    try:
        run("unset HISTFILE; stty -echo; umask 077")
        run(
            "/usr/bin/fx-sd mount >/dev/null && "
            "awk '$2 == \"/media/sd\" && $1 ~ /^\\/dev\\/mmcblk0/ "
            "{ found=1 } END { exit !found }' /proc/mounts && "
            f"test ! -e {shlex.quote(board)} && "
            f"mkdir -p {shlex.quote(board + '/seed')}"
        )

        # The kernel excludes the fixed WAMR arena from its buddy allocator,
        # so preflight can be sequential. Keeping every helper in the
        # foreground avoids retaining a NOMMU software-fork shadow during fx.
        mounts_prepared = True
        code, _ = console.run(
            f"/usr/bin/fx-example-preflight {shlex.quote(board)} "
            f"{shlex.quote(jail)} > {shlex.quote(board + '/preflight.txt')} "
            "2>&1",
            timeout=45,
        )
        if code:
            raise RuntimeError("target coding-session preflight failed")

        secret_command = (
            "export AI_GATEWAY_API_KEY=" + shlex.quote(key)
            + "; export FX_MODEL=" + shlex.quote(model)
            + "; export FX_WAMR_LOCAL_MODEL_CATALOG=1"
            + "; export FX_WAMR_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt"
            + "; export FX_WAMR_DIAGNOSTICS=1; export FX_EINK_DIAGNOSTICS=1"
            + "; export FX_WAMR_MAX_PAGES=16"
            + "; export FX_EXAMPLE_JAIL=" + shlex.quote(jail)
            + "; export FX_EXAMPLE_TEST_HELPER=/usr/bin/fx-example-test"
        )
        run(secret_command)
        console.prepare_launch()
        code, _ = console.run(
            "prompt=$(/bin/cat /usr/share/fx/example/prompt.md); "
            f"/usr/bin/fx \"$prompt\" "
            f"> {shlex.quote(board + '/stdout.txt')} "
            f"2> {shlex.quote(board + '/stderr.txt')}; "
            "result=$?; unset prompt; "
            f"printf '%s\\n' \"$result\" > {shlex.quote(board + '/fx.rc')}; "
            "test \"$result\" -eq 0",
            timeout=timeout,
        )
        fx_code = code
        if code:
            raise RuntimeError("target fx coding session or preflight failed")
    except Exception as exc:
        failure = str(exc)
        try:
            console.send(b"\x03")
        except (OSError, TimeoutError):
            pass
    finally:
        try:
            console.run(
                "unset AI_GATEWAY_API_KEY VERCEL_API FX_MODEL VERCEL_MODEL "
                "FX_WAMR_LOCAL_MODEL_CATALOG FX_WAMR_CA_BUNDLE "
                "FX_WAMR_DIAGNOSTICS FX_EINK_DIAGNOSTICS FX_WAMR_MAX_PAGES "
                "FX_EXAMPLE_JAIL FX_EXAMPLE_TEST_HELPER; "
                "stty echo",
                timeout=10,
            )
        except Exception:
            pass

    try:
        # These checks run even after an fx failure so a partial edit cannot be
        # mislabeled as a completed coding session.
        console.run(
            f"cat /proc/meminfo > {shlex.quote(board + '/memory-after.txt')}; "
            f"cat /proc/buddyinfo >> {shlex.quote(board + '/memory-after.txt')}; "
            f"dmesg > {shlex.quote(board + '/dmesg-tail.txt')}; "
            f"df -k /media/sd > {shlex.quote(board + '/storage.txt')}; "
            f"uname -a > {shlex.quote(board + '/identity.txt')}; "
            f"output=$(/usr/bin/fx-example-test {shlex.quote(jail)} 2>&1); "
            "result=$?; "
            f"printf '%s\\n' \"$output\" > {shlex.quote(board + '/after.txt')}; "
            "unset output; "
            f"printf '%s\\n' \"$result\" > {shlex.quote(board + '/after.rc')}; "
            f"cp {shlex.quote(jail + '/project/battery.sh')} "
            f"{shlex.quote(board + '/battery.after.sh')}; "
            f"cp {shlex.quote(jail + '/project/SPEC.md')} "
            f"{shlex.quote(board + '/SPEC.after.md')}; "
            f"cp {shlex.quote(jail + '/project/test.sh')} "
            f"{shlex.quote(board + '/test.after.sh')}; true",
            timeout=45,
        )
        for name in ARTIFACTS:
            path = board + "/" + name
            try:
                data = console.download(path)
            except Exception:
                data = b""
                missing_artifacts.append(name)
            secret_values = (
                value for key_name, value in credentials.items()
                if key_name not in ("FX_MODEL", "VERCEL_MODEL")
            )
            for value in secret_values:
                encoded = value.encode()
                if len(encoded) >= 4 and encoded in data:
                    data = data.replace(encoded, b"[REDACTED]")
                    leaked = True
            downloaded[name] = data
            (output / name).write_bytes(data)
    finally:
        if mounts_prepared:
            console.run(
                f"umount {shlex.quote(jail + '/usr/lib')} 2>/dev/null || true; "
                f"umount {shlex.quote(jail + '/lib')} 2>/dev/null || true; "
                f"umount {shlex.quote(jail + '/bin/sh')} 2>/dev/null || true",
                timeout=20,
            )
        console.close()

    def exit_code_artifact(name):
        try:
            return integer_file(downloaded[name])
        except (KeyError, UnicodeError, ValueError):
            return 255

    preflight_rc = exit_code_artifact("preflight.rc")
    before_rc = exit_code_artifact("before.rc")
    after_rc = exit_code_artifact("after.rc")
    recorded_fx_rc = exit_code_artifact("fx.rc")
    diagnostics = downloaded["stderr.txt"].decode("utf-8", "replace")
    stdout = downloaded["stdout.txt"].decode("utf-8", "replace")
    after = downloaded["after.txt"]
    tools = audit_tool_trace(diagnostics)
    memory = memory_summary(diagnostics)
    source = {name: (ROOT / "example/project" / name).read_bytes()
              for name in ("SPEC.md", "battery.sh", "test.sh")}
    inputs_unchanged = (
        downloaded["SPEC.after.md"] == source["SPEC.md"]
        and downloaded["test.after.sh"] == source["test.sh"]
    )
    battery_changed = (
        downloaded["battery.before.sh"] == source["battery.sh"]
        and downloaded["battery.after.sh"] != source["battery.sh"]
    )
    display_refreshed = "display refresh complete" in (stdout + diagnostics)
    fatal = re.search(
        r"Out of memory:|page allocation failure|Kernel panic|"
        r"failed to (?:load|instantiate|execute) module|display timeout",
        diagnostics + downloaded["dmesg-tail.txt"].decode("utf-8", "replace"),
        re.IGNORECASE,
    ) is not None
    passed = (
        failure is None
        and fx_code == 0
        and preflight_rc == 0
        and recorded_fx_rc == 0
        and before_rc == 1
        and after_rc == 0
        and b"EXAMPLE_PASS checks=14" in after
        and inputs_unchanged
        and battery_changed
        and tools["all_project_files_read"]
        and tools["agent_tested_before_and_after_edit"]
        and display_refreshed
        and memory["maximum_observed_guest_pages"] is not None
        and memory["maximum_observed_guest_pages"] <= 16
        and not fatal
        and not leaked
    )
    manifest = json.loads(
        (ROOT / "build/reterminal-e1001-image/manifest.json").read_text()
    )
    summary = {
        "scope": "physical ESP32-S3 Linux real-provider coding session",
        "model": model,
        "board_storage": "microSD",
        "board_run_directory": board,
        "preflight_exit_code": preflight_rc,
        "fx_exit_code": recorded_fx_rc,
        "baseline_exit_code": before_rc,
        "independent_test_exit_code": after_rc,
        "independent_test_passed": b"EXAMPLE_PASS checks=14" in after,
        "tests_and_spec_unchanged": inputs_unchanged,
        "battery_changed": battery_changed,
        "display_refresh_observed": display_refreshed,
        "credential_material_detected": leaked,
        "fatal_runtime_pattern_detected": fatal,
        "missing_artifacts": missing_artifacts,
        "elapsed_seconds": round(time.monotonic() - started, 2),
        "image_sha256": manifest["files"][
            "linux-reterminal-e1001-fx-16m.bin"
        ]["sha256"],
        "guest_sha256": manifest["fx_wasm"]["sha256"],
        "artifact_sha256": {name: digest(data)
                            for name, data in downloaded.items()},
        "passed": passed,
        **tools,
        **memory,
    }
    if failure:
        summary["failure"] = failure
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print("Private evidence:", output)
    print("Board evidence retained on microSD:", board)
    raise SystemExit(0 if passed else 1)
