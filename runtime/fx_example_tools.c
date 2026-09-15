// SPDX-License-Identifier: MIT
/* Deliberately narrow demo tools; not a general filesystem or shell service. */
#define _GNU_SOURCE
#include "fx_example_tools.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_LIMIT 4096U
#define ARG_LIMIT 8192U
#define TEST_LIMIT 8192U

#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    && defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
int fx_wamr_lend_test_contiguity_guard(void);
int fx_wamr_rearm_test_contiguity_guard(void);
#endif

static void whitespace(const uint8_t **p, const uint8_t *end)
{
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n'))
        ++*p;
}

/* The demo edits ASCII shell code. Reject NUL, non-ASCII, malformed escapes,
 * duplicate/unknown fields and trailing data instead of guessing JSON meaning. */
static int string_value(const uint8_t **p, const uint8_t *end, char *out, size_t cap)
{
    size_t length = 0;
    if (*p == end || *(*p)++ != '"') return -1;
    while (*p < end) {
        unsigned ch = *(*p)++;
        if (ch == '"') { out[length] = 0; return 0; }
        if (ch < 32 || ch > 126) return -1;
        if (ch == '\\') {
            if (*p == end) return -1;
            ch = *(*p)++;
            switch (ch) {
            case '"': case '\\': case '/': break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                unsigned value = 0, i;
                for (i = 0; i < 4; i++) {
                    unsigned digit;
                    if (*p == end) return -1;
                    digit = *(*p)++;
                    if (digit >= '0' && digit <= '9') digit -= '0';
                    else if (digit >= 'a' && digit <= 'f') digit = digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') digit = digit - 'A' + 10;
                    else return -1;
                    value = value * 16 + digit;
                }
                if (!value || value > 126 || (value < 32 && value != 9 && value != 10 && value != 13))
                    return -1;
                ch = value;
                break;
            }
            default: return -1;
            }
        }
        if (length + 1 >= cap) return -1;
        out[length++] = (char)ch;
    }
    return -1;
}

struct args { char path[32]; char content[FILE_LIMIT + 1]; unsigned fields; };

static int parse_args(const uint8_t *p, uint32_t length, struct args *args)
{
    const uint8_t *end = p + length;
    if (length > ARG_LIMIT) return -1;
    memset(args, 0, sizeof(*args));
    whitespace(&p, end);
    if (p == end || *p++ != '{') return -1;
    whitespace(&p, end);
    if (p < end && *p != '}') for (;;) {
        char key[16];
        unsigned field;
        if (string_value(&p, end, key, sizeof(key))) return -1;
        field = !strcmp(key, "path") ? 1U : !strcmp(key, "content") ? 2U : 0U;
        if (!field || (args->fields & field)) return -1;
        args->fields |= field;
        whitespace(&p, end);
        if (p == end || *p++ != ':') return -1;
        whitespace(&p, end);
        if (string_value(&p, end, field == 1 ? args->path : args->content,
                         field == 1 ? sizeof(args->path) : sizeof(args->content))) return -1;
        whitespace(&p, end);
        if (p == end) return -1;
        if (*p != ',') break;
        p++;
        whitespace(&p, end);
    }
    if (p == end || *p++ != '}') return -1;
    whitespace(&p, end);
    return p == end ? 0 : -1;
}

static int32_t reply(uint8_t *out, uint32_t cap, uint8_t *status,
                     int failed, const char *text)
{
    size_t length = strlen(text);
    *status = failed ? 1 : 0;
    if (length > cap) return -3;
    memcpy(out, text, length);
    return (int32_t)length;
}

static int checked_file(int directory, const char *name)
{
    struct stat st;
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1
        || st.st_size < 0 || (uintmax_t)st.st_size > FILE_LIMIT) {
        close(fd); return -1;
    }
    return fd;
}

static long long milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static __attribute__((noinline)) pid_t spawn_test(const char *helper, const char *jail, int pipes[2])
{
    char *const child_env[] = { "PATH=/usr/bin:/bin", "LANG=C", NULL };
    pid_t pid = vfork();
    if (pid == 0) {
        /* Only async-signal-safe setup before exec on shared NOMMU memory. */
        if (setpgid(0, 0) || dup2(pipes[1], 1) < 0 || dup2(pipes[1], 2) < 0)
            _exit(126);
        close(pipes[0]); close(pipes[1]); close(0);
        execle(helper, helper, jail, (char *)NULL, child_env);
        _exit(127);
    }
    return pid;
}

static int32_t run_tests(const char *jail, uint8_t *out, uint32_t cap, uint8_t *status)
{
    const char *helper = getenv("FX_EXAMPLE_TEST_HELPER");
    int pipes[2], child_status = 0, failed = 0, reaped = 0;
    size_t length = 0, limit = cap < TEST_LIMIT ? cap : TEST_LIMIT;
    long long deadline = milliseconds() + 10000;
    pid_t pid;
    if (!helper || !*helper) helper = "/usr/bin/fx-example-test";
    if (limit < 128 || pipe(pipes)) return -1;
    if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) || fcntl(pipes[1], F_SETFD, FD_CLOEXEC)) {
        close(pipes[0]); close(pipes[1]); return -1;
    }
#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    && defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
    /* vfork itself is allocation-free, but the helper's final /bin/sh exec
     * needs a 64 KiB mapping while its launcher still occupies about 32 KiB.
     * Lend both blocks reserved before WAMR startup and reacquire them only
     * after the child has exited. */
    if (fx_wamr_lend_test_contiguity_guard() < 0) {
        close(pipes[0]); close(pipes[1]); return -1;
    }
