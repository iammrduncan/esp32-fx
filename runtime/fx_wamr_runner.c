// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
/* A small, credential-safe WAMR launcher for the headless fx WASI module. */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wasm_export.h"
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
#include "fx_memory.h"
#endif

#ifndef DEFAULT_FX_MODULE
#define DEFAULT_FX_MODULE "/usr/share/fx/fx-core.wasm"
#endif
/* The interpreter frame stack plus WAMRExecEnv fits beside the function table
 * in the board's fixed runtime arena. This is not the guest's linear-memory
 * stack. Keep this budget qualified by the real fx protocol/coding tests. */
#define WAMR_STACK_BYTES (31U * 1024U)
#define MAX_ENV_VALUE_BYTES 8192U
#define WAMR_MODE_DEFAULT ((RunningMode)0)

#ifndef FX_WAMR_NATIVE_STREAM_RESERVE_BYTES
#define FX_WAMR_NATIVE_STREAM_RESERVE_BYTES 0U
#endif

int init_native_lib(void);
void deinit_native_lib(void);
uint32_t get_native_lib(char **module_name, NativeSymbol **symbols);
void fx_wamr_set_stream_buffer(uint8_t *bytes, size_t length);
int fx_wamr_prepare_low_memory_guards(void);
void fx_wamr_release_low_memory_guards(void);

#ifdef FX_WAMR_MEMORY_SUMMARY
/* Internal WAMR diagnostic entry points, linked only by the profiling build. */
void wasm_runtime_dump_module_mem_consumption(const void *module);
void wasm_runtime_dump_module_inst_mem_consumption(const void *instance);
#endif

struct module_bytes {
    uint8_t *data;
    uint32_t size;
    bool mapped;
    bool owned;
    bool xip;
    bool raw;
};

#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
static int reclaim_file_cache(void)
{
    int fd;
    ssize_t written;

    /* SD logs and rootfs reads leave clean pages scattered across PSRAM.
     * NOMMU cannot compact them. Flush dirty data, then reclaim disposable
     * filesystem caches before reserving the launch/test allocations. This
     * is board-host policy; no files or application memory are discarded. */
    sync();
    fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    written = write(fd, "3\n", 2);
    close(fd);
    if (written != 2)
        return -1;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-wamr: reclaimed filesystem caches before launch\n");
    return 0;
}
#endif

#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
#if WASM_MEM_ALLOC_WITH_USAGE != 1 || WASM_MEM_ALLOC_WITH_USER_DATA != 1
#error "the linear reserve requires WAMR allocation usage and user data"
#endif

struct wamr_allocator {
    uint8_t *linear_memory;
    size_t capacity;
    size_t used;
    bool in_use;
    bool borrowed;
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    uint8_t *metadata;
    struct { size_t offset, size; } metadata_blocks[2];
#endif
};

static int init_wamr_allocator(struct wamr_allocator *allocator)
{
    const char *borrowed_address = getenv("FX_WAMR_LINEAR_MEMORY_ADDRESS");

#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    if (fx_verify_reserved_psram() < 0)
        return -1;
    allocator->metadata = (uint8_t *)(uintptr_t)FX_METADATA_ADDRESS;
#endif

    allocator->capacity = FX_WAMR_LINEAR_RESERVE_BYTES;
    if ((allocator->capacity % 65536U) != 0) {
        fprintf(stderr, "fx-wamr: guest linear-memory arena is not page aligned\n");
        return -1;
    }
#ifdef FX_WAMR_BORROWED_LINEAR_MIN
#ifndef FX_WAMR_BORROWED_LINEAR_MAX
#error "FX_WAMR_BORROWED_LINEAR_MAX is required with the minimum address"
#endif
    if (borrowed_address && *borrowed_address) {
        char *end = NULL;
        unsigned long address;

        errno = 0;
        address = strtoul(borrowed_address, &end, 0);
        if (errno || !end || *end || (address & 7U) != 0
            || address < FX_WAMR_BORROWED_LINEAR_MIN
            || address >= FX_WAMR_BORROWED_LINEAR_MAX
            || allocator->capacity
                   > (size_t)(FX_WAMR_BORROWED_LINEAR_MAX - address)) {
            fprintf(stderr, "fx-wamr: invalid borrowed linear-memory address\n");
            return -1;
        }
        allocator->linear_memory = (uint8_t *)(uintptr_t)address;
        allocator->borrowed = true;
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: using borrowed %lu-byte guest linear-memory arena: %p\n",
                    (unsigned long)allocator->capacity,
                    (void *)allocator->linear_memory);
        return 0;
    }
