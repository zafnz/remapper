/*
 * verify_test_interpose.c - verify results WITHOUT the interposer
 *
 * Run AFTER test_interpose, WITHOUT DYLD_INSERT_LIBRARIES:
 *   ./verify_test_interpose /tmp/alt-test "$HOME"
 *
 * Checks:
 *   1. Expected files exist in <target> with correct content/perms
 *   2. Deleted/renamed files do NOT exist in <target>
 *   3. Nothing leaked to the real <home> directory
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int passes   = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("  PASS: %s\n", label); passes++; } \
    else      { printf("  FAIL: %s (errno=%d: %s)\n", label, errno, strerror(errno)); failures++; } \
    errno = 0; \
} while(0)

static int file_contains(const char *path, const char *expected) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, expected) == 0;
}

static int path_exists(const char *path) {
    struct stat sb;
    return lstat(path, &sb) == 0;
}

static int is_symlink_to(const char *path, const char *expected_target) {
    char buf[1024];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, expected_target) == 0;
}

static mode_t file_mode(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return 0;
    return sb.st_mode & 0777;
}

static off_t file_size(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return -1;
    return sb.st_size;
}

static ino_t file_inode(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return 0;
    return sb.st_ino;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <target-dir> <home-dir>\n", prog);
    fprintf(stderr, "  Verifies test_interpose results on the real filesystem.\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) usage(argv[0]);

    const char *target = argv[1];
    const char *home   = argv[2];

    char path[1024];

    printf("=== Verifying interpose results (no interposer active) ===\n");
    printf("TARGET: %s\n", target);
    printf("HOME:   %s\n\n", home);

    /* ================================================================ */
    /*  Files that SHOULD exist in target                               */
    /* ================================================================ */
    printf("[target: expected files]\n");

    snprintf(path, sizeof(path), "%s/.dummy-test", target);
    CHECK(".dummy-test/ exists", path_exists(path));

    /* open.txt */
    snprintf(path, sizeof(path), "%s/.dummy-test/open.txt", target);
    CHECK("open.txt exists", path_exists(path));
    CHECK("open.txt content", file_contains(path, "open-content\n"));

    /* creat.txt */
    snprintf(path, sizeof(path), "%s/.dummy-test/creat.txt", target);
    CHECK("creat.txt exists", path_exists(path));
    CHECK("creat.txt content", file_contains(path, "creat-content\n"));

    /* openat.txt */
    snprintf(path, sizeof(path), "%s/.dummy-test/openat.txt", target);
    CHECK("openat.txt exists", path_exists(path));
    CHECK("openat.txt content", file_contains(path, "openat-content\n"));

    /* renamed.txt (was pre-rename.txt) */
    snprintf(path, sizeof(path), "%s/.dummy-test/renamed.txt", target);
    CHECK("renamed.txt exists", path_exists(path));
    CHECK("renamed.txt content", file_contains(path, "rename-me\n"));

    /* renamed2.txt (was pre-renameat.txt) */
    snprintf(path, sizeof(path), "%s/.dummy-test/renamed2.txt", target);
    CHECK("renamed2.txt exists", path_exists(path));
    CHECK("renamed2.txt content", file_contains(path, "renameat-me\n"));

    /* link-target.txt */
    snprintf(path, sizeof(path), "%s/.dummy-test/link-target.txt", target);
    CHECK("link-target.txt exists", path_exists(path));
    CHECK("link-target.txt content", file_contains(path, "link-target\n"));

    /* hardlink.txt — hard link, same inode as link-target.txt */
    printf("\n[target: hard links]\n");
    char link_target_path[1024];
    snprintf(link_target_path, sizeof(link_target_path), "%s/.dummy-test/link-target.txt", target);
    ino_t target_inode = file_inode(link_target_path);

    snprintf(path, sizeof(path), "%s/.dummy-test/hardlink.txt", target);
    CHECK("hardlink.txt exists", path_exists(path));
    CHECK("hardlink.txt same inode", file_inode(path) == target_inode);

    /* hardlink2.txt — via linkat */
    snprintf(path, sizeof(path), "%s/.dummy-test/hardlink2.txt", target);
    CHECK("hardlink2.txt exists", path_exists(path));
    CHECK("hardlink2.txt same inode", file_inode(path) == target_inode);

    /* symlinks */
    printf("\n[target: symlinks]\n");
    snprintf(path, sizeof(path), "%s/.dummy-test/symlink.lnk", target);
    CHECK("symlink.lnk exists", path_exists(path));
    CHECK("symlink.lnk target", is_symlink_to(path, "link-target.txt"));

    snprintf(path, sizeof(path), "%s/.dummy-test/symlinkat.lnk", target);
    CHECK("symlinkat.lnk exists", path_exists(path));
    CHECK("symlinkat.lnk target", is_symlink_to(path, "link-target.txt"));

    /* truncated.txt — should be exactly 5 bytes: "hello" */
    printf("\n[target: truncated file]\n");
    snprintf(path, sizeof(path), "%s/.dummy-test/truncated.txt", target);
    CHECK("truncated.txt exists", path_exists(path));
    CHECK("truncated.txt size=5", file_size(path) == 5);
    CHECK("truncated.txt content", file_contains(path, "hello"));

    /* chmod.txt — mode 0600 */
    printf("\n[target: permissions]\n");
    snprintf(path, sizeof(path), "%s/.dummy-test/chmod.txt", target);
    CHECK("chmod.txt exists", path_exists(path));
    CHECK("chmod.txt content", file_contains(path, "chmod\n"));
    CHECK("chmod.txt mode=0600", file_mode(path) == 0600);

    /* fchmodat.txt — mode 0400 */
    snprintf(path, sizeof(path), "%s/.dummy-test/fchmodat.txt", target);
    CHECK("fchmodat.txt exists", path_exists(path));
    CHECK("fchmodat.txt content", file_contains(path, "fchmodat\n"));
    CHECK("fchmodat.txt mode=0400", file_mode(path) == 0400);

    /* subdir/mkdirat.txt */
    printf("\n[target: mkdirat subdir]\n");
    snprintf(path, sizeof(path), "%s/.dummy-test/subdir", target);
    CHECK("subdir/ exists", path_exists(path));
    snprintf(path, sizeof(path), "%s/.dummy-test/subdir/mkdirat.txt", target);
    CHECK("mkdirat.txt exists", path_exists(path));
    CHECK("mkdirat.txt content", file_contains(path, "mkdirat-content\n"));

    /* chdir-proof.txt */
    printf("\n[target: chdir proof]\n");
    snprintf(path, sizeof(path), "%s/.dummy-test/chdir-proof.txt", target);
    CHECK("chdir-proof.txt exists", path_exists(path));
    CHECK("chdir-proof.txt content", file_contains(path, "chdir-ok\n"));

    /* .dummy.txt (top-level glob match) */
    printf("\n[target: top-level glob]\n");
    snprintf(path, sizeof(path), "%s/.dummy.txt", target);
    CHECK(".dummy.txt exists", path_exists(path));
    CHECK(".dummy.txt content", file_contains(path, "toplevel\n"));

    /* ================================================================ */
    /*  Files that should NOT exist in target (deleted/renamed away)     */
    /* ================================================================ */
    printf("\n[target: should NOT exist]\n");

    snprintf(path, sizeof(path), "%s/.dummy-test/pre-rename.txt", target);
    CHECK("pre-rename.txt gone", !path_exists(path));

    snprintf(path, sizeof(path), "%s/.dummy-test/pre-renameat.txt", target);
    CHECK("pre-renameat.txt gone", !path_exists(path));

    snprintf(path, sizeof(path), "%s/.dummy-test/to-unlink.txt", target);
    CHECK("to-unlink.txt gone", !path_exists(path));

    snprintf(path, sizeof(path), "%s/.dummy-test/to-unlinkat.txt", target);
    CHECK("to-unlinkat.txt gone", !path_exists(path));

    snprintf(path, sizeof(path), "%s/.dummy-test/empty-subdir", target);
    CHECK("empty-subdir/ gone", !path_exists(path));

    /* ================================================================ */
    /*  Nothing should have leaked to real home                         */
    /* ================================================================ */
    printf("\n[home: no leaked files]\n");

    snprintf(path, sizeof(path), "%s/.dummy-test", home);
    CHECK("~/.dummy-test/ not in home", !path_exists(path));

    snprintf(path, sizeof(path), "%s/.dummy.txt", home);
    CHECK("~/.dummy.txt not in home", !path_exists(path));

    /* ================================================================ */
    /*  Summary                                                         */
    /* ================================================================ */
    printf("\n=== %s: %d passed, %d failed ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", passes, failures);

    return failures ? 1 : 0;
}