#endif
    pid = spawn_test(helper, jail, pipes);
    close(pipes[1]);
    if (pid < 0) {
        close(pipes[0]);
#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    && defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
        fx_wamr_rearm_test_contiguity_guard();
#endif
        return -1;
    }
    if (fcntl(pipes[0], F_SETFL, O_NONBLOCK)) failed = 1;
    while (!failed) {
        struct pollfd poll_fd = { .fd = pipes[0], .events = POLLIN };
        uint8_t buffer[512];
        ssize_t count;
        int ready;
        if (milliseconds() >= deadline) { failed = 1; break; }
        ready = poll(&poll_fd, 1, 50);
        if (ready < 0 && errno != EINTR) { failed = 1; break; }
        if (ready <= 0) continue;
        count = read(pipes[0], buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            failed = 1; break;
        }
        if (!count) break;
        if ((size_t)count > limit - 80 - length) { failed = 1; break; }
        memcpy(out + length, buffer, (size_t)count);
        length += (size_t)count;
    }
    close(pipes[0]);
    /* EOF is not proof of exit: a child can close stdout then loop forever. */
    while (!failed && !reaped) {
        pid_t waited = waitpid(pid, &child_status, WNOHANG);
        if (waited == pid) { reaped = 1; break; }
        if (waited < 0 && errno != EINTR) { failed = 1; break; }
        if (milliseconds() >= deadline) { failed = 1; break; }
        poll(NULL, 0, 20);
    }
    /* Kill any descendants even if the immediate child exited normally. */
    kill(-pid, SIGKILL);
    if (!reaped) {
        kill(pid, SIGKILL);
        while (waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {}
    }
#if defined(FX_WAMR_SHELL_CONTIGUITY_GUARD_BYTES) \
    && defined(FX_WAMR_TEST_CONTIGUITY_GUARD_BYTES)
    if (fx_wamr_rearm_test_contiguity_guard() < 0)
        failed = 1;
#endif
    if (failed) return reply(out, cap, status, 1, "test runner exceeded output/time limit or failed\n");
    {
        int code = WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
        int n = snprintf((char *)out + length, cap - length, "exit_code=%d\n", code);
        if (n < 0 || (size_t)n >= cap - length) return -3;
        *status = code ? 1 : 0;
        return (int32_t)(length + (size_t)n);
    }
}

int32_t fx_example_tool_call(const uint8_t *name, uint32_t name_len,
                            const uint8_t *arguments, uint32_t arguments_len,
                            uint8_t *out, uint32_t cap, uint8_t *status)
{
    const char *jail = getenv("FX_EXAMPLE_JAIL");
    struct args *args;
    int directory = -1, jail_fd = -1, fd = -1, tool;
    int32_t result;
    *status = 1;
    if (!jail || jail[0] != '/') return reply(out, cap, status, 1, "example tools disabled\n");
    tool = name_len == 12 && !memcmp(name, "read_project", 12) ? 1
         : name_len == 15 && !memcmp(name, "replace_battery", 15) ? 2
         : name_len == 9 && !memcmp(name, "run_tests", 9) ? 3 : 0;
    args = calloc(1, sizeof(*args));
    if (!args) return -1;
    if (!tool || parse_args(arguments, arguments_len, args)
        || args->fields != (tool == 1 ? 1U : tool == 2 ? 3U : 0U)) {
        free(args); return reply(out, cap, status, 1, "invalid tool or arguments\n");
    }
    /* Only known names go into diagnostics. Never print arguments or credentials. */
    fprintf(stderr, "fx-tools: %s begin\n", tool == 1 ? "read_project" : tool == 2 ? "replace_battery" : "run_tests");
    if (tool == 3) { free(args); return run_tests(jail, out, cap, status); }
    result = reply(out, cap, status, 1, "file denied or unavailable\n");
    if (strcmp(args->path, "battery.sh")
        && (tool == 2 || (strcmp(args->path, "SPEC.md") && strcmp(args->path, "test.sh")))) goto done;
    jail_fd = open(jail, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (jail_fd < 0) goto done;
    directory = openat(jail_fd, "project", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0 || (fd = checked_file(directory, args->path)) < 0) goto done;
    if (tool == 1) {
        ssize_t count;
        size_t used = 0, bound = cap < FILE_LIMIT ? cap : FILE_LIMIT;
        do {
            if (used == bound) {
                uint8_t extra;
                do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
                if (count != 0) { result = -3; goto done; }
                break;
            }
            count = read(fd, out + used, bound - used);
            if (count < 0) { if (errno == EINTR) continue; goto done; }
            used += count > 0 ? (size_t)count : 0;
        } while (count);
        *status = 0; result = (int32_t)used;
    } else {
        char temporary[64];
        int target_fd, failed = 0;
        size_t length = strlen(args->content), used = 0;
        if (!length) goto done;
        snprintf(temporary, sizeof(temporary), ".battery-%ld.tmp", (long)getpid());
        target_fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (target_fd < 0) goto done;
        while (used < length) {
            ssize_t count = write(target_fd, args->content + used, length - used);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { failed = 1; break; }
            used += (size_t)count;
        }
        if (fchmod(target_fd, 0644) || fsync(target_fd)) failed = 1;
        if (close(target_fd)) failed = 1;
        if (!failed && renameat(directory, temporary, directory, "battery.sh")) failed = 1;
        if (failed) { unlinkat(directory, temporary, 0); goto done; }
        result = reply(out, cap, status, 0, "wrote battery.sh\n");
    }
done:
    if (fd >= 0) close(fd);
    if (directory >= 0) close(directory);
    if (jail_fd >= 0) close(jail_fd);
    free(args);
    return result;
}
