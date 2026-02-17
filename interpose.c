/*
 * interpose.c - DYLD_INSERT_LIBRARIES interposer for path redirection on macOS
 *
 * Reads configuration from environment variables:
 *   RMP_TARGET    - target directory (e.g., /tmp/myapp-v1)
 *   RMP_MAPPINGS  - colon-separated patterns (e.g., $HOME/.claude*:/tmp/.stuff*)
 *   RMP_DEBUG_LOG - log file path (enables debug logging when set)
 *   RMP_CONFIG    - base config directory (default: ~/.remapper/)
 *   RMP_CACHE     - cache directory (default: $RMP_CONFIG/cache/)
 *
 * Each mapping is split into (parent_dir, glob). When any intercepted filesystem
 * call receives a path starting with parent_dir whose next component matches glob,
 * the parent_dir prefix is replaced with RMP_TARGET.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <limits.h>
#include <errno.h>

#include "rmp_shared.h"

/*** DYLD interpose macro *************************/

#define DYLD_INTERPOSE(_replacement, _replacee) \
    __attribute__((used)) static struct { \
        const void *replacement; \
        const void *replacee; \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, \
        (const void *)(unsigned long)&_replacee \
    };

/*** Pattern storage ******************************/

#define MAX_PATTERNS 64

typedef struct {
    char parent[PATH_MAX];   // e.g. "/home/user/"
    size_t parent_len;
    char glob[256];          // e.g. ".claude*"
} pattern_t;

static pattern_t g_patterns[MAX_PATTERNS];
static int       g_num_patterns = 0;
static char      g_target[PATH_MAX];  // includes trailing '/'
static int       g_initialized = 0;
static int       g_debug = 0;
static FILE     *g_debug_fp = NULL;  // stderr or file

/*** Initialiser (runs when dylib is loaded) ******/

__attribute__((constructor))
static void remapper_init(void) {
    if (g_initialized) return;
    g_initialized = 1;

    const char *debug_log = getenv("RMP_DEBUG_LOG");
    if (debug_log && debug_log[0]) {
        g_debug = 1;
        g_debug_fp = fopen(debug_log, "a");
        if (!g_debug_fp) g_debug_fp = stderr;
    }

    const char *target = getenv("RMP_TARGET");
    const char *pats   = getenv("RMP_MAPPINGS");
    if (!target || !pats) return;

    // Copy target, ensure trailing slash
    size_t tlen = strlen(target);
    if (tlen == 0 || tlen >= sizeof(g_target) - 1) return;
    memcpy(g_target, target, tlen);
    if (g_target[tlen - 1] != '/') g_target[tlen++] = '/';
    g_target[tlen] = '\0';

    // Parse colon-separated patterns
    char buf[PATH_MAX * 16];
    strncpy(buf, pats, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ":", &saveptr);
    while (tok && g_num_patterns < MAX_PATTERNS) {
        // trim spaces
        while (*tok == ' ') tok++;
        size_t toklen = strlen(tok);
        while (toklen > 0 && tok[toklen - 1] == ' ') tok[--toklen] = '\0';
        if (toklen == 0) { tok = strtok_r(NULL, ":", &saveptr); continue; }

        // split at last '/' → (parent_dir, glob_component)
        char *last_slash = strrchr(tok, '/');
        if (last_slash && last_slash != tok) {
            size_t plen = (size_t)(last_slash - tok + 1);  // includes '/'
            if (plen < PATH_MAX && (toklen - plen) < 256) {
                memcpy(g_patterns[g_num_patterns].parent, tok, plen);
                g_patterns[g_num_patterns].parent[plen] = '\0';
                g_patterns[g_num_patterns].parent_len = plen;
                strcpy(g_patterns[g_num_patterns].glob, last_slash + 1);

                if (g_debug)
                    fprintf(g_debug_fp, "[remapper] pattern[%d]: parent='%s' glob='%s'\n",
                            g_num_patterns,
                            g_patterns[g_num_patterns].parent,
                            g_patterns[g_num_patterns].glob);
                g_num_patterns++;
            }
        }
        tok = strtok_r(NULL, ":", &saveptr);
    }

    if (g_debug)
        fprintf(g_debug_fp, "[remapper] target='%s'  %d pattern(s) loaded\n",
                g_target, g_num_patterns);
}

/*** Path rewriting *******************************/

