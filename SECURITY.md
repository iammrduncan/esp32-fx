# Security

This is an experimental NOMMU Linux host. Native code has no process memory isolation,
the provider key enters guest memory, and the prototype does not enable secure boot
or flash encryption. Use development credentials on a trusted network.

Keep `.env`, raw serial/model logs, and device backups private. Network setup rotates
the packaged development password and stores the replacement under `private/`.

Report sensitive issues privately to a repository maintainer. Do not include live
credentials or raw flash dumps in an issue. Only the current main branch is maintained.
