/*
 * Native WAMR imports for the fx WebAssembly surfaces.
 *
 * fx's browser SDK uses JSPI because its JavaScript host functions are
 * asynchronous.  A native WAMR host can implement the same ABI with bounded,
 * blocking calls.  The e-ink target deliberately buffers each HTTP response:
 * a partial-refresh display cannot usefully repaint at token frequency, and
 * this keeps the bridge single-threaded and small.
 */

#define _GNU_SOURCE

#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#include "wasm_export.h"
#include "fx_example_tools.h"

#ifdef FX_WAMR_MBEDTLS_STREAM_CA
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

struct request_trust {
    const char *path;
    mbedtls_x509_crt roots;
};

/* libcurl's mbedTLS backend normally copies the entire PEM file before
 * parsing it. Stream the same trust roots through a bounded scratch buffer
 * instead: no 45-KiB temporary allocation on each provider turn. The chain
 * remains alive until curl_easy_cleanup has destroyed the TLS connection. */
static CURLcode
load_trust_roots(CURL *curl, void *ssl_context, void *opaque)
{
    struct request_trust *trust = opaque;
    FILE *file = NULL;
    char *pem = NULL;
    char line[256];
    size_t used = 0;
    unsigned int count = 0;
    CURLcode result = CURLE_SSL_CACERT_BADFILE;

    (void)curl;
    file = fopen(trust->path, "r");
    if (!file)
        goto done;
    pem = malloc(8192);
    if (!pem) {
        result = CURLE_OUT_OF_MEMORY;
        goto done;
    }
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        if (!used) {
            if (strcmp(line, "-----BEGIN CERTIFICATE-----\n") != 0
                && strcmp(line, "-----BEGIN CERTIFICATE-----\r\n") != 0)
                continue;
        }
        if (length >= 8192 - used)
            goto done;
        memcpy(pem + used, line, length + 1);
        used += length;
        if (strcmp(line, "-----END CERTIFICATE-----\n") == 0
            || strcmp(line, "-----END CERTIFICATE-----\r\n") == 0
            || strcmp(line, "-----END CERTIFICATE-----") == 0) {
            if (mbedtls_x509_crt_parse(&trust->roots,
                                      (unsigned char *)pem, used + 1) != 0)
                goto done;
            count++;
            used = 0;
        }
    }
    if (ferror(file) || used || !count)
        goto done;
    mbedtls_ssl_conf_ca_chain(ssl_context, &trust->roots, NULL);
    result = CURLE_OK;
done:
    free(pem);
    if (file)
        fclose(file);
    return result;
}
#endif

#ifndef FX_WAMR_MAX_STREAM_BYTES
#define FX_WAMR_MAX_STREAM_BYTES (2U * 1024U * 1024U)
#endif

#define FX_WAMR_MAX_STREAMS 4
#define FX_WAMR_MAX_METHOD_BYTES 16U
#define FX_WAMR_MAX_URL_BYTES 8192U
#define FX_WAMR_MAX_HEADERS_BYTES (64U * 1024U)
#define FX_WAMR_MAX_FIXTURE_BYTES 8192U
/* libcurl defaults its upload buffer to 64 KiB.  On NOMMU Linux that
 * malloc becomes a roughly 68-KiB mapping and therefore consumes a
 * physically-contiguous order-5 (128-KiB) buddy block.  A second provider
 * turn can legitimately occur after the heap has fragmented enough that no
 * such block remains.  libcurl documents CURL_MAX_WRITE_SIZE (16 KiB in the
 * target build) as the supported upload-buffer minimum, which is ample at
 * ESP32-S3 Wi-Fi speeds and only requires an order-3 allocation here. */
#define FX_WAMR_CURL_IO_BYTES (16L * 1024L)

struct byte_sink {
    uint8_t *bytes;
    size_t len;
    size_t cap;
    int owned;
    int overflow;
};

struct stream_slot {
    int used;
    int owned;
    uint16_t status;
    uint8_t *bytes;
    size_t len;
    size_t cursor;
};

static struct stream_slot streams[FX_WAMR_MAX_STREAMS];
static uint8_t *borrowed_stream_buffer;
static size_t borrowed_stream_buffer_bytes;

#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    || defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
struct contiguity_guard {
    void *mapping;
    size_t length;
};

#ifdef FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES
static struct contiguity_guard shell_guard;
#endif
#ifdef FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES
static struct contiguity_guard test_guard;
#endif

static int
acquire_guard(struct contiguity_guard *guard, size_t length,
              const char *purpose)
{
    void *mapping;

    if (guard->mapping)
        return guard->length == length ? 0 : -1;
    mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: cannot acquire %lu-byte %s contiguity guard: %s\n",
                    (unsigned long)length, purpose, strerror(errno));
        return -1;
    }
    guard->mapping = mapping;
    guard->length = length;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: holding %lu-byte %s contiguity guard at %p\n",
                (unsigned long)length, purpose, mapping);
    return 0;
}

