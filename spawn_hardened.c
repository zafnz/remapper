/*
 * spawn_hardened.c - simulates what codex's node wrapper does:
 *   posix_spawn a hardened binary as a child process.
 *
 * Modes:
 *   spawn_hardened <path>             — posix_spawn with full path (default)
 *   spawn_hardened --spawnp <name>    — posix_spawnp with bare filename (PATH lookup)
 *   spawn_hardened --execvp <name>    — execvp with bare filename (PATH lookup)
 *
 * Run via remapper:
 *   RMP_DEBUG_LOG=/tmp/spawn-debug.log \
 *     ./remapper /tmp/alt-test '~/.dummy*' -- ./spawn_hardened ./hardened_test
 *
 * The interposer should:
 *   1. Detect hardened_test is hardened
 *   2. Create a re-signed cached copy
 *   3. Spawn the cached copy instead
 *   4. The cached copy runs with the interposer, redirecting paths
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [--spawnp|--execvp] <path-or-name> [args...]\n", argv[0]);
        return 1;
    }

    int arg_idx = 1;
    enum { MODE_SPAWN, MODE_SPAWNP, MODE_EXECVP } mode = MODE_SPAWN;

    if (strcmp(argv[1], "--spawnp") == 0) {
        mode = MODE_SPAWNP;
        arg_idx = 2;
    } else if (strcmp(argv[1], "--execvp") == 0) {
        mode = MODE_EXECVP;
        arg_idx = 2;
    }

    if (arg_idx >= argc) {
        fprintf(stderr, "Error: no binary specified\n");
        return 1;
    }

    const char *target = argv[arg_idx];

    if (mode == MODE_EXECVP) {
        printf("spawn_hardened: execvp %s\n", target);
        execvp(target, &argv[arg_idx]);
        /* execvp only returns on error */
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid;
    int ret;

    if (mode == MODE_SPAWNP) {
        printf("spawn_hardened: posix_spawnp %s\n", target);
        ret = posix_spawnp(&pid, target, NULL, NULL, &argv[arg_idx], environ);
    } else {
        printf("spawn_hardened: posix_spawn %s\n", target);
        ret = posix_spawn(&pid, target, NULL, NULL, &argv[arg_idx], environ);
    }

    if (ret != 0) {
        fprintf(stderr, "spawn failed: %s\n", strerror(ret));
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    printf("spawn_hardened: child exited with %d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
