"""The single supported build: compact fx + classic WAMR + E1001 Linux."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "localhost/esp32-fx-host-tools:bookworm"
BASE = ROOT / "build/linux-reproduce/base"
DRIVER = BASE / "refs/esp32-linux-build/build"
TOOLCHAIN = DRIVER / "crosstool-NG/builds/xtensa-esp32s3-linux-uclibcfdpic"
SYSROOT = DRIVER / "build-buildroot-esp32s3_devkit_c1_16m/host/xtensa-buildroot-linux-uclibc/sysroot"
LOG = None


def run(*args, cwd=ROOT, env=None, capture=False):
    result = subprocess.run([str(a) for a in args], cwd=cwd,
                            env={**os.environ, **(env or {})}, check=True,
                            stdout=subprocess.PIPE if capture else LOG,
                            stderr=LOG, text=True)
    return result.stdout if capture else None


def container(*args, env=None, capture=False):
    command = ["podman", "run", "--rm", "--userns=keep-id", "--network", "host",
               "--volume", f"{ROOT}:/work", "--workdir", "/work"]
    for key, value in (env or {}).items():
        command += ["--env", f"{key}={value}"]
    return run(*command, IMAGE, *args, capture=capture)


def mounted(path):
    return "/work/" + str(Path(path).relative_to(ROOT))


def snapshot(source, destination):
    destination.mkdir(parents=True)
    with tempfile.TemporaryFile() as archive:
        subprocess.run(["git", "-C", str(source), "archive", "HEAD"],
                       stdout=archive, check=True)
        archive.seek(0)
        with tarfile.open(fileobj=archive) as stream:
            # The immutable upstream checkout is the trusted build input.
            stream.extractall(destination, members=(m for m in stream
                              if not m.name.startswith("images/")), filter="data")


def dependencies():
    for name in ("git", "podman", "patch", "curl", "tar"):
        if not shutil.which(name):
            raise RuntimeError(f"Install {name} before running prep")
    run(sys.executable, ROOT / "tools/fetch.py")
    zig = ROOT / ".tools/zig-x86_64-linux-0.16.0/zig"
    if not zig.exists():
        zig.parent.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=zig.parent.parent) as temporary:
            archive = Path(temporary) / "zig.tar.xz"
            run("curl", "--fail", "--location", "--retry", "2", "--max-time", "600",
                "-o", archive, "https://ziglang.org/download/0.16.0/zig-x86_64-linux-0.16.0.tar.xz")
            expected = "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00"
            if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
                raise RuntimeError("Zig archive checksum mismatch")
            with tarfile.open(archive) as stream:
                stream.extractall(temporary, filter="data")
            shutil.move(str(Path(temporary) / zig.parent.name), zig.parent)
    if run(zig, "version", capture=True).strip() != "0.16.0":
        raise RuntimeError("Zig version must be 0.16.0")
    run("podman", "build", "--network", "host", "--build-arg", f"BUILDER_UID={os.getuid()}",
        "--build-arg", f"BUILDER_GID={os.getgid()}", "-f", ROOT / "board/Containerfile",
        "-t", IMAGE, ROOT)


def patch_wamr():
    lock = json.loads((ROOT / "sources.lock.json").read_text())["sources"]["wamr"]
    source = ROOT / "third_party/wasm-micro-runtime"
    patch = ROOT / lock["patch"]
    if hashlib.sha256(patch.read_bytes()).hexdigest() != lock["patch_sha256"]:
        raise RuntimeError("WAMR patch checksum mismatch")
    if run("git", "rev-parse", "HEAD", cwd=source, capture=True).strip() != lock["commit"]:
        raise RuntimeError("Unexpected WAMR revision")
    already = subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                             cwd=source, capture_output=True).returncode == 0
    if not already:
        run("git", "apply", "--check", patch, cwd=source)
        run("git", "apply", patch, cwd=source)
    diff = run("git", "diff", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/",
               cwd=source, capture=True)
    if diff != patch.read_text():
        raise RuntimeError("WAMR has additional local modifications")


def font():
    source = ROOT / "third_party/linux-xtensa/lib/fonts/font_8x16.c"
    values = re.findall(r"^\s*(0x[0-9a-fA-F]{2}),", source.read_text(), re.M)[32*16:127*16]
    if len(values) != 95*16:
        raise RuntimeError("Unexpected Linux font layout")
    output = ROOT / "build/generated/font8x16_ascii.inc"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("/* SPDX-License-Identifier: GPL-2.0-only */\n"
                      "/* From Linux lib/fonts/font_8x16.c; keep byte data in RAM. */\n"
                      "static unsigned char font8x16_ascii[95][16] EINKCTL_RAM_DATA = {\n"
                      + "\n".join("    { " + ", ".join(values[i:i+16]) + " },"
                                  for i in range(0, len(values), 16)) + "\n};\n")


def wamr(target=False):
    directory = "wamr-target" if target else "wamr-host-targetsem"
    flags = {"INTERP": 1, "FAST_INTERP": 0, "MINI_LOADER": 1, "AOT": 0, "JIT": 0,
             "FAST_JIT": 0, "LIBC_BUILTIN": 1, "LIBC_WASI": 1, "LIB_PTHREAD": 0,
             "LIB_WASI_THREADS": 0, "MULTI_MODULE": 0, "SIMD": 0, "REF_TYPES": 1,
             "GC": 0, "MEMORY_PROFILING": 0}
    options = ["-DCMAKE_BUILD_TYPE=MinSizeRel",
               "-DCMAKE_C_FLAGS=-DWASM_ENABLE_LABELS_AS_VALUES=0 -DWASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS=0"]
    env = {}
    if target:
        flags.update(PLATFORM="linux", TARGET="XTENSA", INVOKE_NATIVE_GENERAL=1,
                     ALLOC_WITH_USAGE=1, ALLOC_WITH_USER_DATA=1)
        options += ["-DCMAKE_TOOLCHAIN_FILE=/work/board/toolchain.cmake",
                    "-DXTENSA_TOOLCHAIN_ROOT=" + mounted(TOOLCHAIN)]
        env["XTENSA_GNU_CONFIG"] = mounted(DRIVER / "xtensa-dynconfig/esp32s3.so")
    source = "/work/third_party/wasm-micro-runtime"
    if not target:
        source += "/product-mini/platforms/linux"
    container("cmake", "-S", source, "-B", f"/work/build/{directory}", "-G", "Ninja",
              *options, *(f"-DWAMR_BUILD_{k}={v}" for k,v in flags.items()), env=env)
    container("cmake", "--build", f"/work/build/{directory}", "--parallel", "4", env=env)


def host():
    font()
    wamr()
    for directory in ("fx-wamr-host", "fx-eink-host", "einkctl-host", "example-host"):
        (ROOT / "build" / directory).mkdir(parents=True, exist_ok=True)
    flags = ["-std=c11", "-Os", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
             "-fdata-sections"]
    curl = container("pkg-config", "--cflags", "--libs", "libcurl", capture=True).split()
    container("cc", *flags, "-DFX_WAMR_NO_AOT=1",
              "-I/work/third_party/wasm-micro-runtime/core/iwasm/include",
              "-I/work/third_party/wasm-micro-runtime/core/shared/platform/include",
              "/work/runtime/fx_wamr_runner.c", "/work/runtime/fx_wamr_native.c",
              "/work/runtime/fx_example_tools.c", "/work/build/wamr-host-targetsem/libiwasm.a",
              *curl, "-Wl,--gc-sections", "-ldl", "-lpthread", "-lm",
              "-o", "/work/build/fx-wamr-host/fx-wamr-targetsem")
    for source, output in (("fx_eink.c", "fx-eink-host/fx-eink"),
                           ("einkctl.c", "einkctl-host/einkctl"),
                           ("fx_example_test.c", "example-host/fx-example-test")):
        container("cc", *flags, "-I/work/build/generated", "/work/runtime/" + source,
                  "-Wl,--gc-sections", "-o", "/work/build/" + output)


def guest():
    zig = ROOT / ".tools/zig-x86_64-linux-0.16.0/zig"
    inputs = sorted((ROOT / "guest").iterdir())
    digest = hashlib.sha256(b"".join(p.read_bytes() for p in inputs if p.is_file())).hexdigest()[:16]
    work = ROOT / "build" / ("fx-src-" + digest)
    if not work.exists():
        with tempfile.TemporaryDirectory(prefix="fx-guest-", dir=ROOT / "build") as temporary:
            stage = Path(temporary) / "source"
            snapshot(ROOT / "third_party/fx", stage)
            for name in ("embedded-esp32.patch", "compact-allocator.patch", "compact-stack.patch"):
                run("patch", "--batch", "-p1", "-i", ROOT / "guest" / name, cwd=stage)
            for source, target in ((ROOT / "third_party/dlmalloc/malloc.c", "embedded_dlmalloc.c"),
                                   (ROOT / "guest/dlmalloc-config.h", "embedded_dlmalloc_config.h"),
                                   (ROOT / "guest/dlmalloc-wrapper.c", "embedded_dlmalloc_wrapper.c"),
                                   (ROOT / "guest/compact-allocator.zig", "embedded_allocator.zig"),
                                   (ROOT / "guest/compact-arena.zig", "embedded_arena.zig")):
                shutil.copyfile(source, stage / "src" / target)
            for path in (stage / "src").rglob("*.zig"):
                text = path.read_text()
                path.write_text(text.replace("std.heap.c_allocator", '(if (@hasDecl(@import("root"), "embedded_allocator")) @import("root").embedded_allocator else std.heap.c_allocator)'))
            stage.rename(work)
    run(zig, "test", ROOT / "guest/compact-arena.zig")
    run(zig, "build", "fx-core-wasm", "-Dwasm-surface=core", "--prefix", ROOT / "build/fx-compact", cwd=work)
    run(zig, "build-exe", ROOT / "guest/compact-allocator-test.zig", "-target", "wasm32-wasi",
        "-O", "ReleaseSmall", "-lc", "--stack", "131072", "-cflags", "-Os", "-fno-builtin",
        "--", work / "src/embedded_dlmalloc_wrapper.c",
        "-femit-bin=" + str(ROOT / "build/fx-compact/allocator-test.wasm"))
    container("/work/build/fx-wamr-host/fx-wamr-targetsem", "/work/build/fx-compact/allocator-test.wasm",
              env={"FX_WAMR_MAX_PAGES": "24", "FX_WAMR_READONLY_MODULE": "1",
                   "AI_GATEWAY_API_KEY": "allocator-test-no-network", "FX_MODEL": "fixture/wamr"})


def linux():
    from overlay import apply
    source = ROOT / "third_party/Linux-on-esp32-S3"
    tree = BASE / "Linux-on-esp32-S3"
    if not tree.exists():
        snapshot(source, tree)
        (BASE / "source-commit.txt").write_text(run("git", "rev-parse", "HEAD", cwd=source, capture=True))
    expected = json.loads((ROOT / "sources.lock.json").read_text())["sources"]["linux_on_esp32_s3"]["commit"]
    if (BASE / "source-commit.txt").read_text().strip() != expected:
        raise RuntimeError("Linux build snapshot revision differs from source lock")
    apply(tree)
    image = "localhost/esp32-fx-linux-builder:bookworm"
    run("podman", "build", "--network", "host", "--build-arg", f"BUILDER_UID={os.getuid()}",
        "--build-arg", f"BUILDER_GID={os.getgid()}", "-f", tree / "build/Dockerfile",
        "-t", image, ROOT / "board")
    run("podman", "run", "--rm", "--userns=keep-id", "--network", "host", "--volume", f"{BASE}:/work",
        "--env", f"JOBS={os.environ.get('JOBS', '4')}", "--env", "GIT_AUTHOR_NAME=builder",
        "--env", "GIT_AUTHOR_EMAIL=builder@example.invalid", "--env", "GIT_COMMITTER_NAME=builder",
        "--env", "GIT_COMMITTER_EMAIL=builder@example.invalid", image)

    # Upstream stage stamps intentionally preserve a costly Linux build.
    # Apply the metadata reservation to an already-built kernel as well as
    # fresh builds, then let make update only the changed device tree/image.
    experiment = tree / "experiments/mmu-poc/out"
    kernel = experiment / "linux-fork"
    patch = ROOT / "board/kernel/08-kernel-fx-runtime-metadata.patch"
    applied = subprocess.run(["patch", "--force", "--dry-run", "-R", "-p1", "-i", str(patch)],
                             cwd=kernel, capture_output=True).returncode == 0
    if not applied:
        run("patch", "--batch", "--forward", "-p1", "-i", patch, cwd=kernel)
    run("podman", "run", "--rm", "--userns=keep-id", "--network", "none",
        "--volume", f"{BASE}:/work", "--entrypoint", "make",
        "--env", "XTENSA_GNU_CONFIG=/work/refs/esp32-linux-build/build/xtensa-dynconfig/esp32s3.so",
        "--env", "KBUILD_BUILD_TIMESTAMP=Sat Sep 5 00:00:00 UTC 2026",
        "--env", "KBUILD_BUILD_USER=builder", "--env", "KBUILD_BUILD_HOST=esp32-repro",
        image, "-C", "/work/Linux-on-esp32-S3/experiments/mmu-poc/out/linux-fork",
        "-j4", "ARCH=xtensa", "CROSS_COMPILE=/work/refs/esp32-linux-build/build/"
        "crosstool-NG/builds/xtensa-esp32s3-linux-uclibcfdpic/bin/xtensa-esp32s3-linux-uclibcfdpic-",
        "xipImage")
    decoded = run(kernel / "scripts/dtc/dtc", "-I", "dtb", "-O", "dts",
                  kernel / "arch/xtensa/boot/dts/esp32s3-reterminal-e1001.dtb", capture=True)
    reservation = re.search(r"fx-wamr-arena@3ded0000\s*\{.*?reg\s*=\s*<([^>]+)>;",
                            decoded, re.S)
    if not reservation or [int(n, 0) for n in reservation[1].split()] != [0x3ded0000, 0x130000]:
        raise RuntimeError("Compiled kernel has the wrong reserved PSRAM layout")
    shutil.copyfile(kernel / "arch/xtensa/boot/xipImage", experiment / "real-bins/xipImage-fork-quiet")


def target():
    wamr(target=True)
    output = ROOT / "build/target-tools"
    output.mkdir(parents=True, exist_ok=True)
    prefix = str(TOOLCHAIN / "bin/xtensa-esp32s3-linux-uclibcfdpic")
    env = {"XTENSA_GNU_CONFIG": str(DRIVER / "xtensa-dynconfig/esp32s3.so")}
    common = ["--sysroot=" + str(SYSROOT), "-std=c11", "-Oz", "-flto", "-Wall", "-Wextra",
              "-Werror", "-mfdpic", "-mauto-litpools", "-fPIC", "-ffunction-sections", "-fdata-sections"]
    defines = {"DEFAULT_FX_MODULE": '"/usr/share/fx/fx-core.wasm"', "FX_WAMR_NO_AOT": "1",
               "FX_WAMR_FLASH_MODULE_ADDRESS": "0x3cd80000UL", "FX_WAMR_FLASH_PARTITION_BYTES": "0x200000U",
               "FX_WAMR_LINEAR_RESERVE_BYTES": "0x100000U", "FX_WAMR_NATIVE_STREAM_RESERVE_BYTES": "0x10000U",
               "FX_WAMR_MBEDTLS_STREAM_CA": "1",
               "FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES": "65536U", "FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES": "32768U",
               "FX_WAMR_BORROWED_LINEAR_MIN": "0x3d800000UL", "FX_WAMR_BORROWED_LINEAR_MAX": "0x3e000000UL",
               "FX_WAMR_MAX_STREAM_BYTES": "65536", "WASM_MEM_ALLOC_WITH_USAGE": "1", "WASM_MEM_ALLOC_WITH_USER_DATA": "1"}
    run(prefix + "-gcc", *common, *(f"-D{k}={v}" for k,v in defines.items()),
        "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/iwasm/include"),
        "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/shared/platform/include"),
        *(ROOT / "runtime" / n for n in ("fx_wamr_runner.c", "fx_wamr_native.c", "fx_example_tools.c", "fx_rsa_hw.c")),
        ROOT / "build/wamr-target/libiwasm.a",
        "-Wl,--gc-sections,--export-dynamic,-z,stack-size=32768", "-Wl,-rpath-link," + str(SYSROOT / "usr/lib"),
        "-lcurl", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-latomic", "-ldl", "-lpthread", "-lm",
        "-o", output / "fx-wamr", env=env)
    recipes = {
        "fx-example-test": ([ROOT / "runtime/fx_example_test.c"], [], 16384),
        "fx-crypto-selftest": ([ROOT / "runtime/fx_crypto_selftest.c", ROOT / "runtime/fx_rsa_hw.c"],
                               ["-Wl,--export-dynamic", "-lmbedtls", "-lmbedx509", "-lmbedcrypto", "-ldl"], 32768),
        "fx-eink": ([ROOT / "runtime/fx_eink.c", BASE / "Linux-on-esp32-S3/experiments/mmu-poc/fork/fork-compat.c"],
                    ['-DDEFAULT_MODULE="/usr/share/fx/fx-core.wasm"', "-DFX_EINK_WAMR_LAUNCH_GUARD_BYTES=0x100000U",
                     "-DFX_EINK_WAMR_LAUNCH_GUARD_ADDRESS=0x3df00000UL", "-DFX_EINK_NATIVE_STREAM_GUARD_BYTES=0x10000U",
                     "-DFX_EINK_NATIVE_STREAM_GUARD_ADDRESS=0x3def0000UL", "-pthread"], 32768),
        "einkctl": ([ROOT / "runtime/einkctl.c"], ["-I" + str(ROOT / "build/generated")], 65536),
    }
    for name, (sources, flags, stack) in recipes.items():
        run(prefix + "-gcc", *common, *sources, *flags, f"-Wl,--gc-sections,-z,stack-size={stack}",
            "-o", output / name, env=env)
    symbols = run(prefix + "-nm", "-S", output / "einkctl", capture=True, env=env)
    if not re.search(r" [dD] font8x16_ascii(?:$|\.)", symbols, re.M):
        raise RuntimeError("Display font must be in RAM-backed data")
    run(prefix + "-strip", "--strip-unneeded", "-R", ".xt.prop", "-R", ".xt.lit",
        *(output / name for name in ["fx-wamr", *recipes]), env=env)


def prep(host_only=False):
    global LOG
    (ROOT / "build").mkdir(exist_ok=True)
    with (ROOT / "build/prep.log").open("w") as log:
        LOG = log
        try:
            print("Preparing pinned tools and sources… (details: build/prep.log)", flush=True)
            dependencies()
            patch_wamr()
            print("Building the runtime and compact fx guest…", flush=True)
            host()
            guest()
            print("Testing protocol, tools, and rendering…", flush=True)
            run(sys.executable, ROOT / "tests/test-runtime.py")
            container("python3", "/work/tests/test-memory.py")
            run("podman", "run", "--rm", "--user", "0", "--volume", f"{ROOT}:/work:ro",
                IMAGE, "python3", "/work/tests/test-example-tools.py")
            for name in ("load", "serial", "network", "deploy-wasm", "tool-trace", "linux-overlay"):
                run(sys.executable, ROOT / f"tests/test-{name}.py")
            if not host_only:
                print("Building Linux and the board image (first build takes several hours)…", flush=True)
                linux()
                container("python3", "/work/tests/test-tls.py")
                target()
                run(sys.executable, ROOT / "tools/package.py")
        finally:
            LOG = None
    print("Prepared. Next: ./esp32-fx load" if not host_only else "Host build and tests passed.")