static int
release_guard(struct contiguity_guard *guard, const char *purpose)
{
    size_t length;

    if (!guard->mapping)
        return -1;
    length = guard->length;
    /* After the block is returned, the next operation must be the
     * vfork/exec that borrows it. Do not even emit a success
     * diagnostic here: stdio growth could fragment freshly freed pages. */
    if (munmap(guard->mapping, length) < 0) {
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: cannot lend %s contiguity guard: %s\n",
                    purpose, strerror(errno));
        return -1;
    }
    guard->mapping = NULL;
    return 0;
}
#endif

int
fx_wamr_prepare_low_memory_guards(void)
{
#ifdef FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES
    if (acquire_guard(&shell_guard, FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES,
                      "shell") < 0)
        return -1;
#endif
#ifdef FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES
    if (acquire_guard(&test_guard, FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES,
                      "test") < 0) {
#ifdef FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES
        release_guard(&shell_guard, "shell");
#endif
        return -1;
    }
#endif
    return 0;
}

void
fx_wamr_release_low_memory_guards(void)
{
#ifdef FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES
    if (test_guard.mapping)
        release_guard(&test_guard, "test");
    memset(&test_guard, 0, sizeof(test_guard));
#endif
#ifdef FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES
    if (shell_guard.mapping)
        release_guard(&shell_guard, "shell");
    memset(&shell_guard, 0, sizeof(shell_guard));
#endif
}

#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    && defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
int
fx_wamr_lend_test_contiguity_guard(void)
{
    /* BusyBox sh needs a 64 KiB NOMMU mapping while the small test launcher
     * still occupies roughly 32 KiB.  Release both pre-reserved blocks, with
     * no intervening allocation, immediately before vfork/exec. */
    if (release_guard(&shell_guard, "shell") < 0)
        return -1;
    if (release_guard(&test_guard, "test") < 0) {
        acquire_guard(&shell_guard, FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES,
                      "shell");
        return -1;
    }
    return 0;
}

int
fx_wamr_rearm_test_contiguity_guard(void)
{
    /* Take the larger block first so the helper's 32 KiB mapping cannot split
     * the 64 KiB buddy needed for the next shell test. These blocks remain
     * reserved throughout provider requests; TLS no longer borrows them. */
    if (acquire_guard(&shell_guard, FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES,
                      "shell") < 0)
        return -1;
    if (acquire_guard(&test_guard, FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES,
                      "test") < 0) {
        release_guard(&shell_guard, "shell");
        return -1;
    }
    return 0;
}
#endif

static void
trace_system_memory(const char *phase)
{
    struct sysinfo info;

    if (!getenv("FX_WAMR_DIAGNOSTICS") || sysinfo(&info) != 0)
        return;
    fprintf(stderr,
            "fx-wamr: %s system_free_kib=%llu buffer_kib=%llu\n",
            phase,
            (unsigned long long)info.freeram * info.mem_unit / 1024U,
            (unsigned long long)info.bufferram * info.mem_unit / 1024U);
}

/*
 * NOMMU targets may reserve this buffer before the runtime and dynamic loader
 * fragment physical memory.  Only one provider stream is consumed at a time,
 * so a single fixed buffer is sufficient; concurrent opens fail closed.
 */
void
fx_wamr_set_stream_buffer(uint8_t *bytes, size_t length)
{
    borrowed_stream_buffer = bytes;
    borrowed_stream_buffer_bytes = length;
}

static void trace_guest_pages(wasm_exec_env_t exec_env, const char *phase)
{
#ifndef FX_WAMR_SHARED_BRIDGE
    if (getenv("FX_WAMR_DIAGNOSTICS")) {
        wasm_module_inst_t instance = wasm_runtime_get_module_inst(exec_env);
        wasm_memory_inst_t memory = wasm_runtime_get_memory(instance, 0);
        if (memory)
            fprintf(stderr, "fx-wamr: %s guest_pages=%llu\n", phase,
                    (unsigned long long)wasm_memory_get_cur_page_count(memory));
        trace_system_memory(phase);
    }
#else
    (void)exec_env;
    (void)phase;
#endif
}

