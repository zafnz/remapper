// rmp_shared.h - shared cache and hardened-binary utilities for remapper

#ifndef RMP_SHARED_H
#define RMP_SHARED_H

#include <stdio.h>
#include <limits.h>
#include <sys/types.h>

// Context for cache operations
typedef struct {
    char cache_dir[PATH_MAX];
    char config_dir[PATH_MAX];
    char entitlements_path[PATH_MAX];
    FILE *debug_fp; // NULL = no debug logging
} rmp_ctx_t;

// Create directory path recursively
void rmp_mkdirs(const char *path, mode_t mode);

// Initialize context: populate paths, create dirs, write entitlements plist.
// config_dir / cache_dir: if NULL, defaults to ~/.remapper / ~/.remapper/cache.
// debug_fp: if NULL, no debug logging.
void rmp_ctx_init(rmp_ctx_t *ctx, const char *config_dir,
                  const char *cache_dir, FILE *debug_fp);

// Check if a Mach-O binary has hardened runtime without the
// allow-dyld-environment-variables entitlement.
// Returns 1 if it needs re-signing, 0 otherwise.
int rmp_is_hardened(const char *path);

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

#endif // RMP_SHARED_H
