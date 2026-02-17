// rmp_shared.h - shared utilities for remapper

#ifndef RMP_SHARED_H
#define RMP_SHARED_H

#include <stdio.h>
#include <limits.h>
#include <sys/types.h>

/*** Portable utilities ***************************/

// Create directory path recursively
void rmp_mkdirs(const char *path, mode_t mode);

// Resolve a bare filename via $PATH.
// Returns 1 on success (result in `out`), 0 on failure.
int resolve_in_path(const char *file, char *out, size_t outsize);

// Safe popen replacement: fork+execv with stdout+stderr piped back.
// No shell involved — immune to injection via filenames.
typedef struct {
    FILE *fp;    // read end of pipe (caller reads from this)
    pid_t pid;   // child pid
} rmp_pipe_t;

// Spawn a child process. Returns .fp=NULL on failure.
rmp_pipe_t rmp_pipe_open(const char *path, char *const argv[]);

// Close pipe and wait for child. Returns exit status, or -1 on error.
int rmp_pipe_close(rmp_pipe_t *proc);

/*** macOS-only: hardened binary cache ************/
#ifdef __APPLE__

// Context for cache operations (macOS only — codesign + entitlements)
typedef struct {
    char cache_dir[PATH_MAX];
    char config_dir[PATH_MAX];
    char entitlements_path[PATH_MAX];
    char codesign_path[PATH_MAX];  // resolved once at init
    FILE *debug_fp; // NULL = no debug logging
} rmp_ctx_t;

// Initialize context: populate paths, create dirs, write entitlements plist.
// config_dir / cache_dir: if NULL, defaults to ~/.remapper / ~/.remapper/cache.
// debug_fp: if NULL, no debug logging.
void rmp_ctx_init(rmp_ctx_t *ctx, const char *config_dir,
                  const char *cache_dir, FILE *debug_fp);

// Check if a Mach-O binary has hardened runtime without the
// allow-dyld-environment-variables entitlement.
// Returns 1 if it needs re-signing, 0 otherwise.
int rmp_is_hardened(const rmp_ctx_t *ctx, const char *path);

// Build the cached path for a binary: <cache_dir><original_path>
void rmp_cache_path(const char *cache_dir, const char *original,
                    char *out, size_t outsize);

// Check if on-disk cache is valid (exists and matches original mtime/size)
int rmp_cache_valid(const char *cached, time_t orig_mtime, off_t orig_size);

// Copy binary to cache and re-sign with entitlements.
// Thread-safe: uses atomic counter for unique temp file names.
// Returns 0 on success, -1 on failure.
int rmp_cache_create(rmp_ctx_t *ctx, const char *original,
                     const char *cached, time_t mtime, off_t size);

// High-level: check if binary is hardened, and if so return a cached
// re-signed copy. Returns the path to use (original or cached).
// If a new string is returned, caller must free() it.
// Sets *was_cached = 1 if the returned path is a cached copy.
const char *rmp_resolve_hardened(rmp_ctx_t *ctx, const char *path, int *was_cached);

#endif /* __APPLE__ */

#endif // RMP_SHARED_H