static size_t
write_body(void *contents, size_t size, size_t nmemb, void *opaque)
{
    struct byte_sink *sink = opaque;
    size_t count;
    size_t wanted;

    if (size != 0 && nmemb > SIZE_MAX / size) {
        sink->overflow = 1;
        return 0;
    }
    count = size * nmemb;
    if (count > sink->cap - sink->len) {
        sink->overflow = 1;
        return 0;
    }
    wanted = sink->len + count;
    if (sink->owned && wanted > 0) {
        size_t next = sink->cap < 4096 ? 4096 : sink->cap;
        uint8_t *grown;

        while (next < wanted) {
            if (next > FX_WAMR_MAX_STREAM_BYTES / 2U) {
                next = FX_WAMR_MAX_STREAM_BYTES;
                break;
            }
            next *= 2U;
        }
        if (next > FX_WAMR_MAX_STREAM_BYTES)
            next = FX_WAMR_MAX_STREAM_BYTES;
        grown = realloc(sink->bytes, next);
        if (!grown) {
            sink->overflow = 1;
            return 0;
        }
        sink->bytes = grown;
        sink->cap = next;
    }
    if (count)
        memcpy(sink->bytes + sink->len, contents, count);
    sink->len += count;
    return count;
}

static void
skip_ws(const uint8_t **cursor, const uint8_t *end)
{
    while (*cursor < end) {
        uint8_t ch = **cursor;
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
            break;
        (*cursor)++;
    }
}