#else
    (void)borrowed_address;
#endif
    allocator->linear_memory =
        mmap(NULL, allocator->capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocator->linear_memory == MAP_FAILED) {
        allocator->linear_memory = NULL;
        fprintf(stderr,
                "fx-wamr: cannot reserve %lu bytes for guest linear memory: %s\n",
                (unsigned long)allocator->capacity, strerror(errno));
        return -1;
    }
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: reserved %lu-byte guest linear-memory arena: %p\n",
                (unsigned long)allocator->capacity,
                (void *)allocator->linear_memory);
    return 0;
}

static void destroy_wamr_allocator(struct wamr_allocator *allocator)
{
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    if (allocator->metadata)
        memset(allocator->metadata, 0, FX_METADATA_BYTES);
#endif
    if (allocator->linear_memory && !allocator->borrowed)
        munmap(allocator->linear_memory, allocator->capacity);
    memset(allocator, 0, sizeof(*allocator));
}

#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
/* Two simultaneous large allocations: function instances and execution
 * environment. Small objects still use libc. First-fit also handles reuse
 * in either free order, without ever returning this arena to Linux. */
static void *metadata_malloc(struct wamr_allocator *a, size_t size)
{
    size_t offset = 0;
    unsigned int slot;
    if (!size || size > FX_METADATA_BYTES)
        return NULL;
    for (slot = 0; slot < 2 && a->metadata_blocks[slot].size; slot++) {}
    if (slot == 2)
        return NULL;
    for (;;) {
        unsigned int i;
        if (size > FX_METADATA_BYTES - offset)
            return NULL;
        for (i = 0; i < 2; i++) {
            size_t start = a->metadata_blocks[i].offset;
            size_t end = start + a->metadata_blocks[i].size;
            if (a->metadata_blocks[i].size && offset < end && offset + size > start) {
                offset = (end + 7U) & ~(size_t)7U;
                break;
            }
        }
        if (i == 2)
            break;
    }
    a->metadata_blocks[slot].offset = offset;
    a->metadata_blocks[slot].size = size;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-wamr: reserved runtime allocation: %lu bytes\n", (unsigned long)size);
    return a->metadata + offset;
}

static int metadata_slot(struct wamr_allocator *a, void *pointer)
{
    for (int i = 0; i < 2; i++)
        if (a->metadata_blocks[i].size && pointer == a->metadata + a->metadata_blocks[i].offset)
            return i;
    return -1;
}
#endif

static void *wamr_malloc(mem_alloc_usage_t usage, void *opaque,
                         unsigned int size)
{
    struct wamr_allocator *allocator = opaque;

    if (usage != Alloc_For_LinearMemory) {
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
        if (size > 32768U)
            return metadata_malloc(allocator, size);
#endif
        if (getenv("FX_WAMR_DIAGNOSTICS") && size >= 16384U)
            fprintf(stderr, "fx-wamr: runtime heap allocation: %u bytes\n", size);
        return malloc(size);
    }
    if (allocator->in_use || size > allocator->capacity)
        return NULL;
    memset(allocator->linear_memory, 0, size);
    allocator->used = size;
    allocator->in_use = true;
    return allocator->linear_memory;
}

