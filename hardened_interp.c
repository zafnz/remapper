/*
 * hardened_interp.c - a fake "interpreter" signed with hardened runtime.
 *
 * When invoked (typically via a #! shebang), writes a proof file using
 * $HOME-relative paths to verify that interposition works through
 * hardened shebang interpreters.
 *
 * Built and signed with hardened runtime:
 *   codesign --force -s - --options runtime build/hardened_interp
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "HOME not set\n");
        return 1;
    }

    char dir[1024], file[1024];
    snprintf(dir, sizeof(dir), "%s/.dummy-hardened-interp", home);
    snprintf(file, sizeof(file), "%s/.dummy-hardened-interp/proof.txt", home);

    printf("hardened_interp: mkdir %s\n", dir);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        printf("  mkdir failed: %s\n", strerror(errno));
        return 1;
    }
    printf("  ok\n");

    printf("hardened_interp: writing %s\n", file);
    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *msg = "hardened-interp-was-here\n";
        write(fd, msg, strlen(msg));
        if (argc > 1) {
            char buf[1024];
            int n = snprintf(buf, sizeof(buf), "script: %s\n", argv[1]);
            write(fd, buf, n);
        }
        close(fd);
        printf("  ok\n");
    } else {
        printf("  open failed: %s\n", strerror(errno));
        return 1;
    }

    printf("hardened_interp: done\n");
    return 0;
}
