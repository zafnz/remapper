/* rmp_shared.c - shared utilities for remapper
 *
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
*/
#include "rmp_shared.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <copyfile.h>
#endif

#ifdef __APPLE__
// Thread-safe home directory lookup: try $HOME, fall back to getpwuid_r.
static const char *get_home_dir(char *buf, size_t bufsize) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;

    struct passwd pw, *result = NULL;
    if (getpwuid_r(geteuid(), &pw, buf, bufsize, &result) == 0 && result)
        return result->pw_dir;

    return NULL;
}
#endif


// Resolve a bare filename via $PATH. If `file` contains '/', copy it
// directly. Otherwise walk $PATH looking for an executable match.
// Returns 1 on success (result in `out`), 0 on failure.
int resolve_in_path(const char *file, char *out, size_t outsize) {
    if (!file || !file[0]) return 0;

    // If it contains '/', treat as a path — just copy it
    if (strchr(file, '/')) {
        if (strlen(file) >= outsize) return 0;
        strcpy(out, file);
        return 1;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char *path_copy = strdup(path_env);
    if (!path_copy) return 0;

    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir) {
        size_t needed = strlen(dir) + 1 + strlen(file) + 1;
        if (needed <= outsize) {
            snprintf(out, outsize, "%s/%s", dir, file);
            if (access(out, X_OK) == 0) {
                free(path_copy);
                return 1;
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_copy);
    return 0;
}


/*** Safe pipe-based process spawning ************/

rmp_pipe_t rmp_pipe_open(const char *path, char *const argv[]) {
    rmp_pipe_t proc = { NULL, -1 };

    int pipefd[2];
    if (pipe(pipefd) != 0) return proc;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return proc;
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr to pipe
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(path, argv);
        // exec failed — write error to stderr (which is the pipe)
        int e = errno;
        const char *msg = "execv failed: ";
        write(STDERR_FILENO, msg, strlen(msg));
        const char *err = strerror(e);
        write(STDERR_FILENO, err, strlen(err));
        write(STDERR_FILENO, "\n", 1);
        _exit(127);
    }

    // Parent: read end
    close(pipefd[1]);
    proc.fp = fdopen(pipefd[0], "r");
    if (!proc.fp) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return proc;
    }
    proc.pid = pid;
    return proc;
}

int rmp_pipe_close(rmp_pipe_t *proc) {
    if (!proc || proc->pid < 0) return -1;

    if (proc->fp) {
        fclose(proc->fp);
        proc->fp = NULL;
    }

    int status;
    if (waitpid(proc->pid, &status, 0) < 0) return -1;
    proc->pid = -1;

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// Atomic counter for unique temp file names (thread-safe)
static _Atomic int g_tmp_seq = 0;

/*** rmp_mkdirs **********************************/

void rmp_mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

// Write data to a temp file and atomically rename into place.
// Avoids partial reads if two processes race.
static int atomic_write_file(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[PATH_MAX];
    int seq = atomic_fetch_add(&g_tmp_seq, 1);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%d", path, getpid(), seq);

    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -1;

    ssize_t written = write(fd, data, len);
    close(fd);

    if (written != (ssize_t)len) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp); // another process won the race — that's fine
    }
    return 0;
}

/*** macOS-only: hardened binary cache ************/
#ifdef __APPLE__

/*** Entitlements plist **************************/

static const char *ENTITLEMENTS_PLIST =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "\t<key>com.apple.security.cs.allow-dyld-environment-variables</key>\n"
    "\t<true/>\n"
    "\t<key>com.apple.security.cs.disable-library-validation</key>\n"
    "\t<true/>\n"
    "</dict>\n"
    "</plist>\n";

/*** rmp_ctx_init ********************************/

