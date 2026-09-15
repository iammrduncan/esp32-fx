// SPDX-License-Identifier: MIT
/* Privilege-dropping launcher for the fixed, builtin-only shell test harness.
 * Never install setuid. The operator provides a root-owned, prepared jail. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(void) { perror("fx-example-test"); _exit(125); }
static void limit(int resource, rlim_t maximum)
{
    struct rlimit value = { maximum, maximum };
    if (setrlimit(resource, &value)) fail();
}

int main(int argc, char **argv)
{
    struct stat st;
    int fd;
    char *const env[] = { "PATH=/bin", "HOME=/project", "LANG=C", NULL };
    if (argc != 2 || geteuid() != 0 || argv[1][0] != '/') {
        fputs("fx-example-test: requires root and an absolute prepared jail\n", stderr);
        return 125;
    }
    /* Discard every inherited descriptor except the captured stdout/stderr. */
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3U, ~0U, 0U) != 0)
#endif
    {
        struct rlimit descriptors;
        unsigned long i;
        if (getrlimit(RLIMIT_NOFILE, &descriptors)
            || descriptors.rlim_max == RLIM_INFINITY
            || descriptors.rlim_max > INT_MAX) fail();
        for (i = 3; i < descriptors.rlim_max; i++) close((int)i);
    }
    close(0);
    fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) || st.st_uid != 0 || (st.st_mode & 0022)) fail();
    if (fchdir(fd) || chroot(".") || chdir("/project")) fail();
    close(fd);
    if (stat(".", &st) || st.st_uid != 0 || (st.st_mode & 0022)) fail();
    limit(RLIMIT_CORE, 0);
    limit(RLIMIT_FSIZE, 0);
    limit(RLIMIT_CPU, 2);
    limit(RLIMIT_NOFILE, 16);
    limit(RLIMIT_NPROC, 1);
    if (setgroups(0, NULL) || setgid(65534) || setuid(65534)) fail();
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) fail();
    /* No shell supplied by the model; /bin/sh is the prepared system dash.
     * No /proc, devices, network utilities, keys, or writable directories. */
    execle("/bin/sh", "sh", "./test.sh", (char *)NULL, env);
    fail();
    return 125;
}
