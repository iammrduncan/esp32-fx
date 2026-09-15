// SPDX-License-Identifier: MIT
/* One-shot ACP client that sends a prompt to fx-core and paints the result. */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* uClibc deliberately omits fork(2)'s declaration for NOMMU targets.  The
 * ESP32-S3 Linux image supplies its software-banked compatibility shim. */
extern pid_t fork(void);

#define DEFAULT_RUNNER "/usr/bin/fx-wamr"
#ifndef DEFAULT_MODULE
#define DEFAULT_MODULE "/usr/share/fx/fx-core.wasm"
#endif
#define DEFAULT_EINKCTL "/usr/bin/einkctl"
#define MAX_PROMPT_BYTES 4096U
#define MAX_RESPONSE_BYTES (32U * 1024U)
#define MAX_PROTOCOL_LINE_BYTES (128U * 1024U)

#if defined(FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS) \
    && !defined(FX_EINK_WAMR_LAUNCH_GUARD_BYTES)
#error "a fixed WAMR arena address requires its size"
#endif
#if defined(FX_EINK_NATIVE_STREAM_GUARD_ADDRESS) \
    && !defined(FX_EINK_NATIVE_STREAM_GUARD_BYTES)
#error "a fixed native stream address requires its size"
#endif
#if defined(FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS) \
    != defined(FX_EINK_NATIVE_STREAM_GUARD_ADDRESS)
#error "the fixed WAMR arena and native stream buffer must be configured together"
#endif
#if defined(FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS) \
    && defined(FX_EINK_NATIVE_STREAM_GUARD_ADDRESS)
_Static_assert(
    FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS
            + FX_EINK_WAMR_LAUNCH_GUARD_BYTES
        <= FX_EINK_NATIVE_STREAM_GUARD_ADDRESS
        || FX_EINK_NATIVE_STREAM_GUARD_ADDRESS
                   + FX_EINK_NATIVE_STREAM_GUARD_BYTES
               <= FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS,
    "fixed WAMR reservations overlap");
#endif

struct fx_process {
    pid_t pid;
    FILE *input;
    FILE *output;
};

struct text_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

#if defined(FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS)
#include "fx_memory.h"
#endif

#if defined(FX_EINK_WAMR_LAUNCH_GUARD_BYTES) \
    || defined(FX_EINK_NATIVE_STREAM_GUARD_BYTES)
static void wipe_reserved_psram(void *address, size_t length)
{
    volatile uint8_t *cursor = address;

    while (length-- > 0)
        *cursor++ = 0;
}
#endif

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: fx-eink [OPTIONS] PROMPT...\n"
            "\n"
            "Options:\n"
            "  --render FILE   render a PBM instead of refreshing the panel\n"
            "  --runner FILE   fx WAMR runner (default: %s)\n"
            "  --wasm FILE     fx-core Wasm module (default: %s)\n"
            "  --einkctl FILE  display renderer (default: %s)\n"
            "  --title TEXT    e-paper header text\n"
            "  --wait-ready FILE\n"
            "                  reserve guest memory, then wait up to 180s for FILE\n"
            "  -h, --help      show this help\n",
            DEFAULT_RUNNER, DEFAULT_MODULE, DEFAULT_EINKCTL);
}