static int
hex_digit(uint8_t ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int
append_utf8(char *out, size_t cap, size_t *len, uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        if (*len + 1 >= cap)
            return -1;
        out[(*len)++] = (char)codepoint;
    }
    else if (codepoint <= 0x7ff) {
        if (*len + 2 >= cap)
            return -1;
        out[(*len)++] = (char)(0xc0 | (codepoint >> 6));
        out[(*len)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    else {
        if (*len + 3 >= cap)
            return -1;
        out[(*len)++] = (char)(0xe0 | (codepoint >> 12));
        out[(*len)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[(*len)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    return 0;
}

static int
parse_json_string(const uint8_t **cursor, const uint8_t *end, char **value)
{
    const uint8_t *p = *cursor;
    size_t cap;
    size_t len = 0;
    char *out;

    if (p >= end || *p++ != '"')
        return -1;
    cap = (size_t)(end - p) + 1U;
    out = malloc(cap);
    if (!out)
        return -1;
    while (p < end) {
        uint8_t ch = *p++;
        if (ch == '"') {
            out[len] = '\0';
            *cursor = p;
            *value = out;
            return 0;
        }
        if (ch < 0x20)
            goto invalid;
        if (ch != '\\') {
            if (len + 1 >= cap)
                goto invalid;
            out[len++] = (char)ch;
            continue;
        }
        if (p >= end)
            goto invalid;
        ch = *p++;
        switch (ch) {
            case '"':
            case '\\':
            case '/':
                out[len++] = (char)ch;
                break;
            case 'b': out[len++] = '\b'; break;
            case 'f': out[len++] = '\f'; break;
            case 'n': out[len++] = '\n'; break;
            case 'r': out[len++] = '\r'; break;
            case 't': out[len++] = '\t'; break;
            case 'u': {
                uint32_t codepoint = 0;
                int digit;
                unsigned i;
                if ((size_t)(end - p) < 4U)
                    goto invalid;
                for (i = 0; i < 4; i++) {
                    digit = hex_digit(*p++);
                    if (digit < 0)
                        goto invalid;
                    codepoint = (codepoint << 4) | (uint32_t)digit;
                }
                if (codepoint >= 0xd800 && codepoint <= 0xdfff)
                    goto invalid;
                if (append_utf8(out, cap, &len, codepoint) != 0)
                    goto invalid;
                break;
            }
            default:
                goto invalid;
        }
    }

invalid:
    free(out);
    return -1;
}

static int
header_is_safe(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    while (*p) {
        if (*p == '\r' || *p == '\n' || *p == 0x7f || *p < 0x20)
            return 0;
        p++;
    }
    return 1;
}

static int
parse_header_object(const uint8_t **cursor, const uint8_t *end,
                    struct curl_slist **headers)
{
    char *name = NULL;
    char *value = NULL;
    int first = 1;

    if (*cursor >= end || *(*cursor)++ != '{')
        return -1;
    for (;;) {
        char *key = NULL;
        char *field = NULL;
        skip_ws(cursor, end);
        if (*cursor < end && **cursor == '}') {
            (*cursor)++;
            break;
        }
        if (!first) {
            if (*cursor >= end || *(*cursor)++ != ',')
                goto invalid;
            skip_ws(cursor, end);
        }
        first = 0;
        if (parse_json_string(cursor, end, &key) != 0)
            goto invalid;
        skip_ws(cursor, end);
        if (*cursor >= end || *(*cursor)++ != ':') {
            free(key);
            goto invalid;
        }
        skip_ws(cursor, end);
        if (parse_json_string(cursor, end, &field) != 0) {
            free(key);
            goto invalid;
        }
        if (strcmp(key, "name") == 0) {
            free(name);
            name = field;
            field = NULL;
        }
        else if (strcmp(key, "value") == 0) {
            free(value);
            value = field;
            field = NULL;
        }
        free(field);
        free(key);
        skip_ws(cursor, end);
    }
    if (!name || !value || !*name || !header_is_safe(name)
        || !header_is_safe(value) || strchr(name, ':'))
        goto invalid;
    {
        size_t name_len = strlen(name);
        size_t value_len = strlen(value);
        char *line;
        struct curl_slist *next;
        if (name_len > SIZE_MAX - value_len - 3U)
            goto invalid;
        line = malloc(name_len + value_len + 3U);
        if (!line)
            goto invalid;
        memcpy(line, name, name_len);
        line[name_len] = ':';
        line[name_len + 1U] = ' ';
        memcpy(line + name_len + 2U, value, value_len + 1U);
        next = curl_slist_append(*headers, line);
        free(line);
        if (!next)
            goto invalid;
        *headers = next;
    }
    free(name);
    free(value);
    return 0;

invalid:
    free(name);
    free(value);
    return -1;
}

static int
parse_headers(const uint8_t *json, uint32_t len, struct curl_slist **headers)
{
    const uint8_t *cursor = json;
    const uint8_t *end = json + len;
    int first = 1;

    if (len > FX_WAMR_MAX_HEADERS_BYTES)
        return -1;
    skip_ws(&cursor, end);
    if (cursor >= end || *cursor++ != '[')
        return -1;
    for (;;) {
        skip_ws(&cursor, end);
        if (cursor < end && *cursor == ']') {
            cursor++;
            break;
        }
        if (!first) {
            if (cursor >= end || *cursor++ != ',')
                return -1;
            skip_ws(&cursor, end);
        }
        first = 0;
        if (parse_header_object(&cursor, end, headers) != 0)
            return -1;
    }
    skip_ws(&cursor, end);
    return cursor == end ? 0 : -1;
}

static char *
copy_string(const uint8_t *bytes, uint32_t len, uint32_t limit)
{
    char *out;
    if (len == 0 || len > limit || memchr(bytes, '\0', len))
        return NULL;
    out = malloc((size_t)len + 1U);
    if (!out)
        return NULL;
    memcpy(out, bytes, len);
    out[len] = '\0';
    return out;
}

/* Test-only origin replacement.  Keeping this in the native bridge lets the
 * exact release Wasm artifact exercise deterministic local HTTP fixtures. */
static char *
rewrite_gateway_url(char *url)
{
    /* Keep byte-copied/comparison inputs in writable data.  The ESP32-S3
     * FDPIC image executes .rodata directly from instruction-mapped flash;
     * using those addresses as ordinary byte-data sources is not reliable. */
    static char origin[] = "https://ai-gateway.vercel.sh";
    const char *base = getenv("FX_WAMR_GATEWAY_BASE_URL");
    size_t base_len;
    size_t suffix_len;
    char *rewritten;

    if (!base || !*base || strncmp(url, origin, sizeof(origin) - 1U) != 0)
        return url;
    if (url[sizeof(origin) - 1U] != '\0'
        && url[sizeof(origin) - 1U] != '/')
        return url;
    if (strncmp(base, "http://127.0.0.1:", 17) != 0
        && strncmp(base, "http://localhost:", 17) != 0)
        return url;
    base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1U] == '/')
        base_len--;
    suffix_len = strlen(url + sizeof(origin) - 1U);
    if (base_len > SIZE_MAX - suffix_len - 1U)
        return url;
    rewritten = malloc(base_len + suffix_len + 1U);
    if (!rewritten)
        return url;
    memcpy(rewritten, base, base_len);
    memcpy(rewritten + base_len, url + sizeof(origin) - 1U, suffix_len + 1U);
    free(url);
    return rewritten;
}

static int
is_model_id_safe(const char *model)
{
    const unsigned char *p = (const unsigned char *)model;
    size_t length = 0;

    while (*p) {
        unsigned char ch = *p++;

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
              || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
              || ch == '.' || ch == '/' || ch == ':'))
            return 0;
        if (++length > 256U)
            return 0;
    }
    return length > 0;
}

/* The public model catalog is hundreds of KiB and changes independently of
 * the firmware. On the embedded profile, expose only the explicitly selected
 * model. fx still parses and owns the catalog through its normal provider. */
static int
serve_local_model_catalog(const char *method, const char *url,
                          struct byte_sink *sink, uint16_t *status)
{
    /* Intentionally non-const for the ESP32-S3 FDPIC/XIP reason documented in
     * rewrite_gateway_url().  These templates must reside in RAM-backed data. */
    static char suffix[] = "/coding-agent/v1/models";
    static char prefix[] =
        "{\"object\":\"list\",\"data\":[{\"id\":\"";
    static char tail[] =
        "\",\"type\":\"language\",\"tags\":[]}]}";
    const char *enabled = getenv("FX_WAMR_LOCAL_MODEL_CATALOG");
    const char *model = getenv("FX_MODEL");
    size_t url_len;
    size_t suffix_len = sizeof(suffix) - 1U;

    if (!enabled || strcmp(enabled, "1") != 0)
        return 0;
    url_len = strlen(url);
    if (strcmp(method, "GET") != 0 || url_len < suffix_len
        || strcmp(url + url_len - suffix_len, suffix) != 0)
        return 0;
    if (!is_model_id_safe(model))
        return -1;
    if (write_body((void *)prefix, 1, sizeof(prefix) - 1U, sink) == 0
        || write_body((void *)model, 1, strlen(model), sink) == 0
        || write_body((void *)tail, 1, sizeof(tail) - 1U, sink) == 0)
        return -1;
    *status = 200;
    return 1;
}

static int
sink_append(struct byte_sink *sink, const void *bytes, size_t len)
{
    if (len == 0)
        return 0;
    return write_body((void *)bytes, 1, len, sink) == len ? 0 : -1;
}

static int
sink_append_json_string(struct byte_sink *sink, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    const char quote = '"';

    if (sink_append(sink, &quote, 1) != 0)
        return -1;
    while (*cursor) {
        unsigned char ch = *cursor++;
        char escaped[7];
        size_t escaped_len;

        switch (ch) {
        case '"':
            escaped[0] = '\\';
            escaped[1] = '"';
            escaped_len = 2;
            break;
        case '\\':
            escaped[0] = '\\';
            escaped[1] = '\\';
            escaped_len = 2;
            break;
        case '\b':
            escaped[0] = '\\';
            escaped[1] = 'b';
            escaped_len = 2;
            break;
        case '\f':
            escaped[0] = '\\';
            escaped[1] = 'f';
            escaped_len = 2;
            break;
        case '\n':
            escaped[0] = '\\';
            escaped[1] = 'n';
            escaped_len = 2;
            break;
        case '\r':
            escaped[0] = '\\';
            escaped[1] = 'r';
            escaped_len = 2;
            break;
        case '\t':
            escaped[0] = '\\';
            escaped[1] = 't';
            escaped_len = 2;
            break;
        default:
            if (ch < 0x20U) {
                unsigned high = ch >> 4;
                unsigned low = ch & 0x0fU;

                escaped[0] = '\\';
                escaped[1] = 'u';
                escaped[2] = '0';
                escaped[3] = '0';
                escaped[4] = (char)(high < 10U ? '0' + high : 'a' + high - 10U);
                escaped[5] = (char)(low < 10U ? '0' + low : 'a' + low - 10U);
                escaped_len = 6;
            }
            else {
                escaped[0] = (char)ch;
                escaped_len = 1;
            }
            break;
        }
        if (sink_append(sink, escaped, escaped_len) != 0)
            return -1;
    }
    return sink_append(sink, &quote, 1);
}

/*
 * Explicitly opt-in, deterministic response for commissioning hardware with
 * no Wi-Fi or API credentials.  The request still traverses fx's real ACP,
 * model/provider, streaming parser, and WAMR/native-import paths; only the
 * remote HTTP exchange is replaced.  Production boots leave this unset.
 */
static int
serve_fixture_chat(const char *method, struct byte_sink *sink,
                   uint16_t *status)
{
    /* Intentionally RAM-backed: these buffers are copied bytewise into the
     * response stream on the target. */
    static char prefix[] =
        "data: {\"type\":\"text-delta\",\"delta\":";
    static char suffix[] =
        "}\n\n"
        "data: {\"type\":\"finish\",\"finishReason\":{"
        "\"unified\":\"stop\",\"raw\":\"stop\"},\"usage\":{"
        "\"inputTokens\":{\"total\":1},\"outputTokens\":{"
        "\"total\":1}}}\n\n"
        "data: [DONE]\n\n";
    const char *fixture = getenv("FX_WAMR_FIXTURE_TEXT");

    if (!fixture || !*fixture || strcmp(method, "POST") != 0)
        return 0;
    if (strlen(fixture) > FX_WAMR_MAX_FIXTURE_BYTES)
        return -1;
    if (sink_append(sink, prefix, sizeof(prefix) - 1U) != 0
        || sink_append_json_string(sink, fixture) != 0
        || sink_append(sink, suffix, sizeof(suffix) - 1U) != 0)
        return -1;
    *status = 200;
    return 1;
}

static int
perform_request(const uint8_t *method_bytes, uint32_t method_len,
                const uint8_t *url_bytes, uint32_t url_len,
                const uint8_t *headers_json, uint32_t headers_len,
                const uint8_t *body, uint32_t body_len,
                struct byte_sink *sink, uint16_t *status)
{
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    char *method = NULL;
    char *url = NULL;
    CURLcode result;
    char curl_error[CURL_ERROR_SIZE] = { 0 };
    long response_code = 0;
    int rc = -1;
    int local_catalog;
    int fixture_chat;
#ifdef FX_WAMR_MBEDTLS_STREAM_CA
    struct request_trust trust = { 0 };
    mbedtls_x509_crt_init(&trust.roots);
#endif

    method = copy_string(method_bytes, method_len, FX_WAMR_MAX_METHOD_BYTES);
    url = copy_string(url_bytes, url_len, FX_WAMR_MAX_URL_BYTES);
    if (url)
        url = rewrite_gateway_url(url);
    if (!method || !url)
        goto done;
    local_catalog = serve_local_model_catalog(method, url, sink, status);
    if (local_catalog < 0)
        goto done;
    if (local_catalog > 0) {
        rc = 0;
        goto done;
    }
    fixture_chat = serve_fixture_chat(method, sink, status);
    if (fixture_chat < 0)
        goto done;
    if (fixture_chat > 0) {
        rc = 0;
        goto done;
    }
    if (parse_headers(headers_json, headers_len, &headers) != 0)
        goto done;
    curl = curl_easy_init();
    if (!curl)
        goto done;
    {
        const char *ca_bundle = getenv("FX_WAMR_CA_BUNDLE");
#ifdef FX_WAMR_MBEDTLS_STREAM_CA
        trust.path = ca_bundle ? ca_bundle : "/etc/ssl/certs/ca-certificates.crt";
        /* Suppress the backend's whole-file loader, not peer verification.
         * The callback must supply a valid chain or fail the handshake. */
        if (trust.path[0] != '/'
            || curl_easy_setopt(curl, CURLOPT_CAINFO, NULL) != CURLE_OK
            || curl_easy_setopt(curl, CURLOPT_CAPATH, NULL) != CURLE_OK
            || curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION,
                                load_trust_roots) != CURLE_OK
            || curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, &trust) != CURLE_OK)
            goto done;
#else
        /* Operator-supplied trust roots for rootfs images without a CA bundle.
         * This never disables certificate or hostname verification. */
        if (ca_bundle && (ca_bundle[0] != '/'
            || curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle) != CURLE_OK))
            goto done;