// Try to rewrite `path`. If it matches a pattern, write the rewritten
// path into `out` and return 1. Otherwise return 0.
static int try_rewrite(const char *path, char *out, size_t outsize) {
    if (!path || g_num_patterns == 0) return 0;

    for (int i = 0; i < g_num_patterns; i++) {
        if (strncmp(path, g_patterns[i].parent, g_patterns[i].parent_len) != 0)
            continue;

        const char *rest = path + g_patterns[i].parent_len;
        if (*rest == '\0') continue;  // path IS the parent dir, nothing to match

        // Extract next path component
        const char *slash = strchr(rest, '/');
        size_t clen = slash ? (size_t)(slash - rest) : strlen(rest);
        if (clen == 0 || clen >= 256) continue;

        char component[256];
        memcpy(component, rest, clen);
        component[clen] = '\0';

        if (fnmatch(g_patterns[i].glob, component, 0) == 0) {
            int n = snprintf(out, outsize, "%s%s", g_target, rest);
            if (n < 0 || (size_t)n >= outsize) continue;
            if (g_debug) {
                fprintf(g_debug_fp, "[remapper] rewrite: '%s' → '%s'\n", path, out);
                fflush(g_debug_fp);
            }
            return 1;
        }
    }
    return 0;
}

// Convenience: rewrite a single path on the stack
#define REWRITE_1(varname, path) \
    char varname##_buf[PATH_MAX]; \
    const char *varname = try_rewrite((path), varname##_buf, sizeof(varname##_buf)) \
                          ? varname##_buf : (path)

// Convenience: rewrite a path only if absolute (for *at() variants)
#define REWRITE_ABS(varname, path) \
    char varname##_buf[PATH_MAX]; \
    const char *varname = ((path) && (path)[0] == '/' && \
                           try_rewrite((path), varname##_buf, sizeof(varname##_buf))) \
                          ? varname##_buf : (path)

/*** Interposed functions *************************/

static int my_open(const char *path, int flags, ...) {
    REWRITE_1(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return open(actual, flags, mode);
    }
    return open(actual, flags);
}
DYLD_INTERPOSE(my_open, open)

static int my_openat(int fd, const char *path, int flags, ...) {
    REWRITE_ABS(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return openat(fd, actual, flags, mode);
    }
    return openat(fd, actual, flags);
}
DYLD_INTERPOSE(my_openat, openat)

//creat
static int my_creat(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return open(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}
DYLD_INTERPOSE(my_creat, creat)

//stat / lstat / fstatat
static int my_stat(const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return stat(actual, sb);
}
DYLD_INTERPOSE(my_stat, stat)

static int my_lstat(const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return lstat(actual, sb);
}
DYLD_INTERPOSE(my_lstat, lstat)

static int my_fstatat(int fd, const char *path, struct stat *sb, int flag) {
    REWRITE_ABS(actual, path);
    return fstatat(fd, actual, sb, flag);
}
DYLD_INTERPOSE(my_fstatat, fstatat)

//access / faccessat
static int my_access(const char *path, int mode) {
    REWRITE_1(actual, path);
    return access(actual, mode);
}
DYLD_INTERPOSE(my_access, access)

static int my_faccessat(int fd, const char *path, int mode, int flag) {
    REWRITE_ABS(actual, path);
    return faccessat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_faccessat, faccessat)

//mkdir / mkdirat
static int my_mkdir(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return mkdir(actual, mode);
}
DYLD_INTERPOSE(my_mkdir, mkdir)

static int my_mkdirat(int fd, const char *path, mode_t mode) {
    REWRITE_ABS(actual, path);
    return mkdirat(fd, actual, mode);
}
DYLD_INTERPOSE(my_mkdirat, mkdirat)

//unlink / unlinkat
static int my_unlink(const char *path) {
    REWRITE_1(actual, path);
    return unlink(actual);
}
DYLD_INTERPOSE(my_unlink, unlink)

static int my_unlinkat(int fd, const char *path, int flag) {
    REWRITE_ABS(actual, path);
    return unlinkat(fd, actual, flag);
}
DYLD_INTERPOSE(my_unlinkat, unlinkat)

//rename / renameat
static int my_rename(const char *oldp, const char *newp) {
    REWRITE_1(aold, oldp);
    REWRITE_1(anew, newp);
    return rename(aold, anew);
}
DYLD_INTERPOSE(my_rename, rename)

static int my_renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    REWRITE_ABS(aold, oldp);
    REWRITE_ABS(anew, newp);
    return renameat(ofd, aold, nfd, anew);
}
DYLD_INTERPOSE(my_renameat, renameat)

//rmdir
static int my_rmdir(const char *path) {
    REWRITE_1(actual, path);
    return rmdir(actual);
}
DYLD_INTERPOSE(my_rmdir, rmdir)

//opendir
static DIR *my_opendir(const char *path) {
    REWRITE_1(actual, path);
    return opendir(actual);
}
DYLD_INTERPOSE(my_opendir, opendir)

//chdir
static int my_chdir(const char *path) {
    REWRITE_1(actual, path);
    return chdir(actual);
}
DYLD_INTERPOSE(my_chdir, chdir)

//readlink / readlinkat
static ssize_t my_readlink(const char *path, char *buf, size_t bufsiz) {
    REWRITE_1(actual, path);
    return readlink(actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlink, readlink)

static ssize_t my_readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    REWRITE_ABS(actual, path);
    return readlinkat(fd, actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlinkat, readlinkat)

//chmod / fchmodat
static int my_chmod(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return chmod(actual, mode);
}
DYLD_INTERPOSE(my_chmod, chmod)

static int my_fchmodat(int fd, const char *path, mode_t mode, int flag) {
    REWRITE_ABS(actual, path);
    return fchmodat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_fchmodat, fchmodat)

//chown / lchown / fchownat
static int my_chown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return chown(actual, owner, group);
}
DYLD_INTERPOSE(my_chown, chown)

static int my_lchown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return lchown(actual, owner, group);
}
DYLD_INTERPOSE(my_lchown, lchown)

static int my_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag) {
    REWRITE_ABS(actual, path);
    return fchownat(fd, actual, owner, group, flag);
}
DYLD_INTERPOSE(my_fchownat, fchownat)

//symlink / symlinkat
static int my_symlink(const char *target, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_1(alink, linkpath);
    return symlink(atarget, alink);
}
DYLD_INTERPOSE(my_symlink, symlink)

static int my_symlinkat(const char *target, int fd, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_ABS(alink, linkpath);
    return symlinkat(atarget, fd, alink);
}
DYLD_INTERPOSE(my_symlinkat, symlinkat)

//link / linkat
static int my_link(const char *p1, const char *p2) {
    REWRITE_1(a1, p1);
    REWRITE_1(a2, p2);
    return link(a1, a2);
}
DYLD_INTERPOSE(my_link, link)

static int my_linkat(int fd1, const char *p1, int fd2, const char *p2, int flag) {
    REWRITE_ABS(a1, p1);
    REWRITE_ABS(a2, p2);
    return linkat(fd1, a1, fd2, a2, flag);
}
DYLD_INTERPOSE(my_linkat, linkat)

//truncate
static int my_truncate(const char *path, off_t length) {
    REWRITE_1(actual, path);
    return truncate(actual, length);
}
DYLD_INTERPOSE(my_truncate, truncate)

//realpath
static char *my_realpath(const char *path, char *resolved) {
    REWRITE_1(actual, path);
    return realpath(actual, resolved);
}
DYLD_INTERPOSE(my_realpath, realpath)

/*** Auto-resign hardened binaries ****************/
//
// When posix_spawn (or exec) is called on a Mach-O binary with hardened
// runtime and no allow-dyld-environment-variables entitlement, we:
//   1. Check an in-memory cache (path → hardened?)
//   2. Check an on-disk cache ($RMP_CACHE/<path>)
//   3. If uncached: read Mach-O header to detect hardened runtime
//   4. If hardened: copy to cache, ad-hoc re-sign with entitlement
//   5. Spawn the cached copy instead

#include <spawn.h>

// Shared context for cache operations
static rmp_ctx_t g_ctx;
static int       g_ctx_initialized = 0;

static void ensure_ctx(void) {
    if (g_ctx_initialized) return;
    g_ctx_initialized = 1;
    rmp_ctx_init(&g_ctx, getenv("RMP_CONFIG"), getenv("RMP_CACHE"),
                 g_debug ? g_debug_fp : NULL);
}

// In-memory cache (interposer-specific, for per-process speed)
#define MCACHE_SIZE 128
typedef struct {
    char path[PATH_MAX];
    int  hardened;       // 0=not hardened, 1=hardened
    time_t mtime;
    off_t  size;
} mcache_entry_t;

static mcache_entry_t g_mcache[MCACHE_SIZE];
static int g_mcache_count = 0;

// Check in-memory cache. Returns: -1=miss, 0=hardened, 1=not hardened
static int mcache_lookup(const char *path, time_t mtime, off_t size) {
    for (int i = 0; i < g_mcache_count; i++) {
        if (strcmp(g_mcache[i].path, path) == 0) {
            if (g_mcache[i].mtime == mtime && g_mcache[i].size == size)
                return g_mcache[i].hardened ? 0 : 1;
            return -1;  // stale
        }
    }
    return -1;
}

static void mcache_store(const char *path, time_t mtime, off_t size, int hardened) {
    int slot = -1;
    for (int i = 0; i < g_mcache_count; i++) {
        if (strcmp(g_mcache[i].path, path) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_mcache_count >= MCACHE_SIZE) return;
        slot = g_mcache_count++;
    }
    strncpy(g_mcache[slot].path, path, PATH_MAX - 1);
    g_mcache[slot].mtime = mtime;
    g_mcache[slot].size = size;
    g_mcache[slot].hardened = hardened;
}

// Resolve a binary path for spawning. If it's hardened, return the
// cached re-signed path. Otherwise return the original.
static __thread int g_resolving = 0;  // re-entrancy guard

static const char *resolve_spawn_path(const char *path) {
    if (!path || g_num_patterns == 0) return path;
    if (g_resolving) return path;  // avoid deadlock from popen→exec→resolve
    g_resolving = 1;

    const char *result = path;

    ensure_ctx();

    struct stat sb;
    if (stat(path, &sb) != 0) goto done;
    if (!S_ISREG(sb.st_mode)) goto done;

    // In-memory cache lookup
    int mc = mcache_lookup(path, sb.st_mtime, sb.st_size);
    if (mc == 1) goto done;  // known not-hardened

    char cached[PATH_MAX];
    rmp_cache_path(g_ctx.cache_dir, path, cached, sizeof(cached));

    if (mc == 0) {
        if (rmp_cache_valid(cached, sb.st_mtime, sb.st_size)) {
            result = strdup(cached);
            goto done;
        }
    }

    // Check on-disk cache
    if (rmp_cache_valid(cached, sb.st_mtime, sb.st_size)) {
        mcache_store(path, sb.st_mtime, sb.st_size, 1);
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] cache hit: %s\n", cached);
            fflush(g_debug_fp);
        }
        result = strdup(cached);
        goto done;
    }

    // Check if hardened
    int hardened = rmp_is_hardened(&g_ctx, path);
    mcache_store(path, sb.st_mtime, sb.st_size, hardened);

    if (!hardened) {
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] not hardened: %s\n", path);
            fflush(g_debug_fp);
        }
        goto done;
    }

    // Hardened — create cached copy
    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] hardened, creating cache: %s\n", path);
        fflush(g_debug_fp);
    }

    if (rmp_cache_create(&g_ctx, path, cached, sb.st_mtime, sb.st_size) == 0)
        result = strdup(cached);

