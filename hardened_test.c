/*
 * hardened_test.c - a simple program that accesses ~/.dummy-test paths
 *
 * Built and signed with hardened runtime to simulate codex/claude.
 * When run under the interposer, paths should be redirected.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "HOME not set\n");
        return 1;
    }

    char dir[1024], file[1024];
    snprintf(dir, sizeof(dir), "%s/.dummy-hardened", home);
    snprintf(file, sizeof(file), "%s/.dummy-hardened/proof.txt", home);

    printf("hardened_test: mkdir %s\n", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        printf("  mkdir failed: %s\n", strerror(errno));
    } else {
        printf("  ok\n");
    }

    printf("hardened_test: writing %s\n", file);
    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *msg = "hardened-binary-was-here\n";
        write(fd, msg, strlen(msg));
        close(fd);
        printf("  ok\n");
    } else {
        printf("  open failed: %s\n", strerror(errno));
    }

    printf("hardened_test: stat %s\n", file);
    struct stat sb;
    if (stat(file, &sb) == 0) {
        printf("  size=%lld\n", (long long)sb.st_size);
    } else {
        printf("  stat failed: %s\n", strerror(errno));
    }

    printf("hardened_test: done\n");
    return 0;
}