static void *wamr_realloc(mem_alloc_usage_t usage, bool full_size_mapped,
                          void *opaque, void *pointer, unsigned int size)
{
    struct wamr_allocator *allocator = opaque;

    (void)full_size_mapped;
    if (usage != Alloc_For_LinearMemory) {
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
        int slot = metadata_slot(allocator, pointer);
        if (slot >= 0) {
            size_t offset = allocator->metadata_blocks[slot].offset;
            if (size > FX_METADATA_BYTES - offset)
                return NULL;
            for (int i = 0; i < 2; i++) {
                size_t other = allocator->metadata_blocks[i].offset;
                if (i != slot && allocator->metadata_blocks[i].size
                    && offset < other + allocator->metadata_blocks[i].size
                    && offset + size > other)
                    return NULL;
            }
            allocator->metadata_blocks[slot].size = size;
            return size ? pointer : NULL;
        }
        if (!pointer)
            return wamr_malloc(usage, opaque, size);
        /* Other runtime allocations must remain small; do not fall back to
         * a fragmentation-sensitive large realloc if a module exceeds this
         * board profile's metadata budget. */
        if (size > 32768U)
            return NULL;
#endif
        return realloc(pointer, size);
    }
    if (pointer != allocator->linear_memory || size > allocator->capacity)
        return NULL;
    if (size > allocator->used)
        memset(allocator->linear_memory + allocator->used, 0,
               size - allocator->used);
    allocator->used = size;
    return allocator->linear_memory;
}

static void wamr_free(mem_alloc_usage_t usage, void *opaque, void *pointer)
{
    struct wamr_allocator *allocator = opaque;

    (void)usage;
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    int slot = metadata_slot(allocator, pointer);
    if (slot >= 0) {
        allocator->metadata_blocks[slot].size = 0;
        return;
    }
#endif
    if (pointer == allocator->linear_memory) {
        allocator->used = 0;
        allocator->in_use = false;
    }
    else {
        free(pointer);
    }
}
#endif

static void dump_guest_memory(const char *phase, wasm_module_inst_t instance)
{
    wasm_memory_inst_t memory;
    const uint8_t *bytes;
    uint64_t pages;
    uint64_t bytes_per_page;
    uint64_t length;
    uint32_t checksum = 2166136261U;
    uint64_t index;

    if (!getenv("FX_WAMR_DIAGNOSTICS"))
        return;
    memory = wasm_runtime_get_memory(instance, 0);
    if (!memory) {
        fprintf(stderr, "fx-wamr: guest memory %s: unavailable\n", phase);
        return;
    }
    pages = wasm_memory_get_cur_page_count(memory);
    bytes_per_page = wasm_memory_get_bytes_per_page(memory);
    length = pages * bytes_per_page;
    fprintf(stderr,
            "fx-wamr: guest memory %s: %llu pages x %llu bytes = %llu bytes\n",
            phase, (unsigned long long)pages,
            (unsigned long long)bytes_per_page,
            (unsigned long long)length);
    if (strcmp(phase, "after instantiate") != 0 || length > UINT32_MAX)
        return;
    bytes = wasm_runtime_addr_app_to_native(instance, 0);
    if (!bytes)
        return;
    for (index = 0; index < length; index++) {
        checksum ^= bytes[index];
        checksum *= 16777619U;
    }
    fprintf(stderr,
            "fx-wamr: guest memory after instantiate FNV-1a32: %08x\n",
            checksum);
}

#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
#ifndef FX_WAMR_FLASH_PARTITION_BYTES
#error "FX_WAMR_FLASH_PARTITION_BYTES is required with flash module mapping"
#endif
#define FX_FLASH_HEADER_MAGIC 0x4d575846U /* "FXWM", little-endian */
#define FX_FLASH_HEADER_VERSION 1U
#define FX_FLASH_HEADER_BYTES 64U
#define WASM_BINARY_MAGIC 0x6d736100U /* "\\0asm", little-endian */

static int load_builtin_flash_module(const char *path,
                                     struct module_bytes *module)
{
    volatile const uint32_t *header;
    uint32_t header_bytes;
    uint32_t wasm_bytes;
    volatile const uint32_t *wasm_magic;