void rmp_ctx_init(rmp_ctx_t *ctx, const char *config_dir,
                  const char *cache_dir, FILE *debug_fp) {
    char pwbuf[1024];
    const char *home = get_home_dir(pwbuf, sizeof(pwbuf));

    ctx->debug_fp = debug_fp;

    // Config dir
    if (config_dir && config_dir[0]) {
        strncpy(ctx->config_dir, config_dir, sizeof(ctx->config_dir) - 1);
    } else if (home) {
        snprintf(ctx->config_dir, sizeof(ctx->config_dir), "%s/.remapper", home);
    } else {
        snprintf(ctx->config_dir, sizeof(ctx->config_dir), "/tmp/.remapper");
    }
    ctx->config_dir[sizeof(ctx->config_dir) - 1] = '\0';

    // Cache dir
    if (cache_dir && cache_dir[0]) {
        strncpy(ctx->cache_dir, cache_dir, sizeof(ctx->cache_dir) - 1);
    } else {
        snprintf(ctx->cache_dir, sizeof(ctx->cache_dir), "%s/cache", ctx->config_dir);
    }
    ctx->cache_dir[sizeof(ctx->cache_dir) - 1] = '\0';

    // Entitlements path
    snprintf(ctx->entitlements_path, sizeof(ctx->entitlements_path),
             "%s/entitlements.plist", ctx->config_dir);

    // Create directories
    rmp_mkdirs(ctx->config_dir, 0755);
    rmp_mkdirs(ctx->cache_dir, 0755);

    // Write entitlements plist atomically if absent
    if (access(ctx->entitlements_path, R_OK) != 0) {
        atomic_write_file(ctx->entitlements_path, ENTITLEMENTS_PLIST,
                          strlen(ENTITLEMENTS_PLIST), 0644);
    }

    // Resolve codesign once
    if (!resolve_in_path("codesign", ctx->codesign_path,
                         sizeof(ctx->codesign_path))) {
        ctx->codesign_path[0] = '\0';
    }
}

/*** rmp_is_hardened *****************************/
// Checks if the binary at `path` has hardened runtime without the
// allow-dyld-environment-variables entitlement, by invoking `codesign`.
int rmp_is_hardened(const rmp_ctx_t *ctx, const char *path) {
    const char *codesign = ctx->codesign_path;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    uint32_t magic;
    if (read(fd, &magic, sizeof(magic)) != sizeof(magic)) {
        close(fd);
        return 0;
    }
    close(fd);

    if (magic != MH_MAGIC_64 && magic != MH_CIGAM_64 &&
        magic != FAT_MAGIC && magic != FAT_CIGAM) {
        return 0;
    }

    if (codesign == NULL || codesign[0] == '\0') {
        // If we don't have codesign then we can't resign the binary, so
        // fallback to treating it as hardened to avoid silently failing to insert the dylib.
        return 1;
    }

    // Check for hardened runtime via codesign
    char *cs_argv[] = {"codesign", "-dvvv", (char *)path, NULL};
    rmp_pipe_t proc = rmp_pipe_open(codesign, cs_argv);
    if (!proc.fp) return 0;

    int has_runtime = 0;
    char line[1024];
    while (fgets(line, sizeof(line), proc.fp))
        if (strstr(line, "runtime")) has_runtime = 1;
    rmp_pipe_close(&proc);

    if (!has_runtime) return 0;

    // Check entitlements
    char *ent_argv[] = {"codesign", "-d", "--entitlements", "-",
                        (char *)path, NULL};
    rmp_pipe_t proc2 = rmp_pipe_open(codesign, ent_argv);
    if (!proc2.fp) return 1;

    int has_dyld_ent = 0;
    while (fgets(line, sizeof(line), proc2.fp))
        if (strstr(line, "allow-dyld-environment-variables"))
            has_dyld_ent = 1;
    rmp_pipe_close(&proc2);

    return has_dyld_ent ? 0 : 1;
}

/*** rmp_cache_path ******************************/

void rmp_cache_path(const char *cache_dir, const char *original,
                    char *out, size_t outsize) {
    snprintf(out, outsize, "%s%s", cache_dir, original);
}

/*** rmp_cache_valid *****************************/

int rmp_cache_valid(const char *cached, time_t orig_mtime, off_t orig_size) {
    struct stat sb;
    if (stat(cached, &sb) != 0) return 0;

    char meta[PATH_MAX];
    snprintf(meta, sizeof(meta), "%s.meta", cached);
    int fd = open(meta, O_RDONLY);
    if (fd < 0) return 0;

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    time_t cached_mtime;
    long long cached_size;
    if (sscanf(buf, "%ld %lld", &cached_mtime, &cached_size) != 2)
        return 0;

    return (cached_mtime == orig_mtime && (off_t)cached_size == orig_size);
}

