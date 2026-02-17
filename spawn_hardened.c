/*
 * spawn_hardened.c - simulates what codex's node wrapper does:
 *   posix_spawn a hardened binary as a child process.
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
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path-to-hardened-binary> [args...]\n", argv[0]);
        return 1;
    }

    printf("spawn_hardened: spawning %s\n", argv[1]);

    pid_t pid;
    int ret = posix_spawn(&pid, argv[1], NULL, NULL, &argv[1], environ);
    if (ret != 0) {
        fprintf(stderr, "posix_spawn failed: %s\n", strerror(ret));
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    printf("spawn_hardened: child exited with %d\n", WEXITSTATUS(status));
    return WEXITSTATUS(status);
}
