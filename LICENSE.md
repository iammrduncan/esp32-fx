# Licensing and attribution

This is a mixed-license source project. Existing file-level notices take precedence.
Original scripts, documentation, tests, configuration, and otherwise unmarked local
code are provided under [MIT](LICENSES/MIT.txt), copyright 2026 esp32-fx contributors.
This does not relicense upstream source or patches derived from it.

| Material | License |
| --- | --- |
| `runtime/fx_eink.c` | MIT |
| `runtime/fx_wamr_runner.c`, WAMR patch | Apache-2.0 WITH LLVM-exception |
| `runtime/einkctl.c`, Linux board/driver patches | GPL-2.0-only |
| Generated Linux 8x16 font | GPL-2.0; retain upstream font provenance |
| fx guest patch | Apache-2.0, matching the pinned fx source |
| Linux-on-esp32-S3 build/fork support | GPLv3; see preserved upstream NOTICE |
| Doug Lea's dlmalloc, compact guest profile | [MIT-0](LICENSES/MIT-0.txt), copyright 2023 Doug Lea |

Full [Apache-2.0](LICENSES/Apache-2.0.txt),
[Apache with LLVM exception](LICENSES/Apache-2.0-WITH-LLVM-exception.txt), and
[GPL-2.0](LICENSES/GPL-2.0-only.txt) texts are included.
The [GPLv3](LICENSES/GPL-3.0-only.txt) text is included for the Linux integration
project and Seeed/GxEPD2 reference. The target build also links the upstream
`fork-compat.c` into `fx-eink`; do not describe that linked target as solely MIT
just because the local frontend source is MIT. This source repository does not
redistribute that upstream implementation or the combined executable.

The modified fx patches reduce embedded buffer/history/stack budgets. The WAMR patch
adds immutable input handling and out-of-line loader rewrites. The Linux patches add
the E1001 display and microSD device tree, a reserved-memory region, and a bounded
userspace interface to the existing ESP32-S3 RSA driver. These are local modifications
to pinned upstream projects; preserve their upstream copyright and license notices
when applying them. `runtime/fx_rsa_hw.c` is an MIT-licensed userspace shim; it does not
relicense or claim authorship of the kernel accelerator.

`einkctl` derives its font from Linux `lib/fonts/font_8x16.c` and follows the panel
sequence from Seeed's GxEPD2 GDEY075T7 implementation. The renderer executable is
GPL-covered. Linux, Buildroot packages, ESP-IDF firmware, curl/TLS libraries, fx,
and WAMR each retain their own licenses/notices.

The repository contains source, patches, and instructions,
not Linux firmware images or prebuilt guest/runtime binaries. Before distributing
combined images or executables, collect each component's notices and corresponding
source/build inputs, including GPL-covered programs and generated fonts. Merely
including two license files in a rootfs is not a complete binary release process.
Exact upstream identities are in `sources.lock.json` and the Linux build's own lock.
See [CREDITS.md](CREDITS.md) and the unmodified notices under `notices/`.
