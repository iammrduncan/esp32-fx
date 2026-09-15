# E1001 hardware and recovery

The supported board is a Seeed reTerminal E1001 N32R8: ESP32-S3, 32 MiB flash, 8 MiB
OPI PSRAM. Use USB power during installation. Other ESP32-S3 boards need a separate
device tree and memory profile.

The 800×480 GDEY075T7 panel uses SPI mode 0 at 2 MHz: MOSI 9, clock 7, chip select 10,
DC 11, reset 12, busy 13 (active low). A full refresh takes several seconds.

## Installation

```sh
./esp32-fx prep
./esp32-fx load
./esp32-fx execute --demo
```

`load` requires a detected ESP32-S3 with 32 MiB flash. It saves and verifies a full
device backup in `private/` before writing the lower 16 MiB. It verifies the new image
against its manifest before the write and against flash afterward. Existing verified
backups are reused. Keep a second private copy for recovery.

The board must be an E1001 N32R8; flash-size detection alone cannot identify its wiring
or PSRAM. `load` replaces onboard `/etc` and `/home`. SD contents and the upper flash
half are preserved. No secure-boot or encryption fuses are changed.

The CLI selects the only connected USB serial device. Specify `--serial` when needed.
The console transport preserves DTR/RTS and permits a single owner. Close other
terminal programs before running a command.

## Network and login

The image boots with automatic Wi-Fi and remote login services disabled. `execute`
reads `.env`, logs in, provisions Wi-Fi, sets the clock for verified TLS, and saves a
random replacement root password under `private/`. Existing keys `VERCEL_API` and
`VERCEL_MODEL` are accepted as aliases for `AI_GATEWAY_API_KEY` and `FX_MODEL`.

The offline `--demo` command needs no credentials. Before initial network setup,
the local root password is the upstream development default `changeme123`.
The current provider key is passed into guest memory; use a short-lived development key.

## SD storage

Use a FAT-formatted card. The helpers do not format or repartition it. The coding
example and SD module loader mount it at `/media/sd` automatically. At the board console:

```sh
fx-sd status
fx-sd home          # bind the card's home tree over /home for this boot
fx-sd enable-home   # also bind it on future boots
fx-sd disable-home  # use internal /home on future boots
```

Keep the card inserted while it backs `/home`. Power-cut recovery and crash-consistent
module activation remain unqualified; unmount cleanly before removing power or media.
An SD file is persistent storage, not a swapfile or additional PSRAM.

## Stock recovery

Select the verified backup for the exact device. A stock restore deliberately writes
all 32 MiB. The build container includes esptool; use its normal `write-flash` and
`verify-flash` commands at offset zero with that backup. Device dumps and credentials
must remain private. Retired development commands remain available in Git history.
