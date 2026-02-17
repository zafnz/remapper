// rmp_shared.c - shared cache and hardened-binary utilities for remapper

#include "rmp_shared.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <stdatomic.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <copyfile.h>

// Thread-safe home directory lookup: try $HOME, fall back to getpwuid_r.
static const char *get_home_dir(char *buf, size_t bufsize) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;

    struct passwd pw, *result = NULL;
    if (getpwuid_r(geteuid(), &pw, buf, bufsize, &result) == 0 && result)
        return result->pw_dir;

    return NULL;
}

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
}

/*** rmp_is_hardened *****************************/

int rmp_is_hardened(const char *path) {
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

    // Check for hardened runtime via codesign
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "codesign -dvvv '%s' 2>&1", path);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;

    int has_runtime = 0;
    char line[1024];
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "runtime")) has_runtime = 1;
    pclose(p);

    if (!has_runtime) return 0;

    // Check entitlements
    snprintf(cmd, sizeof(cmd), "codesign -d --entitlements - '%s' 2>&1", path);
    p = popen(cmd, "r");
    if (!p) return 1;

    int has_dyld_ent = 0;
    while (fgets(line, sizeof(line), p))
        if (strstr(line, "allow-dyld-environment-variables"))
            has_dyld_ent = 1;
    pclose(p);

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
    char cmd[PATH_MAX * 2 + 256];
    snprintf(cmd, sizeof(cmd),
             "codesign --force -s - --entitlements '%s' '%s' 2>&1",
             ctx->entitlements_path, tmp);

    FILE *p = popen(cmd, "r");
    if (!p) { unlink(tmp); return -1; }
    char line[256];
    while (fgets(line, sizeof(line), p)) {
        if (ctx->debug_fp) {
            fprintf(ctx->debug_fp, "[remapper] codesign: %s", line);
            fflush(ctx->debug_fp);
        }
    }
    int ret = pclose(p);
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
    if (!rmp_is_hardened(path)) {
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