#endif
    }
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_BUFFERSIZE,
                            FX_WAMR_CURL_IO_BYTES) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE,
                            FX_WAMR_CURL_IO_BYTES) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_IPRESOLVE,
                            CURL_IPRESOLVE_V4) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                            CURL_HTTP_VERSION_1_1) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "") != CURLE_OK
        || curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK)
        goto done;
    if (body_len > 0
        && (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK
            || curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                (curl_off_t)body_len) != CURLE_OK))
        goto done;
    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr, "fx-wamr: HTTPS request failed curl_code=%d detail=%s\n",
                    (int)result, curl_error[0] ? curl_error : "unavailable");
        trace_system_memory("provider request failure");
        goto done;
    }
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code) != CURLE_OK
        || response_code < 0 || response_code > UINT16_MAX)
        goto done;
    trace_system_memory("provider response buffered");
    *status = (uint16_t)response_code;
    rc = 0;

done:
    if (curl)
        curl_easy_cleanup(curl);
#ifdef FX_WAMR_MBEDTLS_STREAM_CA
    mbedtls_x509_crt_free(&trust.roots);
#endif
    curl_slist_free_all(headers);
    free(method);
    free(url);
    return rc;
}

static int32_t
fx_http_request_wrapper(wasm_exec_env_t exec_env,
                        const uint8_t *method, uint32_t method_len,
                        const uint8_t *url, uint32_t url_len,
                        const uint8_t *headers, uint32_t headers_len,
                        const uint8_t *body, uint32_t body_len,
                        uint16_t *status_out, uint8_t *response,
                        uint32_t response_cap)
{
    struct byte_sink sink = {
        .bytes = response,
        .len = 0,
        .cap = response_cap,
        .owned = 0,
        .overflow = 0,
    };
    (void)exec_env;
    if (perform_request(method, method_len, url, url_len, headers, headers_len,
                        body, body_len, &sink, status_out) != 0)
        return sink.overflow ? -2 : -1;
    return (int32_t)sink.len;
}

