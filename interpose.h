/*
 * interpose.h - Shared declarations for the DYLD interposer
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef INTERPOSE_H
#define INTERPOSE_H

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

extern pattern_t g_patterns[MAX_PATTERNS];
extern int       g_num_patterns;
extern char      g_target[PATH_MAX];
extern int       g_initialized;
extern int       g_debug;
extern FILE     *g_debug_fp;

/*** Path rewriting *******************************/

int try_rewrite(const char *path, char *out, size_t outsize);

/* Convenience: rewrite a single path on the stack
 * Equivilant of the function:
 *  const char *rewrite_path(const char *path) {
 *     static char buf[PATH_MAX];
 *     if (try_rewrite(path, buf, sizeof(buf))) {
 *         return buf;
 *     } else {
 *         return path;
 *     }
 * }
 * But avoids function call overhead and allows inlining into interposed functions.
 */
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

/*** Debug logging ********************************/

#define RMP_DEBUG(fmt, ...) do { \
    if (g_debug) { \
        fprintf(g_debug_fp, "[remapper] " fmt "\n", ##__VA_ARGS__); \
        fflush(g_debug_fp); \
    } \
} while (0)

#endif /* INTERPOSE_H */
