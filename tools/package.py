#!/usr/bin/env python3
"""Build the checked, non-flashing reTerminal E1001 Linux + fx image."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


REPO = Path(__file__).resolve().parent.parent
LINUX_WORK = Path(os.environ.get(
    "ESP32_LINUX_BUILD_ROOT", REPO / "build/linux-reproduce/base"
)).resolve()
SNAPSHOT = LINUX_WORK / "Linux-on-esp32-S3"
DRIVER_BUILD = LINUX_WORK / "refs/esp32-linux-build/build"
BUILDROOT_OUT = DRIVER_BUILD / "build-buildroot-esp32s3_devkit_c1_16m"
HOST = BUILDROOT_OUT / "host"
EXPERIMENT = SNAPSHOT / "experiments/mmu-poc"
PROGRAMS = EXPERIMENT / "programs"
EXP_OUT = EXPERIMENT / "out"
OUTPUT = REPO / "build/reterminal-e1001-image"
TARGET_TOOLS = REPO / "build/target-tools"
FX_WASM = Path(os.environ.get(
    "FX_WASM_IMAGE", REPO / "build/fx-compact/bin/fx-core.wasm"
)).resolve()
DASH = EXP_OUT / "real-bins/dash"
PARTITION_CSV = REPO / "board/partition_table.reterminal-e1001.16m"
FLASH_BYTES = 16 * 1024 * 1024
FX_WASM_FLASH_OFFSET = 0xD80000
FX_WASM_FLASH_ADDRESS = 0x3C000000 + FX_WASM_FLASH_OFFSET
FX_WASM_PARTITION_BYTES = 0x200000
FX_WASM_HEADER_BYTES = 64


def run(*args: object) -> None:
    subprocess.run([str(arg) for arg in args], check=True)


def digest(path: Path) -> str:
    block_hash = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            block_hash.update(block)
    return block_hash.hexdigest()


def copy(source: Path, target: Path, mode: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(mode)


def parse_partitions(path: Path) -> dict[str, dict[str, int]]:
    partitions: dict[str, dict[str, int]] = {}
    occupied: list[tuple[int, int, str]] = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            raise SystemExit(f"invalid partition line: {raw}")
        name, offset, size = fields[0], int(fields[3], 0), int(fields[4], 0)
        if offset < 0x9000 or size <= 0 or offset + size > FLASH_BYTES:
            raise SystemExit(f"partition outside lower 16 MiB: {name}")
        occupied.append((offset, offset + size, name))
        partitions[name] = {"offset": offset, "size": size}
    required = {"factory", "etc", "linux", "rootfs", "fxwasm", "home"}
    if not required <= partitions.keys():
        raise SystemExit(f"missing partitions: {sorted(required - partitions.keys())}")
    for left, right in zip(sorted(occupied), sorted(occupied)[1:]):
        if left[1] > right[0]:
            raise SystemExit(f"overlapping partitions: {left[2]} and {right[2]}")
    if partitions["rootfs"] != {"offset": 0x540000, "size": 0x840000}:
        raise SystemExit("rootfs must preserve XIP base 0x540000 and end at 0xd80000")
    if partitions["fxwasm"] != {"offset": FX_WASM_FLASH_OFFSET,
                                "size": FX_WASM_PARTITION_BYTES}:
        raise SystemExit("fxwasm must occupy the fixed 2-MiB data-XIP window")
    if partitions["home"] != {"offset": 0xF80000, "size": 0x80000}:
        raise SystemExit("home must occupy eight 64-KiB JFFS2 erase blocks")
    return partitions


def configure_rootfs(tree: Path) -> None:
    (tree / "media/sd").mkdir(parents=True, exist_ok=True)
    (tree / "media/sd").chmod(0o755)
    passwd = tree / "etc/passwd"
    entries = passwd.read_text().splitlines()
    roots = [index for index, entry in enumerate(entries) if entry.startswith("root:")]
    if len(roots) != 1:
        raise SystemExit("root passwd entry is missing or duplicated")
    fields = entries[roots[0]].split(":")
    fields[-2:] = ["/home/root", "/usr/bin/user-shell"]
    entries[roots[0]] = ":".join(fields)
    passwd.write_text("\n".join(entries) + "\n")

    shells = tree / "etc/shells"
    allowed = shells.read_text().splitlines()
    if "/usr/bin/user-shell" not in allowed:
        shells.write_text("\n".join(allowed + ["/usr/bin/user-shell"]) + "\n")
    if (tree / "etc/wpa_supplicant.conf").exists():
        raise SystemExit("refusing to package Wi-Fi credentials")

    compact_ca = tree / "usr/share/ca-certificates/ca-bundle.crt"
    if not compact_ca.is_file() or compact_ca.stat().st_size == 0:
        raise SystemExit("base rootfs is missing its public CA bundle")
    conventional_ca = tree / "etc/ssl/certs/ca-certificates.crt"
    conventional_ca.parent.mkdir(parents=True, exist_ok=True)
    if conventional_ca.exists() or conventional_ca.is_symlink():
        conventional_ca.unlink()
    conventional_ca.symlink_to("/usr/share/ca-certificates/ca-bundle.crt")

    # Preserve manual `wifi connect`, but do not spend scarce RAM on BLE
    # provisioning, cron, the demo web server, or a futile DHCP client at
    # credential-free boot.  These are unrelated to the one-shot fx CLI.
    init_dir = tree / "etc/init.d"
    for name in ("S06home-users", "S46blewifi", "S50crond"):
        path = init_dir / name
        if path.exists() or path.is_symlink():
            path.unlink()
    for path in init_dir.glob("S*inetd*"):
        path.unlink()
    interfaces = tree / "etc/network/interfaces"
    if interfaces.exists():
        lines = [line for line in interfaces.read_text().splitlines()
                 if not re.match(r"^\s*(auto|allow-hotplug)\s+espsta0\s*$", line)]
        interfaces.write_text("\n".join(lines) + "\n")

    copy(TARGET_TOOLS / "fx-wamr", tree / "usr/bin/fx-wamr", 0o755)
    copy(TARGET_TOOLS / "fx-eink", tree / "usr/bin/fx-eink", 0o755)
    copy(TARGET_TOOLS / "fx-example-test", tree / "usr/bin/fx-example-test", 0o755)
    copy(TARGET_TOOLS / "fx-crypto-selftest",
         tree / "usr/bin/fx-crypto-selftest", 0o755)
    copy(TARGET_TOOLS / "einkctl", tree / "usr/bin/einkctl", 0o755)
    copy(DASH, tree / "usr/bin/dash", 0o755)
    copy(REPO / "board/rootfs/fx-demo", tree / "usr/bin/fx-demo", 0o755)
    copy(REPO / "board/rootfs/fx-sd", tree / "usr/bin/fx-sd", 0o755)
    copy(REPO / "board/rootfs/fx-example-preflight",
         tree / "usr/bin/fx-example-preflight", 0o755)
    copy(REPO / "board/rootfs/S07fx-sd-home",
         tree / "etc/init.d/S07fx-sd-home", 0o755)
    example = tree / "usr/share/fx/example"
    copy(REPO / "example/prepare-jail.sh", example / "prepare-jail.sh", 0o755)
    copy(REPO / "example/prompt.md", example / "prompt.md", 0o444)
    for name in ("SPEC.md", "battery.sh", "test.sh"):
        copy(REPO / "example/project" / name, example / "project" / name,
             0o444)
    wasm_metadata = tree / "usr/share/fx/fx-core.wasm.sha256"
    wasm_metadata.parent.mkdir(parents=True, exist_ok=True)
    wasm_metadata.write_text(f"{digest(FX_WASM)}  fx-core.wasm\n")
    wasm_metadata.chmod(0o444)
    copy(REPO / "third_party/fx/LICENSE",
         tree / "usr/share/licenses/fx/LICENSE", 0o444)
    copy(REPO / "third_party/wasm-micro-runtime/LICENSE",
         tree / "usr/share/licenses/wamr/LICENSE", 0o444)
    fx_link = tree / "usr/bin/fx"
    if fx_link.exists() or fx_link.is_symlink():
        fx_link.unlink()
    fx_link.symlink_to("fx-eink")

    motd = tree / "etc/motd"
    existing = motd.read_text() if motd.exists() else ""
    motd.write_text(existing.rstrip() + "\n\n"
                    "fx on e-paper: fx-demo\n"
                    "Live usage: export AI_GATEWAY_API_KEY=... FX_MODEL=...; "
                    "fx 'your prompt'\n"
                    "microSD: fx-sd status; fx-sd mount\n")
    (tree / "bin/busybox").chmod(0o4755)
    (tree / "etc/shadow").chmod(0o600)


def validate_dependencies(tree: Path) -> None:
    readelf = (DRIVER_BUILD / "crosstool-NG/builds/"
                "xtensa-esp32s3-linux-uclibcfdpic/bin/"
                "xtensa-esp32s3-linux-uclibcfdpic-readelf")
    environment = {**os.environ,
                   "XTENSA_GNU_CONFIG": str(DRIVER_BUILD /
                                              "xtensa-dynconfig/esp32s3.so")}
    missing: list[str] = []
    for executable in (tree / "usr/bin/fx-wamr", tree / "usr/bin/fx-eink",
                       tree / "usr/bin/einkctl",
                       tree / "usr/bin/fx-crypto-selftest",
                       tree / "usr/bin/dash"):
        dynamic = subprocess.check_output(
            [str(readelf), "-d", str(executable)], text=True, env=environment)
        for library in re.findall(r"Shared library: \[([^]]+)]", dynamic):
            if not any((tree / directory / library).exists()
                       for directory in ("lib", "usr/lib")):
                missing.append(f"{executable.name}: {library}")
    if missing:
        raise SystemExit("missing target libraries:\n" + "\n".join(missing))


def place(image: bytearray, offset: int, limit: int, payload: Path) -> None:
    data = payload.read_bytes()
    if len(data) > limit:
        raise SystemExit(f"{payload.name} is {len(data)} bytes; limit is {limit}")
    if any(byte != 0xFF for byte in image[offset:offset + len(data)]):
        raise SystemExit(f"image overlap while placing {payload.name}")
    image[offset:offset + len(data)] = data


def build_fx_flash_payload(output: Path) -> None:
    wasm = FX_WASM.read_bytes()
    if len(wasm) > FX_WASM_PARTITION_BYTES - FX_WASM_HEADER_BYTES:
        raise SystemExit("fx Wasm does not fit the 2-MiB data-XIP partition")
    header = struct.pack(
        "<4sIII32s16x", b"FXWM", 1, FX_WASM_HEADER_BYTES, len(wasm),
        bytes.fromhex(digest(FX_WASM)))
    if len(header) != FX_WASM_HEADER_BYTES:
        raise SystemExit("internal fxwasm header size mismatch")
    output.write_bytes(header + wasm)


def main() -> None:
    partitions = parse_partitions(PARTITION_CSV)
    kernel = EXP_OUT / "real-bins/xipImage-fork-quiet"
    firmware = DRIVER_BUILD / "esp-hosted/esp_hosted_ng/esp/esp_driver/network_adapter/build"
    base_files = {
        "bootloader.bin": firmware / "bootloader/bootloader.bin",
        "network_adapter.bin": firmware / "network_adapter.bin",
        "xipImage": kernel,
    }
    required = [
        HOST / "bin/cramfsck", HOST / "bin/mkcramfs", HOST / "sbin/mkfs.jffs2",
        PROGRAMS / "image-profiles.py", PARTITION_CSV, FX_WASM, DASH,
        *(TARGET_TOOLS / name for name in
          ("fx-wamr", "fx-eink", "einkctl", "fx-crypto-selftest")),
        REPO / "board/rootfs/fx-example-preflight",
        *base_files.values(),
    ]
    absent = [str(path) for path in required if not path.is_file()]
    if absent:
        raise SystemExit("missing build inputs (the Linux build may still be running):\n"
                         + "\n".join(absent))

    kernel_config = EXP_OUT / "linux-fork/.config"
    config = kernel_config.read_text()
    for setting in ('CONFIG_BUILTIN_DTB_SOURCE="esp32s3-reterminal-e1001"',
                    "CONFIG_SPI_SPIDEV=y", "CONFIG_SLUB_TINY=y",
                    "CONFIG_XTENSA_NOMMU_FORK=y",
                    "CONFIG_MMC=y", "CONFIG_MMC_SPI=y",
                    "CONFIG_BASE_SMALL=y", "CONFIG_LOG_BUF_SHIFT=15",
                    "CONFIG_INET_TABLE_PERTURB_ORDER=8",
                    "# CONFIG_IPV6 is not set",
                    "# CONFIG_NETFILTER is not set"):
        if setting not in config:
            raise SystemExit(f"kernel configuration is missing: {setting}")

    OUTPUT.mkdir(parents=True, exist_ok=True)
    fx_flash = OUTPUT / "fx-core.flash.bin"
    build_fx_flash_payload(fx_flash)
    base_rootfs = OUTPUT / "rootfs-base.cramfs"
    run("python3", PROGRAMS / "image-profiles.py", "build", "--profile", "base",
        "--output", base_rootfs, "--replace")

    with tempfile.TemporaryDirectory(prefix="fx-rootfs-", dir=OUTPUT) as scratch:
        tree = Path(scratch) / "tree"
        run(HOST / "bin/cramfsck", "-x", tree, base_rootfs)
        configure_rootfs(tree)
        validate_dependencies(tree)

        rootfs = OUTPUT / "rootfs.cramfs"
        run(HOST / "bin/mkcramfs", "-X", "-q", tree, rootfs)
        run(HOST / "bin/cramfsck", rootfs)
        rootfs_size = rootfs.stat().st_size
        if rootfs_size > partitions["rootfs"]["size"]:
            raise SystemExit(
                f"rootfs exceeds its partition by "
                f"{rootfs_size - partitions['rootfs']['size']} bytes")

        verify_tree = Path(scratch) / "verify"
        run(HOST / "bin/cramfsck", "-x", verify_tree, rootfs)
        packed_metadata = verify_tree / "usr/share/fx/fx-core.wasm.sha256"
        expected_metadata = f"{digest(FX_WASM)}  fx-core.wasm\n"
        if packed_metadata.read_text() != expected_metadata:
            raise SystemExit("fx Wasm metadata changed while packaging")

        etc_image = OUTPUT / "etc.jffs2"
        run(HOST / "sbin/mkfs.jffs2", "-l", "-e", "65536", "-U", "-f",
            f"--pad={partitions['etc']['size']}", "-d", tree / "etc",
            "-o", etc_image)
        home_tree = Path(scratch) / "home"
        home_tree.mkdir(mode=0o755)
        home_root = home_tree / "root"
        home_root.mkdir(mode=0o700)
        # dash launches external commands with vfork.  That matters on this
        # NOMMU target: bash's software-banked fork can fragment the sole
        # 2-MiB block needed by fx's WebAssembly linear-memory arena.
        shell_preference = home_root / ".shell"
        shell_preference.write_text("dash\n")
        shell_preference.chmod(0o600)
        home_image = OUTPUT / "home.jffs2"
        run(HOST / "sbin/mkfs.jffs2", "-l", "-e", "65536", "-U", "-f",
            f"--pad={partitions['home']['size']}", "-d", home_tree,
            "-o", home_image)

    generators = list(DRIVER_BUILD.rglob("partition_table/gen_esp32part.py"))
    if not generators:
        raise SystemExit("ESP-IDF gen_esp32part.py was not found")
    partition_bin = OUTPUT / "partition-table.bin"
    run("python3", generators[0], PARTITION_CSV, partition_bin)

    for name, source in base_files.items():
        shutil.copyfile(source, OUTPUT / name)
    images = {
        "factory": OUTPUT / "network_adapter.bin",
        "etc": OUTPUT / "etc.jffs2",
        "linux": OUTPUT / "xipImage",
        "rootfs": OUTPUT / "rootfs.cramfs",
        "fxwasm": fx_flash,
        "home": OUTPUT / "home.jffs2",
    }
    full = bytearray(b"\xFF") * FLASH_BYTES
    place(full, 0, 0x8000, OUTPUT / "bootloader.bin")
    place(full, 0x8000, 0x1000, partition_bin)
    for partition, payload in images.items():
        info = partitions[partition]
        place(full, info["offset"], info["size"], payload)
    merged = OUTPUT / "linux-reterminal-e1001-fx-16m.bin"
    merged.write_bytes(full)

    if len(full) != FLASH_BYTES:
        raise SystemExit("merged image is not exactly 16 MiB")
    if struct.unpack_from("<H", partition_bin.read_bytes(), 0)[0] != 0x50AA:
        raise SystemExit("invalid ESP-IDF partition-table magic")
    manifest = {
        "board": "Seeed reTerminal E1001",
        "flash_image_bytes": FLASH_BYTES,
        "physical_flash_bytes": 32 * 1024 * 1024,
        "upper_16_mib": "preserved by the deployment command",
        "partitions": partitions,
        "rootfs": {
            "image_bytes": (OUTPUT / "rootfs.cramfs").stat().st_size,
            "free_bytes": partitions["rootfs"]["size"]
                          - (OUTPUT / "rootfs.cramfs").stat().st_size,
        },
        "fx_wasm": {"bytes": FX_WASM.stat().st_size,
                    "sha256": digest(FX_WASM),
                    "mode": "WAMR classic interpreter; raw Wasm data-XIP",
                    "flash_offset": FX_WASM_FLASH_OFFSET,
                    "data_address": FX_WASM_FLASH_ADDRESS + FX_WASM_HEADER_BYTES,
                    "header_bytes": FX_WASM_HEADER_BYTES},
        "files": {},
    }
    # Enumerate the release set explicitly.  Build logs or operator notes may
    # coexist in OUTPUT, but must never become mutable manifest inputs merely
    # because they happen to be present there.
    release_names = (
        "bootloader.bin",
        "etc.jffs2",
        "fx-core.flash.bin",
        "home.jffs2",
        "linux-reterminal-e1001-fx-16m.bin",
        "network_adapter.bin",
        "partition-table.bin",
        "rootfs-base.cramfs",
        "rootfs-base.json",
        "rootfs.cramfs",
        "xipImage",
    )
    checksums = []
    for name in release_names:
        path = OUTPUT / name
        if not path.is_file():
            raise SystemExit(f"release artifact disappeared: {path}")
        value = digest(path)
        manifest["files"][path.name] = {"bytes": path.stat().st_size,
                                         "sha256": value}
        checksums.append(f"{value}  {path.name}\n")
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    checksums.append(f"{digest(OUTPUT / 'manifest.json')}  manifest.json\n")
    (OUTPUT / "SHA256SUMS").write_text("".join(checksums))
    print(json.dumps(manifest, indent=2))
    print(f"Built only; nothing flashed: {merged}")


if __name__ == "__main__":
    main()
