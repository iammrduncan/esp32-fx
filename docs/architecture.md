# Architecture

```text
ESP32-S3: native Xtensa Linux 6.11, NOMMU
  fx-eink       one-shot ACP client
    fx-wamr     WAMR classic interpreter and bounded C imports
      fx Wasm   upstream fx 0.0.8 plus embedded memory changes
        HTTPS   remote model provider
    einkctl     text layout and physical e-paper refresh
```

The single supported guest is the compact fx build. Its allocator uses dlmalloc and
a bounded turn arena. The WAMR patch keeps module input immutable so the guest source
can remain mapped from flash. Native synchronous imports handle HTTP and the three
example tools; this execution path does not need JSPI.

The example's capabilities are deliberately small: read one of three project files,
replace `battery.sh`, and invoke a fixed unprivileged test helper. The specification
and test files cannot be edited through that tool interface. The agent's output goes
through ACP to the native frontend, which invokes the renderer after WAMR exits.

## Memory

The E1001 has 32 MiB flash and 8 MiB PSRAM. The image uses the lower 16 MiB; the current
Linux MTD map leaves the upper half unused. Rootfs has an 8.25 MiB slot. The fx Wasm
slot starts at `0xd80000`, occupies 2 MiB, and contains a 64-byte header followed by
the module. Kernel and rootfs XIP bases are `0x140000` and `0x540000`.

The board profile reserves 1 MiB for guest linear memory, 64 KiB for a provider
response, and 128 KiB for WAMR's function-instance table and execution environment.
Both launchers verify the device-tree reservation before using it. Linux reports 6,520 KiB after
those reservations. The CLI syncs pending writes and reclaims disposable filesystem
caches **before starting a process**, so the ELF loader can allocate its data and
stack. The runner repeats this before reserving its operation blocks. NOMMU cannot
compact scattered SD/rootfs cache pages; reclaiming only inside the runner is too
late for the loader. The interpreter's 31 KiB frame stack and its metadata share
the fixed runtime arena with the function table. Separate 64 KiB and 32 KiB guards
are lent only to the shell test, and remain reserved during provider requests.
The fixed runtime arena admits two allocations totaling at most 128 KiB; other
runtime allocations are limited to 32 KiB. Modules exceeding this profile fail rather than requesting a
large, fragmentation-sensitive block from Linux. Network setup reuses a matching,
healthy Wi-Fi connection instead of restarting WPA and DHCP on every execution.

TLS reads the existing PEM trust bundle one certificate at a time (8 KiB maximum
per certificate), avoiding a whole-bundle temporary allocation on every request.
Certificate and hostname verification stay enabled; invalid bundles fail closed.
The 48,000-byte framebuffer is allocated after fx exits. The font table must be
RAM-backed; byte reads from flash caused the
corrupted display during development. The build verifies the font's data section.

Three physical coding sessions completed on one boot in 125–152 seconds each,
with all 14 project checks passing, a 16-page guest maximum, and a minimum sampled
452 KiB free system memory. Live prompting, SD/RAM module deployment, and negative
TLS checks also passed between coding sessions. Device logs confirmed e-paper
refresh completion and no allocation failures during this sequence. This qualifies
the bounded example, not arbitrary prompts or an indefinite soak test. Rootfs is
about 6.54 MB and fx about 2.07 MB; exact sizes and hashes are in the build manifest.

## Deployment and limits

`load` installs the host image. `load module.wasm` transfers a raw Wasm object to RAM
or SD over a temporary HTTP server, verifies size/hash, and records its digest.
`execute --module NAME` rehashes that object before invoking WAMR. These module
operations do not use esptool or write onboard flash.

The RAM/SD path has been demonstrated with a 50-byte WASI module. The complete fx
source still resides in flash because keeping another 2.07 MB in PSRAM exceeds the
qualified budget. SD does not enable Linux swap in this NOMMU port.

MicroWasm integration still needs a resident authenticated receiver, bounded protocol,
crash-consistent SD activation, full fx loading without flash, and credential
confinement. Persistent agent sessions, arbitrary device tools, and a full terminal UI
are also outside the current implementation.

## Development

`./esp32-fx prep --host-only` builds the compact guest and native host and runs the
protocol, rendering, and seven-round coding tests with a local provider fixture.
It needs no board or credentials. The fixed test helper runs inside the build container.

Implementation is grouped by responsibility: `tools/build.py` owns the build,
`tools/cli.py` owns the three commands, `tools/board_io.py` owns serial framing, and
`tools/deploy.py` owns RAM/SD upload. `tools/package.py` assembles and checks the image.
The Linux recipe comes from the locked upstream repository; `tools/overlay.py` applies
the E1001 changes. Build outputs remain under `build/` and sources under `third_party/`.

Individual regression tests use Python's standard library. Run a test directly with
`python3 tests/test-NAME.py`. The C capability tests need a compiler and can run inside
the host build container. Full `prep` also tests bounded certificate loading against
the pinned Mbed TLS sources from the Linux build; it checks every decoded trust root
and rejects empty, malformed, truncated, and oversized certificates.
Historical bring-up records and removed experiments remain
available in Git history.
