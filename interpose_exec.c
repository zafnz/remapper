/*
 * interpose_exec.c - Exec/spawn interpose functions with hardened binary re-signing
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <spawn.h>
#include "interpose.h"

/*** Auto-resign hardened binaries ****************/
//
// When posix_spawn (or exec) is called on a Mach-O binary with hardened
// runtime and no allow-dyld-environment-variables entitlement, we:
//   1. Check an in-memory cache (path → hardened?)
//   2. Check an on-disk cache ($RMP_CACHE/<path>)
//   3. If uncached: read Mach-O header to detect hardened runtime
//   4. If hardened: copy to cache, ad-hoc re-sign with entitlement
//   5. Spawn the cached copy instead

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
        RMP_DEBUG("cache hit: %s", cached);
        result = strdup(cached);
        goto done;
    }

    // Check if hardened
    int hardened = rmp_is_hardened(&g_ctx, path);
    mcache_store(path, sb.st_mtime, sb.st_size, hardened);

    if (!hardened) {
        RMP_DEBUG("not hardened: %s", path);
        goto done;
    }

    // Hardened — create cached copy
    RMP_DEBUG("hardened, creating cache: %s", path);

    if (rmp_cache_create(&g_ctx, path, cached, sb.st_mtime, sb.st_size) == 0)
        result = strdup(cached);

done:
    g_resolving = 0;
    return result;
}

/*** Shebang interpreter resolution ****************/
//
// When exec/spawn targets a script whose #! interpreter is either
// SIP-protected (/usr/, /bin/, /sbin/) or has hardened runtime, the
// interpreter would strip DYLD_INSERT_LIBRARIES. We detect this before
// the kernel sees the script, copy+re-sign the interpreter to the
// cache, and exec the cached copy directly.

static int is_sip_path(const char *path) {
    return (strncmp(path, "/usr/", 5) == 0 ||
            strncmp(path, "/bin/", 5) == 0 ||
            strncmp(path, "/sbin/", 6) == 0);
}

// Check if `path` is a script with a shebang interpreter that needs re-signing
// (SIP-protected or hardened runtime). If so, copy+re-sign the interpreter and
// return its cached path (strdup'd).
// *shebang_arg is set to the optional shebang argument (strdup'd), or NULL.
// Returns NULL if no re-signing needed or on failure.
static const char *resolve_shebang_interp(const char *path, char **shebang_arg) {
    *shebang_arg = NULL;
    if (!path || g_num_patterns == 0) return NULL;
    if (g_resolving) return NULL;
    g_resolving = 1;

    const char *result = NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        RMP_DEBUG("shebang: open failed for %s", path);
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

    RMP_DEBUG("shebang check: interp='%s' sip=%d", interp_path, is_sip_path(interp_path));

    // Check if interpreter needs re-signing (SIP-protected or hardened)
    if (!is_sip_path(interp_path)) {
        ensure_ctx();
        int h = rmp_is_hardened(&g_ctx, interp_path);
        RMP_DEBUG("shebang interp hardened=%d", h);
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

    RMP_DEBUG("shebang resign: %s → %s", interp_path, cached);

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
        RMP_DEBUG("posix_spawn: %s → %s (hardened)", path, actual);
        int ret = posix_spawn(pid, actual, fa, sa, argv, envp);
        free((void *)actual);
        return ret;
    }
    // Check for shebang needing re-signing (SIP or hardened interpreter)
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_shebang_interp(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        RMP_DEBUG("posix_spawn shebang: %s → %s", path, cached_interp);
        int ret = posix_spawn(pid, cached_interp, fa, sa, new_argv, envp);
        free((void *)cached_interp);
        free(shebang_arg);
        return ret;
    }
    RMP_DEBUG("posix_spawn: %s", path);
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
            RMP_DEBUG("posix_spawnp: %s → %s (hardened)", file, actual);
            int ret = posix_spawn(pid, actual, fa, sa, argv, envp);
            free((void *)actual);
            return ret;
        }
        // Check for shebang needing re-signing
        char *shebang_arg = NULL;
        const char *cached_interp = resolve_shebang_interp(resolved_path, &shebang_arg);
        if (cached_interp) {
            char *new_argv[256];
            sip_build_argv(new_argv, 256, cached_interp, shebang_arg, resolved_path, argv);
            RMP_DEBUG("posix_spawnp shebang: %s → %s", file, cached_interp);
            int ret = posix_spawn(pid, cached_interp, fa, sa, new_argv, envp);
            free((void *)cached_interp);
            free(shebang_arg);
            return ret;
        }
        RMP_DEBUG("posix_spawnp: %s (resolved: %s)", file, resolved_path);
    } else {
        RMP_DEBUG("posix_spawnp: %s (unresolved)", file);
    }
    return posix_spawnp(pid, file, fa, sa, argv, envp);
}
DYLD_INTERPOSE(my_posix_spawnp, posix_spawnp)

static int my_execve(const char *path, char *const argv[], char *const envp[]) {
    const char *actual = resolve_spawn_path(path);
    if (actual != path) {
        RMP_DEBUG("execve: %s → %s (hardened)", path, actual);
        return execve(actual, argv, envp);
    }
    // Check for shebang needing re-signing
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_shebang_interp(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        RMP_DEBUG("execve shebang: %s → %s", path, cached_interp);
        return execve(cached_interp, new_argv, envp);
    }
    RMP_DEBUG("execve: %s", path);
    return execve(path, argv, envp);
}
DYLD_INTERPOSE(my_execve, execve)

static int my_execv(const char *path, char *const argv[]) {
    const char *actual = resolve_spawn_path(path);
    if (actual != path) {
        RMP_DEBUG("execv: %s → %s (hardened)", path, actual);
        return execv(actual, argv);
    }
    // Check for shebang needing re-signing
    char *shebang_arg = NULL;
    const char *cached_interp = resolve_shebang_interp(path, &shebang_arg);
    if (cached_interp) {
        char *new_argv[256];
        sip_build_argv(new_argv, 256, cached_interp, shebang_arg, path, argv);
        RMP_DEBUG("execv shebang: %s → %s", path, cached_interp);
        return execv(cached_interp, new_argv);
    }
    RMP_DEBUG("execv: %s", path);
    return execv(path, argv);
}
DYLD_INTERPOSE(my_execv, execv)

static int my_execvp(const char *file, char *const argv[]) {
    char resolved_path[PATH_MAX];
    if (resolve_in_path(file, resolved_path, sizeof(resolved_path))) {
        const char *actual = resolve_spawn_path(resolved_path);
        if (actual != resolved_path) {
            RMP_DEBUG("execvp: %s → %s (hardened)", file, actual);
            return execv(actual, argv);
        }
        // Check for shebang needing re-signing
        char *shebang_arg = NULL;
        const char *cached_interp = resolve_shebang_interp(resolved_path, &shebang_arg);
        if (cached_interp) {
            char *new_argv[256];
            sip_build_argv(new_argv, 256, cached_interp, shebang_arg, resolved_path, argv);
            RMP_DEBUG("execvp shebang: %s → %s", file, cached_interp);
            return execv(cached_interp, new_argv);
        }
        RMP_DEBUG("execvp: %s (resolved: %s)", file, resolved_path);
    } else {
        RMP_DEBUG("execvp: %s (unresolved)", file);
    }
    return execvp(file, argv);
}
DYLD_INTERPOSE(my_execvp, execvp)