done:
    g_resolving = 0;
    return result;
}

/*** SIP-protected shebang resolution *************/
//
// When exec/spawn targets a script whose #! interpreter lives under
// /usr/, /bin/, or /sbin/, macOS SIP strips DYLD_INSERT_LIBRARIES.
// We detect this before the kernel sees the script, copy+re-sign the
// interpreter to the cache, and exec the cached copy directly.

static int is_sip_path(const char *path) {
    return (strncmp(path, "/usr/", 5) == 0 ||
            strncmp(path, "/bin/", 5) == 0 ||
            strncmp(path, "/sbin/", 6) == 0);
}

// Check if `path` is a script with a SIP-protected shebang.
// If so, copy+re-sign the interpreter and return its cached path (strdup'd).
// *shebang_arg is set to the optional shebang argument (strdup'd), or NULL.
// Returns NULL if not a SIP script or on failure.
static const char *resolve_sip_shebang(const char *path, char **shebang_arg) {
    *shebang_arg = NULL;
    if (!path || g_num_patterns == 0) return NULL;
    if (g_resolving) return NULL;
    g_resolving = 1;

    const char *result = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (g_debug) { fprintf(g_debug_fp, "[remapper] shebang: open failed for %s\n", path); fflush(g_debug_fp); }
        goto done;
    }

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 3 || buf[0] != '#' || buf[1] != '!') goto done;
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    char *interp = buf + 2;
    while (*interp == ' ') interp++;

    // Parse interpreter path and optional arg
    char interp_path[PATH_MAX];
    char *space = strchr(interp, ' ');
    if (space) {
        size_t ilen = (size_t)(space - interp);
        if (ilen >= sizeof(interp_path)) goto done;
        memcpy(interp_path, interp, ilen);
        interp_path[ilen] = '\0';
        char *arg = space + 1;
        while (*arg == ' ') arg++;
        if (*arg) *shebang_arg = strdup(arg);
    } else {
        strncpy(interp_path, interp, sizeof(interp_path) - 1);
        interp_path[sizeof(interp_path) - 1] = '\0';
    }

    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] shebang check: interp='%s' sip=%d\n",
                interp_path, is_sip_path(interp_path));
        fflush(g_debug_fp);
    }

    // Check if interpreter needs re-signing (SIP-protected or hardened)
    if (!is_sip_path(interp_path)) {
        ensure_ctx();
        int h = rmp_is_hardened(&g_ctx, interp_path);
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] shebang interp hardened=%d\n", h);
            fflush(g_debug_fp);
        }
        if (!h) {
            free(*shebang_arg);
            *shebang_arg = NULL;
            goto done;
        }
    }

    // Copy + re-sign the interpreter
    ensure_ctx();

    struct stat sb;
    if (stat(interp_path, &sb) != 0) {
        free(*shebang_arg);
        *shebang_arg = NULL;
        goto done;
    }

    char cached[PATH_MAX];
    rmp_cache_path(g_ctx.cache_dir, interp_path, cached, sizeof(cached));

    if (!rmp_cache_valid(cached, sb.st_mtime, sb.st_size)) {
        if (rmp_cache_create(&g_ctx, interp_path, cached,
                             sb.st_mtime, sb.st_size) != 0) {
            free(*shebang_arg);
            *shebang_arg = NULL;
            goto done;
        }
    }

    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] shebang resign: %s → %s\n", interp_path, cached);
        fflush(g_debug_fp);
    }

    result = strdup(cached);

