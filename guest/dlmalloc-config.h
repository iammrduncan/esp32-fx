// SPDX-License-Identifier: MIT
// Small single-threaded WASI profile for Doug Lea's unmodified dlmalloc 2.8.6.
#define HAVE_MORECORE 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 0
#define USE_DEV_RANDOM 0
#define DEFAULT_GRANULARITY 65536
#define malloc_getpagesize 65536
#define MALLOC_ALIGNMENT 16
#define NO_MALLOC_STATS 1
#define NO_MALLINFO 1
#define LACKS_SYS_MMAN_H 1
#define LACKS_SYS_PARAM_H 1
#define LACKS_TIME_H 1
