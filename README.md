# fx on ESP32-S3

Run the [fx](https://github.com/vercel-labs/fx) coding agent on a Seeed reTerminal
E1001 and read its answer on the e-paper display. Native Linux boots on the ESP32-S3;
[WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) runs the fx Wasm core.
Model inference uses a remote provider.

## Prep → load → execute

You need an **E1001 N32R8** (32 MiB flash, 8 MiB PSRAM), USB power, and a Linux
x86-64 workstation with **Python 3.12+, Git, Podman, curl, tar, and patch**.
Your user must be able to run rootless Podman and access the USB serial port.

```sh
./esp32-fx prep
./esp32-fx load
./esp32-fx execute --demo
```

- **prep** downloads the pinned sources/toolchain, builds and tests fx, and builds
  the Linux image. The first build takes several hours and needs about 40 GiB.
  Later runs reuse the build cache.
- **load** detects the connected board, saves and verifies a private recovery
  backup, installs the image, and logs in. This replaces the lower 16 MiB, including
  onboard `/etc` and `/home`; the upper flash half is preserved.
- **execute** runs fx and refreshes the display. `--demo` uses a fixed offline
  provider response. For a live model, put your settings in `.env`:

```dotenv
WIFI_SSID="your-network"
WIFI_PW="your-password"
AI_GATEWAY_API_KEY="your-key"
FX_MODEL="provider/model"
```

```sh
chmod 600 .env
./esp32-fx execute "Explain what you can do on this device."
```

Wi-Fi, clock setup, and the saved device login are handled automatically. Add
`--serial /dev/ttyUSB0` when more than one serial device is connected.
The pinned fx guest uses the AI Gateway protocol. Credentials enter guest memory;
use a development key. See [hardware notes](docs/hardware.md) for recovery and SD.

## Coding example

With a FAT microSD card inserted and `.env` configured:

```sh
./esp32-fx execute --example
```

fx reads the [project](example/project/SPEC.md), fixes its battery thresholds, runs
the tests, and puts the answer on the display. [Example details](example/README.md).

## Load Wasm without reflashing

After the initial host installation:

```sh
./esp32-fx load app.wasm --name app --storage sd
./esp32-fx execute --module app --storage sd
```

Use `--storage ram` for a volatile upload. The loader verifies the module's size and
SHA-256 on the board. SD survives reboot; RAM does not. The board must be reachable
from the workstation over Wi-Fi for transfer.
Standalone modules use an offline provider fixture and receive no live provider key.

Small Wasm modules have run through this path. The complete fx module still uses
its flash slot: loading all 2.07 MB into RAM needs further memory work. A resident
MicroWasm deployment service remains future work.

## Source layout

```text
esp32-fx       prep / load / execute command
runtime/       C: WAMR host, fx client, tools, e-paper driver
guest/         fx allocator changes and fx/WAMR patches
board/         E1001 Linux configuration, rootfs utilities, build container
tools/         Python: build, serial, deployment, and example runner
tests/         Python: runtime and deployment regressions
example/       small coding project and prompt
docs/          architecture and hardware notes
```

The native runtime is C. Workstation automation and tests are Python. Zig is used
only for changes to the upstream Zig guest; a few shell files run in the board's
BusyBox userspace and form the example project. There is no Node.js dependency.
Generated output, fetched sources, credentials, and recovery backups are ignored.

## Status and credit

Physical checks cover repeated real-provider read/edit/test sessions on one boot,
e-paper refresh, SD/RAM Wasm uploads and execution, and rejection of untrusted TLS
certificates. This is a small, experimental one-shot CLI. Linux has no MMU or
ordinary swap; an SD card provides storage, not extra RAM. See
[architecture](docs/architecture.md) for the memory budget and current limits.

Built on [Linux-on-esp32-S3](https://github.com/paulneja/Linux-on-esp32-S3),
[Max Filippov's Xtensa Linux work](https://github.com/jcmvbkbc/linux-xtensa), WAMR,
fx, and [Seeed's GxEPD2 driver](https://github.com/Seeed-Projects/Seeed_GxEPD2).
[Credits](CREDITS.md), [licenses](LICENSE.md), and [pinned sources](sources.lock.json).