static int32_t
fx_http_stream_open_wrapper(wasm_exec_env_t exec_env,
                            const uint8_t *method, uint32_t method_len,
                            const uint8_t *url, uint32_t url_len,
                            const uint8_t *headers, uint32_t headers_len,
                            const uint8_t *body, uint32_t body_len)
{
    struct byte_sink sink;
    uint16_t status = 0;
    int slot = -1;
    int i;
    trace_guest_pages(exec_env, "before provider request");

    for (i = 0; i < FX_WAMR_MAX_STREAMS; i++) {
        if (!streams[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    if (borrowed_stream_buffer) {
        for (i = 0; i < FX_WAMR_MAX_STREAMS; i++) {
            if (streams[i].used)
                return -1;
        }
        sink = (struct byte_sink) {
            .bytes = borrowed_stream_buffer,
            .len = 0,
            .cap = borrowed_stream_buffer_bytes,
            .owned = 0,
            .overflow = 0,
        };
    }
    else {
        sink = (struct byte_sink) {
            .bytes = NULL,
            .len = 0,
            .cap = FX_WAMR_MAX_STREAM_BYTES,
            .owned = 1,
            .overflow = 0,
        };
    }
    if (perform_request(method, method_len, url, url_len, headers, headers_len,
                        body, body_len, &sink, &status) != 0) {
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-wamr: stream open failed (buffered=%lu, overflow=%d)\n",
                    (unsigned long)sink.len, sink.overflow);
        if (sink.owned)
            free(sink.bytes);
        return -1;
    }
    streams[slot].used = 1;
    streams[slot].owned = sink.owned;
    streams[slot].status = status;
    streams[slot].bytes = sink.bytes;
    streams[slot].len = sink.len;
    streams[slot].cursor = 0;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: stream %d opened (status=%u, buffered=%lu)\n",
                slot + 1, (unsigned int)status, (unsigned long)sink.len);
    return slot + 1;
}

static int32_t
fx_http_stream_status_wrapper(wasm_exec_env_t exec_env, int32_t handle,
                              uint16_t *status_out)
{
    struct stream_slot *slot;
    (void)exec_env;
    if (handle < 1 || handle > FX_WAMR_MAX_STREAMS)
        return -1;
    slot = &streams[handle - 1];
    if (!slot->used)
        return -1;
    *status_out = slot->status;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-wamr: stream %d status=%u\n", handle,
                (unsigned int)slot->status);
    return 1;
}

