// SPDX-License-Identifier: MIT
// Adapter only. The included allocator is Doug Lea's MIT-0 implementation.
#include "embedded_dlmalloc_config.h"
#undef NO_MALLINFO
#define NO_MALLINFO 0
#define USE_DL_PREFIX 1
#include "embedded_dlmalloc.c"

void *fx_malloc(size_t bytes) { return dlmalloc(bytes); }
void fx_free(void *pointer) { dlfree(pointer); }
void *fx_realloc(void *pointer, size_t bytes) { return dlrealloc(pointer, bytes); }
size_t fx_usable_size(void *pointer) { return dlmalloc_usable_size(pointer); }
size_t fx_heap_used(void) { return dlmallinfo().uordblks; }
size_t fx_heap_footprint(void) { return dlmalloc_footprint(); }
