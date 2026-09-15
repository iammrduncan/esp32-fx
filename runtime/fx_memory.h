// SPDX-License-Identifier: MIT
#ifndef FX_MEMORY_H
#define FX_MEMORY_H
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define FX_METADATA_ADDRESS 0x3ded0000UL
#define FX_METADATA_BYTES 0x20000U

static inline int fx_verify_reserved_psram(void)
{
    /* Kernel pathname copying needs RAM-backed, byte-readable storage on the
     * ESP32 instruction-flash bus. Check before touching any reserved arena. */
    static char path[] =
        "/proc/device-tree/reserved-memory/fx-wamr-arena@3ded0000/reg";
    uint8_t bytes[8];
    uint32_t address, length;
    ssize_t amount;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "fx: reserved PSRAM node is absent\n");
        return -1;
    }
    amount = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (amount != (ssize_t)sizeof(bytes))
        return -1;
    address = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
              | ((uint32_t)bytes[2] << 8) | bytes[3];
    length = ((uint32_t)bytes[4] << 24) | ((uint32_t)bytes[5] << 16)
             | ((uint32_t)bytes[6] << 8) | bytes[7];
    if (address != FX_METADATA_ADDRESS || length != 0x130000U) {
        fprintf(stderr, "fx: reserved PSRAM layout mismatch; update the host image\n");
        return -1;
    }
    return 0;
}
#endif