done:
    g_resolving = 0;
    return result;
}

// Build a rewritten argv for SIP shebang exec.
// out[]: [cached_interp, shebang_arg?, script_path, orig_argv[1], ...]
// Returns number of elements (not counting NULL terminator).
static int sip_build_argv(char **out, int max,
                           const char *interp, const char *shebang_arg,
                           const char *script, char *const orig_argv[]) {
    int n = 0;
    out[n++] = (char *)interp;
    if (shebang_arg && n < max - 1) out[n++] = (char *)shebang_arg;
    if (n < max - 1) out[n++] = (char *)script;
    if (orig_argv) {
        for (int i = 1; orig_argv[i] && n < max - 1; i++)
            out[n++] = orig_argv[i];
    }
    out[n] = NULL;
    return n;
}

/*** Interposed exec/spawn functions **************/

static int my_posix_spawn(pid_t *pid, const char *path,
                          const posix_spawn_file_actions_t *fa,
                          const posix_spawnattr_t *sa,
                          char *const argv[], char *const envp[]) {
    const char *actual = resolve_spawn_path(path);
    if (actual != path) {
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] posix_spawn: %s → %s (hardened)\n", path, actual);
            fflush(g_debug_fp);
        }
        return posix_spawn(pid, actual, fa, sa, argv, envp);
    }
    // Check for shebang needing re-signing (SIP or hardened interpreter)
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_sip_shebang(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] posix_spawn shebang: %s → %s\n", path, cached_interp);
            fflush(g_debug_fp);
        }
        return posix_spawn(pid, cached_interp, fa, sa, new_argv, envp);
    }
    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] posix_spawn: %s\n", path);
        fflush(g_debug_fp);
    }
    return posix_spawn(pid, path, fa, sa, argv, envp);
}
DYLD_INTERPOSE(my_posix_spawn, posix_spawn)