    if (strcmp(path, DEFAULT_FX_MODULE) != 0)
        return 1;
    header = (volatile const uint32_t *)(uintptr_t)
        FX_WAMR_FLASH_MODULE_ADDRESS;
    header_bytes = header[2];
    wasm_bytes = header[3];
    if (header[0] != FX_FLASH_HEADER_MAGIC
        || header[1] != FX_FLASH_HEADER_VERSION
        || header_bytes != FX_FLASH_HEADER_BYTES
        || wasm_bytes < 8U
        || wasm_bytes > FX_WAMR_FLASH_PARTITION_BYTES - header_bytes) {
        fprintf(stderr, "fx-wamr: invalid fxwasm flash header at %p\n",
                (const void *)header);
        return -1;
    }
    wasm_magic = (volatile const uint32_t *)
        ((uintptr_t)header + header_bytes);
    if (*wasm_magic != WASM_BINARY_MAGIC) {
        fprintf(stderr, "fx-wamr: invalid Wasm magic in fxwasm partition\n");
        return -1;
    }
    module->data = (uint8_t *)(uintptr_t)wasm_magic;
    module->size = wasm_bytes;
    module->xip = true;
    module->raw = true;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: using %u-byte module input from data-mapped flash: %p\n",
                wasm_bytes, (void *)module->data);
    return 0;
}
#endif

static bool is_aot_header(const uint8_t header[4])
{
    return header[0] == 0 && header[1] == 'a' && header[2] == 'o'
           && header[3] == 't';
}

static int read_all(int fd, uint8_t *data, size_t size)
{
    while (size > 0) {
        ssize_t amount = read(fd, data, size);

        if (amount < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        data += (size_t)amount;
        size -= (size_t)amount;
    }
    return 0;
}

static int load_module_bytes(const char *path, struct module_bytes *module)
{
    struct stat metadata;
    uint8_t header[4];
    void *mapping;
    int fd;

    memset(module, 0, sizeof(*module));
#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    {
        int flash_result = load_builtin_flash_module(path, module);

        if (flash_result <= 0)
            return flash_result;
    }
#endif
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "fx-wamr: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &metadata) < 0 || metadata.st_size <= 0
        || (uint64_t)metadata.st_size > UINT32_MAX) {
        fprintf(stderr, "fx-wamr: invalid module file %s\n", path);
        close(fd);
        errno = EFBIG;
        return -1;
    }

    if (read_all(fd, header, sizeof(header)) < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "fx-wamr: cannot read %s: %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }

    /* Only probe an AOT file through an executable mapping.  On ESP32-S3,
     * cramfs XIP mappings live on the instruction flash bus, where byte loads
     * used by a Wasm parser are not valid.  Raw Wasm is therefore read into
     * RAM; a translating runtime may release it, while the classic
     * interpreter retains it until shutdown. */
    if (is_aot_header(header)) {
#ifndef FX_WAMR_NO_AOT
        mapping = mmap(NULL, (size_t)metadata.st_size, PROT_READ | PROT_EXEC,
                       MAP_PRIVATE, fd, 0);
        if (mapping != MAP_FAILED
            && wasm_runtime_is_xip_file(mapping, (uint32_t)metadata.st_size)) {
            module->data = mapping;
            module->size = (uint32_t)metadata.st_size;
            module->mapped = true;
            module->xip = true;
            if (getenv("FX_WAMR_DIAGNOSTICS"))
                fprintf(stderr,
                        "fx-wamr: using %lu-byte AOT/XIP mapping: %s\n",
                        (unsigned long)metadata.st_size, path);
            close(fd);
            return 0;
        }
        if (mapping != MAP_FAILED)
            munmap(mapping, (size_t)metadata.st_size);
#endif
    }

    /* Exercise the same immutable-buffer loader path on a development host
     * that the ESP32 uses for its data-mapped flash partition. */
    if (getenv("FX_WAMR_READONLY_MODULE")) {
        mapping = mmap(NULL, (size_t)metadata.st_size, PROT_READ,
                       MAP_PRIVATE, fd, 0);
        if (mapping == MAP_FAILED) {
            fprintf(stderr, "fx-wamr: cannot map %s read-only: %s\n", path,
                    strerror(errno));
            close(fd);
            return -1;
        }
        module->data = mapping;
        module->size = (uint32_t)metadata.st_size;
        module->mapped = true;
        module->xip = true;
        module->raw = true;
        close(fd);
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: using %u-byte read-only module mapping: %s\n",
                    module->size, path);
        return 0;
    }

    module->data = malloc((size_t)metadata.st_size);
    if (!module->data) {
        fprintf(stderr, "fx-wamr: cannot allocate %lu-byte module buffer\n",
                (unsigned long)metadata.st_size);
        close(fd);
        return -1;
    }
    module->owned = true;
    module->size = (uint32_t)metadata.st_size;
    if (read_all(fd, module->data, module->size) < 0) {
        fprintf(stderr, "fx-wamr: cannot read %s: %s\n", path, strerror(errno));
        free(module->data);
        memset(module, 0, sizeof(*module));
        close(fd);
        return -1;
    }
    close(fd);
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-wamr: loaded %u-byte module input into RAM: %s\n",
                module->size, path);
    return 0;
}