static int32_t
fx_http_stream_next_wrapper(wasm_exec_env_t exec_env, int32_t handle,
                            uint8_t *out, uint32_t cap)
{
    struct stream_slot *slot;
    size_t remaining;
    size_t count;
    (void)exec_env;
    if (handle < 1 || handle > FX_WAMR_MAX_STREAMS)
        return -1;
    slot = &streams[handle - 1];
    if (!slot->used)
        return -1;
    remaining = slot->len - slot->cursor;
    count = remaining < cap ? remaining : cap;
    if (count)
        memcpy(out, slot->bytes + slot->cursor, count);
    slot->cursor += count;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-wamr: stream %d next (capacity=%u, returned=%lu, cursor=%lu/%lu)\n",
                handle, cap, (unsigned long)count,
                (unsigned long)slot->cursor, (unsigned long)slot->len);
    return (int32_t)count;
}

static void
fx_http_stream_close_wrapper(wasm_exec_env_t exec_env, int32_t handle)
{
    struct stream_slot *slot;
    (void)exec_env;
    if (handle < 1 || handle > FX_WAMR_MAX_STREAMS)
        return;
    slot = &streams[handle - 1];
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-wamr: stream %d closed at %lu/%lu bytes\n",
                handle, (unsigned long)slot->cursor,
                (unsigned long)slot->len);
    if (slot->owned)
        free(slot->bytes);
    memset(slot, 0, sizeof(*slot));
}

static int32_t
fx_session_list_wrapper(wasm_exec_env_t exec_env, uint8_t *out,
                        uint32_t cap)
{
    (void)exec_env;
    if (cap < 2)
        return -2;
    out[0] = '[';
    out[1] = ']';
    return 2;
}

static int32_t
fx_session_remove_wrapper(wasm_exec_env_t exec_env, const uint8_t *id,
                          uint32_t id_len)
{
    (void)exec_env;
    (void)id;
    (void)id_len;
    return 0;
}

static int32_t
fx_session_load_wrapper(wasm_exec_env_t exec_env, const uint8_t *id,
                        uint32_t id_len, uint8_t *bytes, uint32_t bytes_cap,
                        uint8_t *revision, uint32_t revision_cap,
                        uint32_t *revision_len)
{
    (void)exec_env;
    (void)id;
    (void)id_len;
    (void)bytes;
    (void)bytes_cap;
    (void)revision;
    (void)revision_cap;
    *revision_len = 0;
    return -2;
}