static int my_posix_spawnp(pid_t *pid, const char *file,
                           const posix_spawn_file_actions_t *fa,
                           const posix_spawnattr_t *sa,
                           char *const argv[], char *const envp[]) {
    char resolved_path[PATH_MAX];
    if (resolve_in_path(file, resolved_path, sizeof(resolved_path))) {
        const char *actual = resolve_spawn_path(resolved_path);
        if (actual != resolved_path) {
            if (g_debug) {
                fprintf(g_debug_fp, "[remapper] posix_spawnp: %s → %s (hardened)\n", file, actual);
                fflush(g_debug_fp);
            }
            return posix_spawn(pid, actual, fa, sa, argv, envp);
        }
        // Check for SIP-protected shebang
        char *shebang_arg = NULL;
        const char *cached_interp = resolve_sip_shebang(resolved_path, &shebang_arg);
        if (cached_interp) {
            char *new_argv[256];
            sip_build_argv(new_argv, 256, cached_interp, shebang_arg, resolved_path, argv);
            if (g_debug) {
                fprintf(g_debug_fp, "[remapper] posix_spawnp shebang: %s → %s\n", file, cached_interp);
                fflush(g_debug_fp);
            }
            return posix_spawn(pid, cached_interp, fa, sa, new_argv, envp);
        }
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] posix_spawnp: %s (resolved: %s)\n", file, resolved_path);
            fflush(g_debug_fp);
        }
    } else if (g_debug) {
        fprintf(g_debug_fp, "[remapper] posix_spawnp: %s (unresolved)\n", file);
        fflush(g_debug_fp);
    }
    return posix_spawnp(pid, file, fa, sa, argv, envp);
}
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp)

