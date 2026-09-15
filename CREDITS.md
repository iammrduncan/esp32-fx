# Credits and source provenance

This experiment is integration work built on substantial upstream projects. We did
not create Linux, the ESP32 Linux port, WAMR, fx, or the e-paper controller protocol.
Please credit those projects when sharing this work or building a derivative.

## Linux foundation

- **paulneja and the Linux-on-esp32-S3 contributors** provide the native Linux
  distribution/build recipe, memory/fork work, userspace, and firmware integration
  used here. [Pinned source](https://github.com/paulneja/Linux-on-esp32-S3/tree/72c246369a5bc9426420081b2e5a9050612d0177).
  Project license: GPLv3, with component-specific terms. Its original
  [NOTICE](notices/Linux-on-esp32-S3-NOTICE.txt) is preserved verbatim.
- **Max Filippov (jcmvbkbc) and the Xtensa Linux contributors** provide the underlying
  [Xtensa Linux port](https://github.com/jcmvbkbc/linux-xtensa/tree/3b01ad2a1f71b72b27fefa08e4bf6acbe1de874f),
  [ESP32 Linux build infrastructure](https://github.com/jcmvbkbc/esp32-linux-build),
  Xtensa configuration, toolchain integration, Buildroot adaptations, and esp-hosted
  work. The kernel remains under its Linux license; it is not our kernel port.
- **Espressif Systems and contributors** provide ESP-IDF, esp-hosted, firmware,
  hardware support, and compiler work. The exact firmware/toolchain inputs used by
  the Linux recipe are retained in [the upstream source lock](board/upstream.lock).
- **Linux, Buildroot, GNU toolchain, uClibc-ng, BusyBox, Dash, and other userspace
  contributors** supply the operating system, compiler, libraries, and commands.
  The image is an aggregate of these components, each retaining its own terms.

The [svermigo.cz ESP32 Linux page](https://svermigo.cz/esp32-linux/) was the discovery
reference that prompted this experiment. It is credited as inspiration/reference;
the locked repositories above identify the actual build inputs.

## WebAssembly and agent

- **Bytecode Alliance and WAMR contributors** provide
  [WebAssembly Micro Runtime 2.4.5](https://github.com/bytecodealliance/wasm-micro-runtime/tree/25bd7eb63e828e4bd242cc9b38d260b4b31c6605),
  the engine executing the guest on this board. License: Apache-2.0 WITH LLVM-exception.
  Our local modification is `guest/wamr-readonly.patch`; it is not an upstream feature
  claim. The patch keeps the source bytes immutable in the classic mini-loader.
- **Vercel Labs and fx contributors** provide the
  [fx 0.0.8 agent core](https://github.com/vercel-labs/fx/tree/43c11dcc34a94a76df870af70bdb824579bf18a0),
  including the agent/session/provider/ACP behavior tested here. License: Apache-2.0.
  Our embedded patch changes resource budgets; it does not reimplement the agent.
  The original [third-party notices](notices/fx-THIRD_PARTY_NOTICES.md) are preserved.
- **Zig contributors** provide the compiler building the Wasm guest. **Python
  contributors** provide the workstation tooling and test runtime.
- **Doug Lea** provides [dlmalloc 2.8.6](https://gee.cs.oswego.edu/pub/misc/malloc.c),
  used by the compact guest. The pinned source retains his 2023 copyright and MIT-0
  permission notice.
- The fixed-chunk embedded turn arena in `guest/compact-arena.zig` is local integration
  code, written specifically for this experiment after measuring Zig's standard
  arena growth on the workload. It is not attributed to dlmalloc, fx, WAMR, or Zig.
- **curl/libcurl and Mbed TLS contributors** provide the target HTTP/TLS libraries.
  Their respective curl and Apache-2.0 licenses remain applicable.
- The userspace public-RSA shim is local integration around the ESP32-S3 hardware
  RSA driver already supplied by Linux-on-ESP32-S3 and Espressif contributors. The
  local ioctl does not claim authorship of that accelerator driver or algorithm;
  the kernel patch retains its upstream SPDX license and states the relationship.

MicroWasm is a separate deployment/capability project. This repository demonstrates
the board/runtime foundation and contains plans for integration; it does not rename
WAMR or attribute WAMR's implementation to MicroWasm.

## Display

- **Seeed Studio** provides the reTerminal E1001 board and the
  [Seeed GxEPD2 fork](https://github.com/Seeed-Projects/Seeed_GxEPD2/tree/1100ea37c16b910fd79152f4250c13d802b9c20b).
- **Jean-Marc Zingg and GxEPD2 contributors** provide the underlying
  [GxEPD2 library](https://github.com/ZinggJM/GxEPD2). Its GDEY075T7 driver was the
  reference for our controller register sequence; the library is GPLv3. The C++
  Arduino library is not linked into our Linux C renderer.
- **Good Display** supplies the GDEY075T7 panel/controller documentation and example
  sequences credited by GxEPD2. **Waveshare** is also credited in that driver's header
  for the e-paper board ecosystem. We do not claim to have invented the panel protocol.
- **Linux font contributors** supply `lib/fonts/font_8x16.c`, the GPL-2.0 bitmap font
  from which `tools/build.py` extracts ASCII glyphs. The generated table retains
  that provenance. It is not an original font designed for this project.

## Local work and release scope

Local work includes the E1001 device-tree overlays for display, microSD, and reserved
PSRAM; the constrained fx build profile; native fx/WAMR imports; the immutable-loader
patch; the bounded RSA userspace bridge; ACP/e-paper integration; image packaging;
and the tests/documentation around that integration. The code shows the boundaries
and exact patches so readers can distinguish upstream work from changes.

See [LICENSE.md](LICENSE.md) for file-level terms. Source credits are not a substitute
for preserving license texts, notices, and corresponding source when distributing
binaries. This repository contains source and patches; combined firmware publication
requires a component license/source bundle.