static void unload_module_bytes(struct module_bytes *module)
{
    if (module->mapped)
        munmap(module->data, module->size);
    else if (module->owned)
        free(module->data);
    memset(module, 0, sizeof(*module));
}

static char *make_wasi_env(const char *name, bool required)
{
    const char *value = getenv(name);
    size_t name_length = strlen(name);
    size_t value_length;
    char *entry;

    if (!value || !*value) {
        if (required)
            fprintf(stderr, "fx-wamr: %s is required\n", name);
        return NULL;
    }
    value_length = strlen(value);
    if (value_length > MAX_ENV_VALUE_BYTES
        || name_length > SIZE_MAX - value_length - 2U) {
        fprintf(stderr, "fx-wamr: %s is too large\n", name);
        return NULL;
    }
    entry = malloc(name_length + value_length + 2U);
    if (!entry)
        return NULL;
    memcpy(entry, name, name_length);
    entry[name_length] = '=';
    memcpy(entry + name_length + 1U, value, value_length + 1U);
    return entry;
}

int main(int argc, char **argv)
{
    const char *wasm_path = DEFAULT_FX_MODULE;
    struct module_bytes module_bytes = { 0 };
    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t instance = NULL;
    NativeSymbol *native_symbols = NULL;
    char *native_module_name = NULL;
    uint32_t native_symbol_count;
    char error[256] = { 0 };
    LoadArgs load_args = { 0 };
    InstantiationArgs instantiate_args = { .default_stack_size = WAMR_STACK_BYTES };
    const char *wasi_env[6];
    char *owned_env[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    uint32_t wasi_env_count = 0;
    char *guest_argv[1];
    int result = EXIT_FAILURE;
    bool native_initialized = false;
    bool low_memory_guards_prepared = false;
    bool runtime_initialized = false;
#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
    struct wamr_allocator allocator = { 0 };
    bool allocator_initialized = false;
#endif
#if FX_WAMR_NATIVE_STREAM_RESERVE_BYTES > 0
    uint8_t *native_stream_buffer = NULL;
    bool native_stream_buffer_borrowed = false;
#endif

    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
        fprintf(argc > 2 ? stderr : stdout,
                "Usage: fx-wamr [fx-core.wasm|fx-core.aot]\n"
                "Reads AI_GATEWAY_API_KEY and FX_MODEL from its environment; "
                "secrets are not copied into argv.\n");
        return argc > 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (argc == 2)
        wasm_path = argv[1];

#ifdef FX_WAMR_FLASH_MODULE_ADDRESS
    if (reclaim_file_cache() < 0) {
        fprintf(stderr, "fx-wamr: could not reclaim filesystem caches\n");
        goto cleanup;
    }
#endif

#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
    /* no-MMU Linux needs one physically contiguous linear-memory mapping.
     * Reserve its maximum size before the loader and libc fragment PSRAM;
     * WAMR can then grow the guest memory in place. */
    if (init_wamr_allocator(&allocator) < 0)
        goto cleanup;
    allocator_initialized = true;
#endif

#if FX_WAMR_NATIVE_STREAM_RESERVE_BYTES > 0
    /* Keep this mapping separate from the 1-MiB guest arena. Linux NOMMU's
     * buddy allocator would round a combined 1-MiB-plus-64-KiB request up to
     * a scarce 2-MiB physically contiguous block. */
    {
        const char *borrowed = getenv("FX_WAMR_NATIVE_STREAM_ADDRESS");

        if (borrowed && *borrowed) {
#if defined(FX_WAMR_BORROWED_LINEAR_MIN) \
    && defined(FX_WAMR_BORROWED_LINEAR_MAX) \
    && defined(FX_WAMR_LINEAR_RESERVE_BYTES)
            char *end = NULL;
            unsigned long address;
            uintptr_t linear_start = (uintptr_t)allocator.linear_memory;
            uintptr_t linear_end = linear_start + allocator.capacity;
            uintptr_t stream_end;

            errno = 0;
            address = strtoul(borrowed, &end, 0);
            stream_end = (uintptr_t)address
                         + FX_WAMR_NATIVE_STREAM_RESERVE_BYTES;
            if (errno || !end || *end || (address & 7U) != 0
                || address < FX_WAMR_BORROWED_LINEAR_MIN
                || address >= FX_WAMR_BORROWED_LINEAR_MAX
                || stream_end < address
                || stream_end > FX_WAMR_BORROWED_LINEAR_MAX
                || ((uintptr_t)address < linear_end
                    && stream_end > linear_start)) {
                fprintf(stderr,
                        "fx-wamr: invalid borrowed native stream address\n");
                goto cleanup;
            }
            native_stream_buffer = (uint8_t *)(uintptr_t)address;
            native_stream_buffer_borrowed = true;
#else
            fprintf(stderr,
                    "fx-wamr: borrowed native stream buffer is unsupported\n");
            goto cleanup;
#endif
        }
        else {
            native_stream_buffer =
                mmap(NULL, FX_WAMR_NATIVE_STREAM_RESERVE_BYTES,
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
            if (native_stream_buffer == MAP_FAILED) {
                native_stream_buffer = NULL;
                fprintf(stderr,
                        "fx-wamr: cannot reserve %u-byte native stream buffer: %s\n",
                        (unsigned int)FX_WAMR_NATIVE_STREAM_RESERVE_BYTES,
                        strerror(errno));
                goto cleanup;
            }
        }
    }
#endif

    /* Reserve the two kernel-managed blocks required by the shell test
     * launcher before loaders fragment NOMMU memory. */
    if (fx_wamr_prepare_low_memory_guards() < 0)
        goto cleanup;
    low_memory_guards_prepared = true;

    owned_env[wasi_env_count] = make_wasi_env("AI_GATEWAY_API_KEY", true);
    if (!owned_env[wasi_env_count])
        goto cleanup;
    wasi_env[wasi_env_count] = owned_env[wasi_env_count];
    wasi_env_count++;
    owned_env[wasi_env_count] = make_wasi_env("FX_MODEL", true);
    if (!owned_env[wasi_env_count])
        goto cleanup;
    wasi_env[wasi_env_count] = owned_env[wasi_env_count];
    wasi_env_count++;
    owned_env[wasi_env_count] = make_wasi_env("FX_LOG", false);
    if (owned_env[wasi_env_count]) {
        wasi_env[wasi_env_count] = owned_env[wasi_env_count];
        wasi_env_count++;
    }
    owned_env[wasi_env_count] = make_wasi_env("FX_TRACE_STDERR", false);
    if (owned_env[wasi_env_count]) {
        wasi_env[wasi_env_count] = owned_env[wasi_env_count];
        wasi_env_count++;
    }
    owned_env[wasi_env_count] = make_wasi_env("FX_TRACE_SCOPES", false);
    if (owned_env[wasi_env_count]) {
        wasi_env[wasi_env_count] = owned_env[wasi_env_count];
        wasi_env_count++;
    }
    owned_env[wasi_env_count] = make_wasi_env("FX_TRACE", false);
    if (owned_env[wasi_env_count]) {
        wasi_env[wasi_env_count] = owned_env[wasi_env_count];
        wasi_env_count++;
    }

    if (!getenv("FX_WAMR_LOCAL_MODEL_CATALOG")
        && setenv("FX_WAMR_LOCAL_MODEL_CATALOG", "1", 0) < 0) {
        fprintf(stderr, "fx-wamr: cannot configure local catalog: %s\n",
                strerror(errno));
        goto cleanup;
    }
    if (load_module_bytes(wasm_path, &module_bytes) < 0)
        goto cleanup;
    if (init_native_lib() != 0) {
        fprintf(stderr, "fx-wamr: native fx host initialization failed\n");
        goto cleanup;
    }
    native_initialized = true;
#if FX_WAMR_NATIVE_STREAM_RESERVE_BYTES > 0
    fx_wamr_set_stream_buffer(native_stream_buffer,
                              FX_WAMR_NATIVE_STREAM_RESERVE_BYTES);
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: using %u-byte borrowed native stream buffer: %p\n",
                (unsigned int)FX_WAMR_NATIVE_STREAM_RESERVE_BYTES,
                (void *)native_stream_buffer);
#endif
    native_symbol_count = get_native_lib(&native_module_name, &native_symbols);
    if (!native_module_name || !native_symbols || native_symbol_count == 0) {
        fprintf(stderr, "fx-wamr: native fx symbol table is empty\n");
        goto cleanup;
    }

    memset(&init_args, 0, sizeof(init_args));
#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = (void *)wamr_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = (void *)wamr_realloc;
    init_args.mem_alloc_option.allocator.free_func = (void *)wamr_free;
    init_args.mem_alloc_option.allocator.user_data = &allocator;
#else
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
#endif
    /* Mode_Default also admits AOT-only target builds; interpreted host
     * builds still choose their configured interpreter automatically. */
    init_args.running_mode = WAMR_MODE_DEFAULT;
    init_args.native_module_name = native_module_name;
    init_args.native_symbols = native_symbols;
    init_args.n_native_symbols = native_symbol_count;
    if (!wasm_runtime_full_init(&init_args)) {
        fprintf(stderr, "fx-wamr: runtime initialization failed\n");
        goto cleanup;
    }
    runtime_initialized = true;

    /* Let the configured runtime free the source image when it has cloned
     * everything needed for execution.  The low-memory classic interpreter
     * deliberately retains raw Wasm instead: source bytes plus its small
     * metadata footprint use less RAM than fast-interpreter translation for
     * this large module.  XIP AOT code, on an ABI that supports it, remains
     * mapped. */
    load_args.wasm_binary_freeable = !module_bytes.xip;
    /* Data-mapped ESP32 flash is byte-readable but cannot be rewritten. WAMR
     * normally NUL-terminates retained import/export strings in the source
     * buffer, so ask it to copy only those small strings while continuing to
     * execute bytecode and initialize data directly from flash. */
    load_args.wasm_binary_readonly = module_bytes.xip;
    module = wasm_runtime_load_ex(module_bytes.data, module_bytes.size,
                                  &load_args, error, sizeof(error));
    if (!module) {
        fprintf(stderr, "fx-wamr: cannot load module: %s\n", error);
        goto cleanup;
    }
    if (module_bytes.raw) {
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: retaining %u-byte flash module input for interpreter\n",
                    module_bytes.size);
    }
    else if (!module_bytes.xip) {
        if (wasm_runtime_is_underlying_binary_freeable(module)) {
            uint32_t released = module_bytes.size;

            unload_module_bytes(&module_bytes);
            if (getenv("FX_WAMR_DIAGNOSTICS"))
                fprintf(stderr,
                        "fx-wamr: released %u-byte module input after load\n",
                        released);
        }
        else if (getenv("FX_WAMR_DIAGNOSTICS")) {
            fprintf(stderr,
                    "fx-wamr: retaining %u-byte module input for interpreter\n",
                    module_bytes.size);
        }
    }
    guest_argv[0] = (char *)wasm_path;
    wasm_runtime_set_wasi_args(module, NULL, 0, NULL, 0, wasi_env,
                               wasi_env_count, guest_argv, 1);
    {
        const char *maximum = getenv("FX_WAMR_MAX_PAGES");
#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
        instantiate_args.max_memory_pages = allocator.capacity / 65536U;
#endif
        if (maximum && *maximum) {
            char *end;
            unsigned long pages = strtoul(maximum, &end, 10);
            if (*end || pages < 12 || pages > 256
                || (instantiate_args.max_memory_pages && pages > instantiate_args.max_memory_pages)) {
                fprintf(stderr, "fx-wamr: invalid guest page limit\n");
                goto cleanup;
            }
            instantiate_args.max_memory_pages = (uint32_t)pages;
        }
    }
    instance = wasm_runtime_instantiate_ex(module, &instantiate_args, error, sizeof(error));
    if (!instance) {
        fprintf(stderr, "fx-wamr: cannot instantiate module: %s\n", error);
        goto cleanup;
    }
    dump_guest_memory("after instantiate", instance);
    /* Create the singleton explicitly so startup evidence distinguishes its
     * allocation from any later provider or tool operation. */
    if (!wasm_runtime_get_exec_env_singleton(instance)) {
        fprintf(stderr, "fx-wamr: cannot create singleton execution environment\n");
        goto cleanup;
    }
#ifdef FX_WAMR_MEMORY_SUMMARY
    if (getenv("FX_WAMR_MEMORY_PROFILE")) {
        wasm_runtime_dump_module_mem_consumption(module);
        wasm_runtime_dump_module_inst_mem_consumption(instance);
        result = EXIT_SUCCESS;
        goto cleanup;
    }
#endif
    if (!wasm_application_execute_main(instance, 1, guest_argv)) {
        const char *exception = wasm_runtime_get_exception(instance);

        dump_guest_memory("after failed execution", instance);
        fprintf(stderr, "fx-wamr: execution failed%s%s\n",
                exception ? ": " : "", exception ? exception : "");
        goto cleanup;
    }
    dump_guest_memory("after execution", instance);
    result = (int)wasm_runtime_get_wasi_exit_code(instance);

cleanup:
    if (instance)
        wasm_runtime_deinstantiate(instance);
    if (module)
        wasm_runtime_unload(module);
    if (runtime_initialized)
        wasm_runtime_destroy();
    if (native_initialized)
        deinit_native_lib();
    if (low_memory_guards_prepared)
        fx_wamr_release_low_memory_guards();
    unload_module_bytes(&module_bytes);
    for (size_t index = 0; index < sizeof(owned_env) / sizeof(owned_env[0]);
         index++) {
        if (owned_env[index]) {
            memset(owned_env[index], 0, strlen(owned_env[index]));
            free(owned_env[index]);
        }
    }
#ifdef FX_WAMR_LINEAR_RESERVE_BYTES
    if (allocator_initialized)
        destroy_wamr_allocator(&allocator);
#endif
#if FX_WAMR_NATIVE_STREAM_RESERVE_BYTES > 0
    if (native_stream_buffer) {
        memset(native_stream_buffer, 0, FX_WAMR_NATIVE_STREAM_RESERVE_BYTES);
        if (!native_stream_buffer_borrowed)
            munmap(native_stream_buffer,
                   FX_WAMR_NATIVE_STREAM_RESERVE_BYTES);
    }
#endif
    return result;
}