static int my_execve(const char *path, char *const argv[], char *const envp[]) {
    const char *actual = resolve_spawn_path(path);
    if (actual != path) {
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] execve: %s → %s (hardened)\n", path, actual);
            fflush(g_debug_fp);
        }
        return execve(actual, argv, envp);
    }
    // Check for SIP-protected shebang
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_sip_shebang(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] execve shebang: %s → %s\n", path, cached_interp);
            fflush(g_debug_fp);
        }
        return execve(cached_interp, new_argv, envp);
    }
    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] execve: %s\n", path);
        fflush(g_debug_fp);
    }
    return execve(path, argv, envp);
}
DYLD_INTERPOSE(my_execve, execve)

static int my_execv(const char *path, char *const argv[]) {
    const char *actual = resolve_spawn_path(path);
    if (actual != path) {
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] execv: %s → %s (hardened)\n", path, actual);
            fflush(g_debug_fp);
        }
        return execv(actual, argv);
    }
    // Check for SIP-protected shebang
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_sip_shebang(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] execv shebang: %s → %s\n", path, cached_interp);
            fflush(g_debug_fp);
        }
        return execv(cached_interp, new_argv);
    }
    if (g_debug) {
        fprintf(g_debug_fp, "[remapper] execv: %s\n", path);
        fflush(g_debug_fp);
    }
    return execv(path, argv);
}
DYLD_INTERPOSE(my_execv, execv)

static int my_execvp(const char *file, char *const argv[]) {
    char resolved_path[PATH_MAX];
    if (resolve_in_path(file, resolved_path, sizeof(resolved_path))) {
        const char *actual = resolve_spawn_path(resolved_path);
        if (actual != resolved_path) {
            if (g_debug) {
                fprintf(g_debug_fp, "[remapper] execvp: %s → %s (hardened)\n", file, actual);
                fflush(g_debug_fp);
            }
            return execv(actual, argv);
        }
        // Check for SIP-protected shebang
        char *shebang_arg = NULL;
        const char *cached_interp = resolve_sip_shebang(resolved_path, &shebang_arg);
        if (cached_interp) {
            char *new_argv[256];
            sip_build_argv(new_argv, 256, cached_interp, shebang_arg, resolved_path, argv);
            if (g_debug) {
                fprintf(g_debug_fp, "[remapper] execvp shebang: %s → %s\n", file, cached_interp);
                fflush(g_debug_fp);
            }
            return execv(cached_interp, new_argv);
        }
        if (g_debug) {
            fprintf(g_debug_fp, "[remapper] execvp: %s (resolved: %s)\n", file, resolved_path);
            fflush(g_debug_fp);
        }
    } else if (g_debug) {
        fprintf(g_debug_fp, "[remapper] execvp: %s (unresolved)\n", file);
        fflush(g_debug_fp);
    }
    return execvp(file, argv);
}
DYLD_INTERPOSE(my_execvp, execvp)
