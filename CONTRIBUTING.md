# Contributing

Keep changes focused on the supported E1001 workflow. Start with
`./esp32-fx prep --host-only`; it builds the real guest and runs runtime/tool/display
regressions without hardware or credentials.

For board changes, identify the hardware and source revision and explain what you
tested physically. Preserve upstream locks, SPDX headers, notices, and credits.
Keep credentials, raw logs, firmware images, and recovery backups out of Git.

Contributions follow the file-level terms in [LICENSE.md](LICENSE.md).
