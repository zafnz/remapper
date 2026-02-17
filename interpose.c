/*
 * interpose.c - DYLD_INSERT_LIBRARIES interposer for path redirection on macOS
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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

#include "interpose.h"

/*** Global state *********************************/

pattern_t g_patterns[MAX_PATTERNS];
int       g_num_patterns = 0;
char      g_target[PATH_MAX];  // includes trailing '/'
int       g_initialized = 0;
int       g_debug = 0;
FILE     *g_debug_fp = NULL;  // stderr or file

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

                RMP_DEBUG("pattern[%d]: parent='%s' glob='%s'",
                          g_num_patterns,
                          g_patterns[g_num_patterns].parent,
                          g_patterns[g_num_patterns].glob);
                g_num_patterns++;
            }
        }
        tok = strtok_r(NULL, ":", &saveptr);
    }

    RMP_DEBUG("target='%s'  %d pattern(s) loaded", g_target, g_num_patterns);
}

/*** Path rewriting *******************************/

// Try to rewrite `path`. If it matches a pattern, write the rewritten
// path into `out` and return 1. Otherwise return 0.
int try_rewrite(const char *path, char *out, size_t outsize) {
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
            RMP_DEBUG("rewrite: '%s' → '%s'", path, out);
            return 1;
        }
    }
    return 0;
}