static int32_t
fx_session_commit_wrapper(wasm_exec_env_t exec_env, const uint8_t *id,
                          uint32_t id_len, const uint8_t *bytes,
                          uint32_t bytes_len, const uint8_t *expected,
                          uint32_t expected_len, uint8_t *revision,
                          uint32_t revision_cap, uint32_t *revision_len)
{
    /* See rewrite_gateway_url(): memcpy's source must be RAM-backed here. */
    static uint8_t ephemeral_revision[] = "ephemeral-1";
    (void)exec_env;
    (void)id;
    (void)id_len;
    (void)bytes;
    (void)bytes_len;
    (void)expected;
    (void)expected_len;
    if (revision_cap < sizeof(ephemeral_revision) - 1U)
        return -1;
    memcpy(revision, ephemeral_revision, sizeof(ephemeral_revision) - 1U);
    *revision_len = sizeof(ephemeral_revision) - 1U;
    return 0;
}

static int32_t
fx_oauth_session_load_wrapper(wasm_exec_env_t exec_env, uint8_t *bytes,
                              uint32_t bytes_cap, uint8_t *revision,
                              uint32_t revision_cap,
                              uint32_t *revision_len)
{
    (void)exec_env;
    (void)bytes;
    (void)bytes_cap;
    (void)revision;
    (void)revision_cap;
    *revision_len = 0;
    return -2;
}

static int32_t
fx_oauth_session_commit_wrapper(wasm_exec_env_t exec_env,
                                const uint8_t *bytes, uint32_t bytes_len,
                                const uint8_t *expected,
                                uint32_t expected_len, uint8_t *revision,
                                uint32_t revision_cap,
                                uint32_t *revision_len)
{
    (void)exec_env;
    (void)bytes;
    (void)bytes_len;
    (void)expected;
    (void)expected_len;
    (void)revision;
    (void)revision_cap;
    *revision_len = 0;
    return -1;
}

static int32_t
fx_oauth_session_remove_wrapper(wasm_exec_env_t exec_env,
                                const uint8_t *expected,
                                uint32_t expected_len)
{
    (void)exec_env;
    (void)expected;
    (void)expected_len;
    return 1;
}

static int32_t
fx_host_tool_call_wrapper(wasm_exec_env_t exec_env, const uint8_t *name,
                          uint32_t name_len, const uint8_t *arguments,
                          uint32_t arguments_len, uint8_t *output,
                          uint32_t output_cap, uint8_t *status)
{
    trace_guest_pages(exec_env, "before tool call");
    return fx_example_tool_call(name, name_len, arguments, arguments_len,
                                output, output_cap, status);
}

static int32_t
fx_host_tool_result_read_wrapper(wasm_exec_env_t exec_env, uint32_t offset,
                                 uint8_t *out, uint32_t cap)
{
    (void)exec_env;
    (void)offset;
    (void)out;
    (void)cap;
    return -1;
}

static void
fx_host_tool_result_release_wrapper(wasm_exec_env_t exec_env)
{
    (void)exec_env;
}

#define FX_NATIVE(name, signature) \
    { #name, (void *)name##_wrapper, signature, NULL }

static NativeSymbol native_symbols[] = {
    FX_NATIVE(fx_session_list, "(*~)i"),
    FX_NATIVE(fx_session_remove, "(*~)i"),
    FX_NATIVE(fx_session_load, "(*~*~*~*)i"),
    FX_NATIVE(fx_oauth_session_remove, "(*~)i"),
    FX_NATIVE(fx_oauth_session_commit, "(*~*~*~*)i"),
    FX_NATIVE(fx_oauth_session_load, "(*~*~*)i"),
    FX_NATIVE(fx_session_commit, "(*~*~*~*~*)i"),
    FX_NATIVE(fx_host_tool_call, "(*~*~*~*)i"),
    FX_NATIVE(fx_host_tool_result_release, "()"),
    FX_NATIVE(fx_host_tool_result_read, "(i*~)i"),
    FX_NATIVE(fx_http_request, "(*~*~*~*~**~)i"),
    FX_NATIVE(fx_http_stream_close, "(i)"),
    FX_NATIVE(fx_http_stream_next, "(i*~)i"),
    FX_NATIVE(fx_http_stream_status, "(i*)i"),
    FX_NATIVE(fx_http_stream_open, "(*~*~*~*~)i"),
};

int
init_native_lib(void)
{
    memset(streams, 0, sizeof(streams));
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void
deinit_native_lib(void)
{
    int i;
    for (i = 0; i < FX_WAMR_MAX_STREAMS; i++) {
        if (streams[i].owned)
            free(streams[i].bytes);
        memset(&streams[i], 0, sizeof(streams[i]));
    }
    borrowed_stream_buffer = NULL;
    borrowed_stream_buffer_bytes = 0;
    curl_global_cleanup();
}

uint32_t
get_native_lib(char **module_name, NativeSymbol **symbols)
{
    *module_name = "fx";
    *symbols = native_symbols;
    return (uint32_t)(sizeof(native_symbols) / sizeof(native_symbols[0]));
}
