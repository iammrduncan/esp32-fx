#!/usr/bin/env python3
"""Exercise the board allocator's fixed metadata and guest-memory budgets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#define main unused_runner_main
#include "runtime/fx_wamr_runner.c"
#undef main
#include <assert.h>
int main(void) {
    struct wamr_allocator a = {0};
    void *large, *second, *small;
    a.metadata = malloc(FX_METADATA_BYTES);
    a.linear_memory = malloc(65536);
    a.capacity = 65536;
    assert(a.metadata && a.linear_memory);
    large = wamr_malloc(Alloc_For_Runtime, &a, 67340);
    assert(large == a.metadata && a.metadata_blocks[0].size == 67340);
    memset(large, 42, 67340);
    second = wamr_malloc(Alloc_For_Runtime, &a, 40000);
    assert(second && second != large && (uintptr_t)second % 8 == 0);
    assert(!wamr_malloc(Alloc_For_Runtime, &a, 33000));
    assert(!wamr_realloc(Alloc_For_Runtime, false, &a, large, 80000));
    wamr_free(Alloc_For_Runtime, &a, second);
    assert(wamr_realloc(Alloc_For_Runtime, false, &a, large, 80000) == large);
    assert(((unsigned char *)large)[67339] == 42);
    assert(!wamr_realloc(Alloc_For_Runtime, false, &a, large, FX_METADATA_BYTES + 1));
    assert(a.metadata_blocks[0].size == 80000);
    wamr_free(Alloc_For_Runtime, &a, large);
    assert(!a.metadata_blocks[0].size);
    large = wamr_malloc(Alloc_For_Runtime, &a, 67340);
    second = wamr_malloc(Alloc_For_Runtime, &a, 40000);
    assert(large && second);
    wamr_free(Alloc_For_Runtime, &a, large);
    large = wamr_malloc(Alloc_For_Runtime, &a, 60000);
    assert(large == a.metadata);
    wamr_free(Alloc_For_Runtime, &a, second);
    wamr_free(Alloc_For_Runtime, &a, large);
    assert(!wamr_malloc(Alloc_For_Runtime, &a, FX_METADATA_BYTES + 1));
    large = wamr_realloc(Alloc_For_Runtime, false, &a, NULL, FX_METADATA_BYTES);
    assert(large == a.metadata);
    assert(!wamr_realloc(Alloc_For_Runtime, false, &a, large, 0));
    assert(!a.metadata_blocks[0].size);
    small = wamr_malloc(Alloc_For_Runtime, &a, 32768);
    assert(small && small != a.metadata);
    memset(small, 17, 32768);
    assert(!wamr_realloc(Alloc_For_Runtime, false, &a, small, 40000));
    assert(((unsigned char *)small)[32767] == 17);
    wamr_free(Alloc_For_Runtime, &a, small);
    assert(wamr_malloc(Alloc_For_LinearMemory, &a, 32768) == a.linear_memory);
    assert(!wamr_malloc(Alloc_For_LinearMemory, &a, 1));
    assert(wamr_realloc(Alloc_For_LinearMemory, false, &a, a.linear_memory, 65536) == a.linear_memory);
    assert(!wamr_realloc(Alloc_For_LinearMemory, false, &a, a.linear_memory, 65537));
    assert(a.used == 65536);
    wamr_free(Alloc_For_LinearMemory, &a, a.linear_memory);
    assert(!a.in_use && !a.used);
    free(a.metadata);
    free(a.linear_memory);
    return 0;
}
'''


class MemoryTests(unittest.TestCase):
    def test_bounds_and_reuse(self):
        with tempfile.TemporaryDirectory(prefix="fx-memory-") as directory:
            source = Path(directory) / "memory.c"
            source.write_text(HARNESS)
            binary = Path(directory) / "memory"
            subprocess.run(["cc", "-std=c11", "-Os", "-ffunction-sections", "-fdata-sections",
                            "-I" + str(ROOT),
                            "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/iwasm/include"),
                            "-I" + str(ROOT / "third_party/wasm-micro-runtime/core/shared/platform/include"),
                            "-DWASM_MEM_ALLOC_WITH_USAGE=1", "-DWASM_MEM_ALLOC_WITH_USER_DATA=1",
                            "-DFX_WAMR_FLASH_MODULE_ADDRESS=0x3cd80000UL",
                            "-DFX_WAMR_FLASH_PARTITION_BYTES=0x200000U",
                            "-DFX_WAMR_LINEAR_RESERVE_BYTES=65536U", "-DFX_WAMR_NO_AOT=1",
                            str(source), "-Wl,--gc-sections", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