/*** rmp_cache_create ****************************/

int rmp_cache_create(rmp_ctx_t *ctx, const char *original,
                     const char *cached, time_t mtime, off_t size) {
    // Create parent directories
    char parent[PATH_MAX];
    strncpy(parent, cached, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *last_slash = strrchr(parent, '/');
    if (last_slash) {
        *last_slash = '\0';
        rmp_mkdirs(parent, 0755);
    }

    // Unique temp file: pid + atomic sequence number (thread-safe)
    char tmp[PATH_MAX];
    int seq = atomic_fetch_add(&g_tmp_seq, 1);
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%d", cached, getpid(), seq);

    if (copyfile(original, tmp, NULL, COPYFILE_ALL) != 0) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] cache: copyfile failed for %s: %s\n",
                    original, strerror(errno));
            fflush(ctx->debug_fp);
        }
        unlink(tmp);
        return -1;
    }

    chmod(tmp, 0755);

    // Re-sign with entitlements
    if (ctx->codesign_path[0] == '\0') {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] cache: codesign not available\n");
            fflush(ctx->debug_fp);
        }
        unlink(tmp);
        return -1;
    }
    char *sign_argv[] = {"codesign", "--force", "-s", "-",
                         "--entitlements", ctx->entitlements_path,
                         tmp, NULL};
    rmp_pipe_t sign_proc = rmp_pipe_open(ctx->codesign_path, sign_argv);
    if (!sign_proc.fp) { unlink(tmp); return -1; }
    char line[256];
    while (fgets(line, sizeof(line), sign_proc.fp)) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] codesign: %s", line);
            fflush(ctx->debug_fp);
        }
    }
    int ret = rmp_pipe_close(&sign_proc);
    if (ret != 0) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] cache: codesign failed (exit %d)\n", ret);
            fflush(ctx->debug_fp);
        }
        unlink(tmp);
        return -1;
    }

    // Atomic rename into place
    if (rename(tmp, cached) != 0)
        unlink(tmp); // another process may have created it

    // Write metadata sidecar atomically
    char meta[PATH_MAX];
    snprintf(meta, sizeof(meta), "%s.meta", cached);
    char mbuf[128];
    int mlen = snprintf(mbuf, sizeof(mbuf), "%ld %lld", mtime, (long long)size);
    atomic_write_file(meta, mbuf, mlen, 0644);

    if (ctx->debug_fp) {
        fprintf(ctx->debug_fp, "[remapper] cache: created %s\n", cached);
        fflush(ctx->debug_fp);
    }

    return 0;
}

/*** rmp_resolve_hardened ************************/

const char *rmp_resolve_hardened(rmp_ctx_t *ctx, const char *path, int *was_cached) {
    *was_cached = 0;

    struct stat sb;
    if (stat(path, &sb) != 0) return path;
    if (!S_ISREG(sb.st_mode)) return path;

    // Check on-disk cache first
    char cached[PATH_MAX];
    rmp_cache_path(ctx->cache_dir, path, cached, sizeof(cached));

    if (rmp_cache_valid(cached, sb.st_mtime, sb.st_size)) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] cache hit: %s\n", cached);
            fflush(ctx->debug_fp);
        }
        *was_cached = 1;
        return strdup(cached);
    }

    // Check if hardened
    if (!rmp_is_hardened(ctx, path)) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] not hardened: %s\n", path);
            fflush(ctx->debug_fp);
        }
        return path;
    }

    // Hardened — create cached copy
    if (ctx->debug_fp) {
        fprintf(ctx->debug_fp, "[remapper] hardened, creating cache: %s\n", path);
        fflush(ctx->debug_fp);
    }

    if (rmp_cache_create(ctx, path, cached, sb.st_mtime, sb.st_size) == 0) {
        *was_cached = 1;
        return strdup(cached);
    }

    return path;
}

#endif /* __APPLE__ */
