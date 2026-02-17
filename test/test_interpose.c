/*
 * test_interpose.c - exercise all interposed filesystem functions
 *
 * Run via remapper:
 *   RMP_DEBUG_LOG=/tmp/rmp.log ./remapper /tmp/alt-test "$HOME/.dummy*" -- ./test_interpose
 *
 * Leaves artifacts in the target dir for verify_test_interpose to check.
 *
 * Expected final state in <target>/:
 *   .dummy-test/
 *     open.txt          "open-content\n"          mode 0644
 *     creat.txt         "creat-content\n"         mode 0644
 *     openat.txt        "openat-content\n"        mode 0644
 *     renamed.txt       "rename-me\n"             (was pre-rename.txt)
 *     renamed2.txt      "renameat-me\n"           (was pre-renameat.txt)
 *     link-target.txt   "link-target\n"           mode 0644
 *     hardlink.txt      hard link to link-target.txt
 *     hardlink2.txt     hard link via linkat to link-target.txt
 *     symlink.lnk       symlink → "link-target.txt"
 *     symlinkat.lnk     symlink → "link-target.txt"
 *     truncated.txt     "hello"                   (5 bytes, was longer)
 *     chmod.txt         "chmod\n"                 mode 0600
 *     fchmodat.txt      "fchmodat\n"              mode 0400
 *     subdir/
 *       mkdirat.txt     "mkdirat-content\n"
 *     chdir-proof.txt   "chdir-ok\n"              (created after chdir)
 *   .dummy.txt          "toplevel\n"              (tests glob: .dummy* ≠ .dummy-test*)
 *
 * Should NOT exist:
 *   .dummy-test/pre-rename.txt
 *   .dummy-test/pre-renameat.txt
 *   .dummy-test/to-unlink.txt
 *   .dummy-test/to-unlinkat.txt
 *   .dummy-test/empty-subdir/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

static int failures = 0;
static int passes   = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("  PASS: %s\n", label); passes++; } \
    else      { printf("  FAIL: %s (errno=%d: %s)\n", label, errno, strerror(errno)); failures++; } \
    errno = 0; \
} while(0)

static void write_to_fd(int fd, const char *s) {
    __attribute__((unused)) ssize_t ignored = write(fd, s, strlen(s));
}

int main(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "HOME not set\n");
        return 1;
    }

    const char *target = getenv("RMP_TARGET");
    if (!target) {
        fprintf(stderr,
            "Run via remapper. Example:\n"
            "  RMP_DEBUG_LOG=/tmp/rmp.log ./remapper /tmp/alt-test '%s/.dummy*' -- ./test_interpose\n",
            home);
        return 1;
    }

    /* Base paths (as the app sees them — will be rewritten by interposer) */
    char base[1024];
    snprintf(base, sizeof(base), "%s/.dummy-test", home);

    char path[1088];  /* scratch buffer for building paths (base + suffix) */

    printf("=== Exercising all interposed functions ===\n");
    printf("HOME:   %s\n", home);
    printf("TARGET: %s\n\n", target);

    /* ================================================================ */
    /*  mkdir                                                           */
    /* ================================================================ */
    printf("[mkdir]\n");
    errno = 0;
    CHECK("mkdir .dummy-test", mkdir(base, 0755) == 0);

    /* ================================================================ */
    /*  open (O_CREAT | O_WRONLY)                                       */
    /* ================================================================ */
    printf("\n[open]\n");
    snprintf(path, sizeof(path), "%s/open.txt", base);
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("open O_CREAT", fd >= 0);
    if (fd >= 0) { write_to_fd(fd, "open-content\n"); close(fd); }

    /* ================================================================ */
    /*  creat                                                           */
    /* ================================================================ */
    printf("\n[creat]\n");
    snprintf(path, sizeof(path), "%s/creat.txt", base);
    fd = creat(path, 0644);
    CHECK("creat", fd >= 0);
    if (fd >= 0) { write_to_fd(fd, "creat-content\n"); close(fd); }

    /* ================================================================ */
    /*  openat (absolute path, AT_FDCWD)                                */
    /* ================================================================ */
    printf("\n[openat]\n");
    snprintf(path, sizeof(path), "%s/openat.txt", base);
    fd = openat(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("openat O_CREAT", fd >= 0);
    if (fd >= 0) { write_to_fd(fd, "openat-content\n"); close(fd); }

    /* ================================================================ */
    /*  stat                                                            */
    /* ================================================================ */
    printf("\n[stat]\n");
    snprintf(path, sizeof(path), "%s/open.txt", base);
    struct stat sb;
    CHECK("stat open.txt", stat(path, &sb) == 0);
    CHECK("stat size=13", sb.st_size == 13);

    /* ================================================================ */
    /*  lstat                                                           */
    /* ================================================================ */
    printf("\n[lstat]\n");
    CHECK("lstat open.txt", lstat(path, &sb) == 0);
    CHECK("lstat is regular", S_ISREG(sb.st_mode));

    /* ================================================================ */
    /*  fstatat                                                         */
    /* ================================================================ */
    printf("\n[fstatat]\n");
    CHECK("fstatat open.txt", fstatat(AT_FDCWD, path, &sb, 0) == 0);
    CHECK("fstatat size=13", sb.st_size == 13);

    /* ================================================================ */
    /*  access                                                          */
    /* ================================================================ */
    printf("\n[access]\n");
    CHECK("access F_OK", access(path, F_OK) == 0);
    CHECK("access R_OK", access(path, R_OK) == 0);

    /* ================================================================ */
    /*  faccessat                                                       */
    /* ================================================================ */
    printf("\n[faccessat]\n");
    CHECK("faccessat F_OK", faccessat(AT_FDCWD, path, F_OK, 0) == 0);

    /* ================================================================ */
    /*  chmod                                                           */
    /* ================================================================ */
    printf("\n[chmod]\n");
    snprintf(path, sizeof(path), "%s/chmod.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "chmod\n"); close(fd); }
    CHECK("chmod 0600", chmod(path, 0600) == 0);
    stat(path, &sb);
    CHECK("chmod verified", (sb.st_mode & 0777) == 0600);

    /* ================================================================ */
    /*  fchmodat                                                        */
    /* ================================================================ */
    printf("\n[fchmodat]\n");
    snprintf(path, sizeof(path), "%s/fchmodat.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "fchmodat\n"); close(fd); }
    CHECK("fchmodat 0400", fchmodat(AT_FDCWD, path, 0400, 0) == 0);
    stat(path, &sb);
    CHECK("fchmodat verified", (sb.st_mode & 0777) == 0400);

    /* ================================================================ */
    /*  chown / lchown / fchownat                                       */
    /*  (chown to own uid/gid — should succeed without root)            */
    /* ================================================================ */
    printf("\n[chown/lchown/fchownat]\n");
    snprintf(path, sizeof(path), "%s/open.txt", base);
    uid_t uid = getuid();
    gid_t gid = getgid();
    CHECK("chown", chown(path, uid, gid) == 0);
    CHECK("lchown", lchown(path, uid, gid) == 0);
    CHECK("fchownat", fchownat(AT_FDCWD, path, uid, gid, 0) == 0);

    /* ================================================================ */
    /*  rename                                                          */
    /* ================================================================ */
    printf("\n[rename]\n");
    char oldp[1088], newp[1088];
    snprintf(oldp, sizeof(oldp), "%s/pre-rename.txt", base);
    snprintf(newp, sizeof(newp), "%s/renamed.txt", base);
    fd = open(oldp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "rename-me\n"); close(fd); }
    CHECK("rename", rename(oldp, newp) == 0);
    CHECK("old gone", access(oldp, F_OK) != 0);
    CHECK("new exists", access(newp, F_OK) == 0);

    /* ================================================================ */
    /*  renameat                                                        */
    /* ================================================================ */
    printf("\n[renameat]\n");
    snprintf(oldp, sizeof(oldp), "%s/pre-renameat.txt", base);
    snprintf(newp, sizeof(newp), "%s/renamed2.txt", base);
    fd = open(oldp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "renameat-me\n"); close(fd); }
    CHECK("renameat", renameat(AT_FDCWD, oldp, AT_FDCWD, newp) == 0);
    CHECK("old gone", access(oldp, F_OK) != 0);
    CHECK("new exists", access(newp, F_OK) == 0);

    /* ================================================================ */
    /*  symlink                                                         */
    /* ================================================================ */
    printf("\n[symlink]\n");
    snprintf(path, sizeof(path), "%s/link-target.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "link-target\n"); close(fd); }

    char lnk[1088];
    snprintf(lnk, sizeof(lnk), "%s/symlink.lnk", base);
    CHECK("symlink", symlink("link-target.txt", lnk) == 0);

    /* ================================================================ */
    /*  symlinkat                                                       */
    /* ================================================================ */
    printf("\n[symlinkat]\n");
    snprintf(lnk, sizeof(lnk), "%s/symlinkat.lnk", base);
    CHECK("symlinkat", symlinkat("link-target.txt", AT_FDCWD, lnk) == 0);

    /* ================================================================ */
    /*  readlink                                                        */
    /* ================================================================ */
    printf("\n[readlink]\n");
    snprintf(lnk, sizeof(lnk), "%s/symlink.lnk", base);
    char rlbuf[1024];
    ssize_t rllen = readlink(lnk, rlbuf, sizeof(rlbuf) - 1);
    CHECK("readlink >= 0", rllen >= 0);
    if (rllen >= 0) {
        rlbuf[rllen] = '\0';
        CHECK("readlink target correct", strcmp(rlbuf, "link-target.txt") == 0);
    }

    /* ================================================================ */
    /*  readlinkat                                                      */
    /* ================================================================ */
    printf("\n[readlinkat]\n");
    snprintf(lnk, sizeof(lnk), "%s/symlinkat.lnk", base);
    rllen = readlinkat(AT_FDCWD, lnk, rlbuf, sizeof(rlbuf) - 1);
    CHECK("readlinkat >= 0", rllen >= 0);
    if (rllen >= 0) {
        rlbuf[rllen] = '\0';
        CHECK("readlinkat target correct", strcmp(rlbuf, "link-target.txt") == 0);
    }

    /* ================================================================ */
    /*  link                                                            */
    /* ================================================================ */
    printf("\n[link]\n");
    snprintf(path, sizeof(path), "%s/link-target.txt", base);
    snprintf(lnk, sizeof(lnk), "%s/hardlink.txt", base);
    CHECK("link", link(path, lnk) == 0);

    /* ================================================================ */
    /*  linkat                                                          */
    /* ================================================================ */
    printf("\n[linkat]\n");
    snprintf(lnk, sizeof(lnk), "%s/hardlink2.txt", base);
    CHECK("linkat", linkat(AT_FDCWD, path, AT_FDCWD, lnk, 0) == 0);

    /* ================================================================ */
    /*  truncate                                                        */
    /* ================================================================ */
    printf("\n[truncate]\n");
    snprintf(path, sizeof(path), "%s/truncated.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "hello-world-truncate\n"); close(fd); }
    CHECK("truncate to 5", truncate(path, 5) == 0);
    stat(path, &sb);
    CHECK("truncate size=5", sb.st_size == 5);

    /* ================================================================ */
    /*  realpath                                                        */
    /* ================================================================ */
    printf("\n[realpath]\n");
    snprintf(path, sizeof(path), "%s/open.txt", base);
    char resolved[PATH_MAX];
    char *rp = realpath(path, resolved);
    CHECK("realpath non-null", rp != NULL);
    if (rp) {
        /* Should resolve to target dir, not home */
        CHECK("realpath points to target", strstr(resolved, target) != NULL);
    }

    /* ================================================================ */
    /*  opendir                                                         */
    /* ================================================================ */
    printf("\n[opendir]\n");
    DIR *d = opendir(base);
    CHECK("opendir", d != NULL);
    if (d) {
        int count = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] != '.') count++;
        }
        CHECK("opendir found files", count > 0);
        closedir(d);
    }

    /* ================================================================ */
    /*  mkdirat                                                         */
    /* ================================================================ */
    printf("\n[mkdirat]\n");
    snprintf(path, sizeof(path), "%s/subdir", base);
    CHECK("mkdirat", mkdirat(AT_FDCWD, path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/subdir/mkdirat.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "mkdirat-content\n"); close(fd); }
    CHECK("file in mkdirat dir", access(path, F_OK) == 0);

    /* ================================================================ */
    /*  chdir                                                           */
    /* ================================================================ */
    printf("\n[chdir]\n");
    char orig_cwd[PATH_MAX];
    if (!getcwd(orig_cwd, sizeof(orig_cwd))) orig_cwd[0] = '\0';
    CHECK("chdir into .dummy-test", chdir(base) == 0);
    /* Create a file via relative path to prove we're in the right dir */
    fd = open("chdir-proof.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "chdir-ok\n"); close(fd); }
    CHECK("chdir-proof.txt created", access("chdir-proof.txt", F_OK) == 0);
    __attribute__((unused)) int ignored = chdir(orig_cwd);  /* restore */

    /* ================================================================ */
    /*  unlink                                                          */
    /* ================================================================ */
    printf("\n[unlink]\n");
    snprintf(path, sizeof(path), "%s/to-unlink.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "delete-me\n"); close(fd); }
    CHECK("unlink", unlink(path) == 0);
    CHECK("unlink verified gone", access(path, F_OK) != 0);

    /* ================================================================ */
    /*  unlinkat                                                        */
    /* ================================================================ */
    printf("\n[unlinkat]\n");
    snprintf(path, sizeof(path), "%s/to-unlinkat.txt", base);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write_to_fd(fd, "delete-me-too\n"); close(fd); }
    CHECK("unlinkat", unlinkat(AT_FDCWD, path, 0) == 0);
    CHECK("unlinkat verified gone", access(path, F_OK) != 0);

    /* ================================================================ */
    /*  rmdir                                                           */
    /* ================================================================ */
    printf("\n[rmdir]\n");
    snprintf(path, sizeof(path), "%s/empty-subdir", base);
    mkdir(path, 0755);
    CHECK("rmdir", rmdir(path) == 0);
    CHECK("rmdir verified gone", access(path, F_OK) != 0);

    /* ================================================================ */
    /*  .dummy.txt — verify glob matches .dummy* not just .dummy-test*  */
    /* ================================================================ */
    printf("\n[glob coverage: .dummy.txt]\n");
    snprintf(path, sizeof(path), "%s/.dummy.txt", home);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK("open ~/.dummy.txt", fd >= 0);
    if (fd >= 0) { write_to_fd(fd, "toplevel\n"); close(fd); }

    /* ================================================================ */
    /*  Summary                                                         */
    /* ================================================================ */
    printf("\n=== %s: %d passed, %d failed ===\n",
           failures == 0 ? "ALL PASSED" : "SOME FAILED", passes, failures);
    printf("Artifacts left in target for verify_test_interpose.\n");

    return failures ? 1 : 0;
}