static int buffer_append(struct text_buffer *buffer, const char *text,
                         size_t length)
{
    size_t needed;
    size_t capacity;
    char *grown;

    if (length > MAX_RESPONSE_BYTES - buffer->length) {
        errno = EFBIG;
        return -1;
    }
    needed = buffer->length + length + 1U;
    if (needed > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 1024U;
        while (capacity < needed)
            capacity *= 2U;
        grown = realloc(buffer->data, capacity);
        if (!grown)
            return -1;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int append_utf8(struct text_buffer *buffer, uint32_t codepoint)
{
    char bytes[4];
    size_t length;

    if (codepoint <= 0x7fU) {
        bytes[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ffU) {
        bytes[0] = (char)(0xc0U | (codepoint >> 6));
        bytes[1] = (char)(0x80U | (codepoint & 0x3fU));
        length = 2;
    } else if (codepoint <= 0xffffU
               && !(codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        bytes[0] = (char)(0xe0U | (codepoint >> 12));
        bytes[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        bytes[2] = (char)(0x80U | (codepoint & 0x3fU));
        length = 3;
    } else {
        bytes[0] = '?';
        length = 1;
    }
    return buffer_append(buffer, bytes, length);
}

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9')
        return byte - '0';
    if (byte >= 'a' && byte <= 'f')
        return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F')
        return byte - 'A' + 10;
    return -1;
}

static int decode_json_string(const char **cursor, struct text_buffer *decoded)
{
    const char *p = *cursor;

    if (*p++ != '"')
        return -1;
    while (*p && *p != '"') {
        unsigned char byte = (unsigned char)*p++;

        if (byte < 0x20U)
            return -1;
        if (byte != '\\') {
            if (buffer_append(decoded, (const char *)&byte, 1) < 0)
                return -1;
            continue;
        }
        switch (*p++) {
        case '"': byte = '"'; break;
        case '\\': byte = '\\'; break;
        case '/': byte = '/'; break;
        case 'b': byte = '\b'; break;
        case 'f': byte = '\f'; break;
        case 'n': byte = '\n'; break;
        case 'r': byte = '\r'; break;
        case 't': byte = '\t'; break;
        case 'u': {
            uint32_t codepoint = 0;
            unsigned int index;

            for (index = 0; index < 4; index++) {
                int value = hex_value((unsigned char)*p++);

                if (value < 0)
                    return -1;
                codepoint = (codepoint << 4) | (uint32_t)value;
            }
            if (append_utf8(decoded, codepoint) < 0)
                return -1;
            continue;
        }
        default:
            return -1;
        }
        if (buffer_append(decoded, (const char *)&byte, 1) < 0)
            return -1;
    }
    if (*p++ != '"')
        return -1;
    *cursor = p;
    return 0;
}

static void skip_whitespace(const char **cursor)
{
    while (**cursor && isspace((unsigned char)**cursor))
        (*cursor)++;
}

static const char *find_json_key(const char *json, const char *wanted)
{
    const char *cursor = json;

    while ((cursor = strchr(cursor, '"')) != NULL) {
        struct text_buffer key = { 0 };
        const char *after = cursor;
        int decoded = decode_json_string(&after, &key);
        bool matches = decoded == 0 && key.data && strcmp(key.data, wanted) == 0;

        free(key.data);
        if (decoded < 0) {
            cursor++;
            continue;
        }
        cursor = after;
        skip_whitespace(&cursor);
        if (*cursor != ':')
            continue;
        cursor++;
        skip_whitespace(&cursor);
        if (matches)
            return cursor;
    }
    return NULL;
}

static int json_string_value(const char *json, const char *key,
                             struct text_buffer *value)
{
    const char *cursor = find_json_key(json, key);

    if (!cursor || *cursor != '"')
        return 0;
    if (decode_json_string(&cursor, value) < 0)
        return -1;
    return 1;
}

static int json_integer_value(const char *json, const char *key, long *value)
{
    const char *cursor = find_json_key(json, key);
    char *end;
    long parsed;

    if (!cursor)
        return 0;
    errno = 0;
    parsed = strtol(cursor, &end, 10);
    if (errno != 0 || end == cursor)
        return -1;
    *value = parsed;
    return 1;
}

static int write_json_string(FILE *output, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (fputc('"', output) == EOF)
        return -1;
    while (*cursor) {
        unsigned char byte = *cursor++;

        switch (byte) {
        case '"': if (fputs("\\\"", output) == EOF) return -1; break;
        case '\\': if (fputs("\\\\", output) == EOF) return -1; break;
        case '\b': if (fputs("\\b", output) == EOF) return -1; break;
        case '\f': if (fputs("\\f", output) == EOF) return -1; break;
        case '\n': if (fputs("\\n", output) == EOF) return -1; break;
        case '\r': if (fputs("\\r", output) == EOF) return -1; break;
        case '\t': if (fputs("\\t", output) == EOF) return -1; break;
        default:
            if (byte < 0x20U) {
                if (fprintf(output, "\\u%04x", byte) < 0)
                    return -1;
            } else if (fputc(byte, output) == EOF) {
                return -1;
            }
        }
    }
    return fputc('"', output) == EOF ? -1 : 0;
}

static int start_fx(struct fx_process *process, const char *runner,
                    const char *module)
{
    int to_child[2];
    int from_child[2];
    pid_t pid;

    memset(process, 0, sizeof(*process));
    process->pid = -1;
    if (pipe(to_child) < 0) {
        fprintf(stderr, "fx-eink: cannot create ACP pipes: %s\n", strerror(errno));
        return -1;
    }
    if (pipe(from_child) < 0) {
        int saved_errno = errno;

        close(to_child[0]);
        close(to_child[1]);
        errno = saved_errno;
        fprintf(stderr, "fx-eink: cannot create ACP pipes: %s\n", strerror(errno));
        return -1;
    }
    /* This target is NOMMU.  A regular fork copies the supervisor's private
     * mappings and can split the only 2 MiB PSRAM block before WAMR reserves
     * it.  The child immediately execs, so vfork is both safe here and the
     * native NOMMU process-launch primitive. */
    pid = vfork();
    if (pid < 0) {
        fprintf(stderr, "fx-eink: cannot vfork fx runtime: %s\n",
                strerror(errno));
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        return -1;
    }
    if (pid == 0) {
        if (dup2(to_child[0], STDIN_FILENO) < 0
            || dup2(from_child[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        execl(runner, runner, module, (char *)NULL);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    process->pid = pid;
    process->input = fdopen(to_child[1], "w");
    process->output = fdopen(from_child[0], "r");
    if (!process->input || !process->output) {
        fprintf(stderr, "fx-eink: cannot open ACP streams: %s\n", strerror(errno));
        if (process->input)
            fclose(process->input);
        else
            close(to_child[1]);
        if (process->output)
            fclose(process->output);
        else
            close(from_child[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        process->pid = -1;
        return -1;
    }
    return 0;
}

static int finish_fx(struct fx_process *process)
{
    int status;

    if (process->input) {
        fclose(process->input);
        process->input = NULL;
    }
    if (process->output) {
        fclose(process->output);
        process->output = NULL;
    }
    if (process->pid < 0)
        return -1;
    while (waitpid(process->pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    process->pid = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "fx-eink: fx runtime exited abnormally\n");
        return -1;
    }
    return 0;
}

static int send_initialize(FILE *input)
{
    const char *jail = getenv("FX_EXAMPLE_JAIL");
    if (jail && *jail) {
        /* Host-supplied libfx tools use the existing Wasm import ABI. */
        return fprintf(input,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":1,\"clientCapabilities\":{\"libfx\":{"
            "\"instructions\":\"Repair this tiny shell project using the tools. Read SPEC.md first. "
            "Only battery.sh is writable. Always run tests before and after editing.\","
            "\"tools\":["
            "{\"name\":\"read_project\",\"description\":\"Read battery.sh, SPEC.md or test.sh in the project.\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
            "\"required\":[\"path\"],\"additionalProperties\":false}},"
            "{\"name\":\"replace_battery\",\"description\":\"Replace battery.sh with ASCII POSIX shell source, maximum 4096 bytes.\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
            "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],\"additionalProperties\":false}},"
            "{\"name\":\"run_tests\",\"description\":\"Run the fixed project tests. Returns output and exit_code. No arguments.\","
            "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}"
            "]}}}}\n") < 0 || fflush(input) != 0 ? -1 : 0;
    }
    return fprintf(input,
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                   "\"params\":{\"protocolVersion\":1,\"clientCapabilities\":{}}}\n") < 0
               || fflush(input) != 0
           ? -1
           : 0;
}

static int send_new_session(FILE *input)
{
    return fprintf(input,
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"libfx/new\"}\n") < 0
               || fflush(input) != 0
           ? -1
           : 0;
}

static int send_prompt(FILE *input, const char *session_id, const char *prompt)
{
    if (fputs("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session/prompt\","
              "\"params\":{\"sessionId\":",
              input) == EOF
        || write_json_string(input, session_id) < 0
        || fputs(",\"prompt\":[{\"type\":\"text\",\"text\":", input) == EOF
        || write_json_string(input, prompt) < 0
        || fputs("}]}}\n", input) == EOF || fflush(input) != 0)
        return -1;
    return 0;
}

static int read_until_id(FILE *output, long wanted_id, struct text_buffer *line,
                         struct text_buffer *response)
{
    char *raw = NULL;
    size_t raw_capacity = 0;
    ssize_t raw_length;

    while ((raw_length = getline(&raw, &raw_capacity, output)) >= 0) {
        long id;
        int has_id;
        struct text_buffer update = { 0 };
        int has_update;

        if ((size_t)raw_length > MAX_PROTOCOL_LINE_BYTES) {
            fprintf(stderr, "fx-eink: ACP line exceeds %u bytes\n",
                    MAX_PROTOCOL_LINE_BYTES);
            free(raw);
            return -1;
        }
        if (getenv("FX_EINK_DIAGNOSTICS"))
            fprintf(stderr, "fx-eink: ACP <= %.*s",
                    (int)raw_length, raw);
        has_update = json_string_value(raw, "sessionUpdate", &update);
        if (has_update < 0) {
            free(update.data);
            free(raw);
            return -1;
        }
        if (has_update > 0 && update.data
            && strcmp(update.data, "agent_message_chunk") == 0 && response) {
            struct text_buffer text = { 0 };
            int has_text = json_string_value(raw, "text", &text);

            if (has_text < 0 || (has_text > 0
                && buffer_append(response, text.data, text.length) < 0)) {
                free(text.data);
                free(update.data);
                free(raw);
                return -1;
            }
            free(text.data);
        }
        free(update.data);

        has_id = json_integer_value(raw, "id", &id);
        if (has_id < 0) {
            free(raw);
            return -1;
        }
        if (has_id > 0 && id == wanted_id) {
            if (find_json_key(raw, "error")) {
                fprintf(stderr, "fx-eink: fx returned an ACP error: %s", raw);
                free(raw);
                return -1;
            }
            free(line->data);
            line->data = raw;
            line->length = (size_t)raw_length;
            line->capacity = raw_capacity;
            return 0;
        }
    }
    fprintf(stderr, "fx-eink: ACP stream ended before response %ld\n", wanted_id);
    free(raw);
    return -1;
}

static char *join_prompt(int argc, char **argv, int first)
{
    size_t length = 0;
    char *prompt;
    char *cursor;
    int index;

    for (index = first; index < argc; index++) {
        size_t part = strlen(argv[index]);
        size_t separator = index > first ? 1U : 0U;

        if (separator > MAX_PROMPT_BYTES - length
            || part > MAX_PROMPT_BYTES - length - separator) {
            errno = EFBIG;
            return NULL;
        }
        length += separator + part;
    }
    if (length == 0 || length > MAX_PROMPT_BYTES) {
        errno = EINVAL;
        return NULL;
    }
    prompt = malloc(length + 1U);
    if (!prompt)
        return NULL;
    cursor = prompt;
    for (index = first; index < argc; index++) {
        size_t part = strlen(argv[index]);

        if (index > first)
            *cursor++ = ' ';
        memcpy(cursor, argv[index], part);
        cursor += part;
    }
    *cursor = '\0';
    return prompt;
}

static int write_transcript(const char *path, const char *prompt,
                            const struct text_buffer *response)
{
    FILE *output = fopen(path, "w");
    int failed;

    if (!output)
        return -1;
    failed = fprintf(output, "You:\n%s\n\nfx:\n%s\n", prompt,
                     response->data ? response->data : "(no text response)") < 0;
    if (fclose(output) != 0)
        failed = 1;
    return failed ? -1 : 0;
}

static int run_renderer(const char *einkctl, const char *render_path,
                        const char *title, const char *transcript)
{
    pid_t pid = vfork();
    int status;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (render_path) {
            execl(einkctl, einkctl, "render", "--output", render_path,
                  "--title", title, transcript, (char *)NULL);
        } else {
            execl(einkctl, einkctl, "display", "--title", title, transcript,
                  (char *)NULL);
        }
        fprintf(stderr, "fx-eink: cannot execute %s: %s\n", einkctl,
                strerror(errno));
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int readiness_marker_state(const char *path)
{
    static char ready[] = "ready\n";
    static char failed[] = "failed\n";
    char state[sizeof(failed)];
    ssize_t count;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);

    if (descriptor < 0)
        return errno == ENOENT ? 0 : -1;
    count = read(descriptor, state, sizeof(state));
    close(descriptor);
    if (count == (ssize_t)(sizeof(ready) - 1U)
        && memcmp(state, ready, sizeof(ready) - 1U) == 0)
        return 1;
    if (count == (ssize_t)(sizeof(failed) - 1U)
        && memcmp(state, failed, sizeof(failed) - 1U) == 0)
        return -1;
    return -1;
}

int main(int argc, char **argv)
{
    const char *runner = getenv("FX_WAMR_RUNNER");
    const char *module = getenv("FX_WASM_MODULE");
    const char *einkctl = getenv("EINKCTL");
    const char *render_path = NULL;
    const char *title = "fx | ESP32-S3 Linux";
    const char *ready_path = NULL;
    struct fx_process process = { .pid = -1 };
    struct text_buffer line = { 0 };
    struct text_buffer session_id = { 0 };
    struct text_buffer response = { 0 };
    char *prompt = NULL;
    char transcript[] = "/tmp/fx-eink.XXXXXX";
    int transcript_fd = -1;
    bool transcript_created = false;
#ifdef FX_EINK_WAMR_LAUNCH_GUARD_BYTES
    void *launch_guard = NULL;
    size_t launch_guard_bytes = 0;
    bool launch_guard_mapped = false;
#endif
#ifdef FX_EINK_NATIVE_STREAM_GUARD_BYTES
    void *native_stream_guard = NULL;
    size_t native_stream_guard_bytes = 0;
    bool native_stream_guard_mapped = false;
#endif
    int option;
    int result = EXIT_FAILURE;
    static const struct option options[] = {
        { "render", required_argument, NULL, 'r' },
        { "runner", required_argument, NULL, 'R' },
        { "wasm", required_argument, NULL, 'w' },
        { "einkctl", required_argument, NULL, 'e' },
        { "title", required_argument, NULL, 't' },
        { "wait-ready", required_argument, NULL, 'W' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

#ifdef FX_EINK_WAMR_LAUNCH_GUARD_BYTES
    char launch_guard_address[2 + sizeof(uintptr_t) * 2 + 1];

    launch_guard_bytes = FX_EINK_WAMR_LAUNCH_GUARD_BYTES;
#ifdef FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS
    if (fx_verify_reserved_psram() < 0)
        goto cleanup;
    launch_guard =
        (void *)(uintptr_t)FX_EINK_WAMR_LAUNCH_GUARD_ADDRESS;
    wipe_reserved_psram(launch_guard, launch_guard_bytes);
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr,
                "fx-eink: using %lu-byte boot-reserved WAMR arena at %p\n",
                (unsigned long)launch_guard_bytes, launch_guard);
#else
    launch_guard = mmap(NULL, launch_guard_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (launch_guard == MAP_FAILED) {
        launch_guard = NULL;
        fprintf(stderr, "fx-eink: cannot reserve WAMR launch guard: %s\n",
                strerror(errno));
        goto cleanup;
    }
    launch_guard_mapped = true;
    if (getenv("FX_WAMR_DIAGNOSTICS"))
        fprintf(stderr, "fx-eink: holding %lu-byte WAMR launch guard at %p\n",
                (unsigned long)launch_guard_bytes, launch_guard);
#endif
    snprintf(launch_guard_address, sizeof(launch_guard_address), "0x%lx",
             (unsigned long)(uintptr_t)launch_guard);
    if (setenv("FX_WAMR_LINEAR_MEMORY_ADDRESS", launch_guard_address, 1) < 0) {
        fprintf(stderr, "fx-eink: cannot publish WAMR launch guard: %s\n",
                strerror(errno));
        goto cleanup;
    }
#endif

#ifdef FX_EINK_NATIVE_STREAM_GUARD_BYTES
    {
        char native_stream_guard_address[2 + sizeof(uintptr_t) * 2 + 1];

        native_stream_guard_bytes = FX_EINK_NATIVE_STREAM_GUARD_BYTES;
#ifdef FX_EINK_NATIVE_STREAM_GUARD_ADDRESS
        native_stream_guard =
            (void *)(uintptr_t)FX_EINK_NATIVE_STREAM_GUARD_ADDRESS;
        wipe_reserved_psram(native_stream_guard,
                            native_stream_guard_bytes);
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-eink: using %lu-byte boot-reserved native stream buffer at %p\n",
                    (unsigned long)native_stream_guard_bytes,
                    native_stream_guard);
#else
        native_stream_guard =
            mmap(NULL, native_stream_guard_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (native_stream_guard == MAP_FAILED) {
            native_stream_guard = NULL;
            fprintf(stderr,
                    "fx-eink: cannot reserve native stream guard: %s\n",
                    strerror(errno));
            goto cleanup;
        }
        native_stream_guard_mapped = true;
        if (getenv("FX_WAMR_DIAGNOSTICS"))
            fprintf(stderr,
                    "fx-eink: holding %lu-byte native stream guard at %p\n",
                    (unsigned long)native_stream_guard_bytes,
                    native_stream_guard);
#endif
        snprintf(native_stream_guard_address,
                 sizeof(native_stream_guard_address), "0x%lx",
                 (unsigned long)(uintptr_t)native_stream_guard);
        if (setenv("FX_WAMR_NATIVE_STREAM_ADDRESS",
                   native_stream_guard_address, 1) < 0) {
            fprintf(stderr,
                    "fx-eink: cannot publish native stream guard: %s\n",
                    strerror(errno));
            goto cleanup;
        }
    }
#endif

    if (!runner || !*runner)
        runner = DEFAULT_RUNNER;
    if (!module || !*module)
        module = DEFAULT_MODULE;
    if (!einkctl || !*einkctl)
        einkctl = DEFAULT_EINKCTL;

    while ((option = getopt_long(argc, argv, "r:R:w:e:t:W:h", options, NULL)) != -1) {
        switch (option) {
        case 'r': render_path = optarg; break;
        case 'R': runner = optarg; break;
        case 'w': module = optarg; break;
        case 'e': einkctl = optarg; break;
        case 't': title = optarg; break;
        case 'W': ready_path = optarg; break;
        case 'h': usage(stdout); result = EXIT_SUCCESS; goto cleanup;
        default: usage(stderr); goto cleanup;
        }
    }
    prompt = join_prompt(argc, argv, optind);
    if (!prompt) {
        fprintf(stderr, "fx-eink: prompt is missing or exceeds %u bytes\n",
                MAX_PROMPT_BYTES);
        usage(stderr);
        goto cleanup;
    }
    if (ready_path) {
        unsigned int waited;
        int ready_state = 0;

        if (strncmp(ready_path, "/tmp/", 5) != 0
            || strlen(ready_path) > 255U) {
            fprintf(stderr, "fx-eink: readiness marker must be an absolute /tmp path\n");
            goto cleanup;
        }
        if (getenv("FX_EINK_DIAGNOSTICS"))
            fprintf(stderr, "fx-eink: guest memory reserved; waiting for %s\n",
                    ready_path);
        for (waited = 0;
             (ready_state = readiness_marker_state(ready_path)) == 0
                 && waited < 180U;
             waited++)
            sleep(1);
        if (ready_state < 0) {
            fprintf(stderr, "fx-eink: readiness preflight rejected launch\n");
            goto cleanup;
        }
        if (ready_state == 0) {
            fprintf(stderr, "fx-eink: readiness marker timed out: %s\n",
                    ready_path);
            goto cleanup;
        }
    }
    if (start_fx(&process, runner, module) < 0)
        goto cleanup;
    if (send_initialize(process.input) < 0
        || read_until_id(process.output, 1, &line, NULL) < 0
        || send_new_session(process.input) < 0
        || read_until_id(process.output, 2, &line, NULL) < 0)
        goto cleanup;
    if (json_string_value(line.data, "sessionId", &session_id) <= 0
        || !session_id.data) {
        fprintf(stderr, "fx-eink: new-session response has no sessionId\n");
        goto cleanup;
    }
    if (send_prompt(process.input, session_id.data, prompt) < 0
        || read_until_id(process.output, 3, &line, &response) < 0)
        goto cleanup;
    if (finish_fx(&process) < 0)
        goto cleanup;
#ifdef FX_EINK_WAMR_LAUNCH_GUARD_BYTES
    /* The WAMR child has stopped using the borrowed NOMMU address.  Return
     * the arena before allocating the framebuffer in the renderer. */
    wipe_reserved_psram(launch_guard, launch_guard_bytes);
    if (launch_guard_mapped
        && munmap(launch_guard, launch_guard_bytes) < 0) {
        fprintf(stderr, "fx-eink: cannot release WAMR launch guard: %s\n",
                strerror(errno));
        goto cleanup;
    }
    launch_guard = NULL;
    unsetenv("FX_WAMR_LINEAR_MEMORY_ADDRESS");
#endif
#ifdef FX_EINK_NATIVE_STREAM_GUARD_BYTES
    wipe_reserved_psram(native_stream_guard, native_stream_guard_bytes);
    if (native_stream_guard_mapped
        && munmap(native_stream_guard, native_stream_guard_bytes) < 0) {
        fprintf(stderr,
                "fx-eink: cannot release native stream guard: %s\n",
                strerror(errno));
        goto cleanup;
    }
    native_stream_guard = NULL;
    unsetenv("FX_WAMR_NATIVE_STREAM_ADDRESS");
#endif
    if (!response.data || response.length == 0) {
        fprintf(stderr, "fx-eink: fx completed without a text response\n");
        goto cleanup;
    }

    printf("%s\n", response.data);
    transcript_fd = mkstemp(transcript);
    if (transcript_fd < 0) {
        fprintf(stderr, "fx-eink: cannot create transcript: %s\n", strerror(errno));
        goto cleanup;
    }
    transcript_created = true;
    close(transcript_fd);
    transcript_fd = -1;
    if (write_transcript(transcript, prompt, &response) < 0) {
        fprintf(stderr, "fx-eink: cannot write transcript: %s\n", strerror(errno));
        goto cleanup;
    }
    if (run_renderer(einkctl, render_path, title, transcript) < 0) {
        fprintf(stderr, "fx-eink: renderer failed\n");
        goto cleanup;
    }
    result = EXIT_SUCCESS;

cleanup:
    if (process.pid >= 0) {
        if (process.input) {
            fclose(process.input);
            process.input = NULL;
        }
        if (process.output) {
            fclose(process.output);
            process.output = NULL;
        }
        kill(process.pid, SIGTERM);
        waitpid(process.pid, NULL, 0);
    }
#ifdef FX_EINK_WAMR_LAUNCH_GUARD_BYTES
    if (launch_guard) {
        wipe_reserved_psram(launch_guard, launch_guard_bytes);
        if (launch_guard_mapped)
            munmap(launch_guard, launch_guard_bytes);
    }
#endif
#ifdef FX_EINK_NATIVE_STREAM_GUARD_BYTES
    if (native_stream_guard) {
        wipe_reserved_psram(native_stream_guard,
                            native_stream_guard_bytes);
        if (native_stream_guard_mapped)
            munmap(native_stream_guard, native_stream_guard_bytes);
    }
#endif
    if (transcript_fd >= 0)
        close(transcript_fd);
    if (transcript_created)
        unlink(transcript);
    free(response.data);
    free(session_id.data);
    free(line.data);
    free(prompt);
    return result;
}
