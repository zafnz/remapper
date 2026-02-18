/*
 * remapper_linux.c - redirect filesystem paths using mount namespaces
 *
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *
 * Usage:
 *   remapper [--debug-log <file>] <target-dir> <mapping>... -- <program> [args...]
 *
 * If '--' is absent, exactly one mapping is expected:
 *   remapper <target-dir> <mapping> <program> [args...]
 *
 * Examples:
 *   remapper ~/v1 '~/.claude*' -- claude
 *   remapper ~/v1 '~/.codex*' codex --model X
 *   remapper --debug-log /tmp/rmp.log ~/v1 '~/.claude*' '~/.config*' -- claude
 *
 * Mappings must be single-quoted to prevent shell glob expansion.
 *
 * Environment variables:
 *   RMP_DEBUG_LOG  Log file path (enables debug logging when set)
 *
 * How it works (Linux mount namespaces):
 *
 *   Unlike macOS, Linux can't reliably use LD_PRELOAD to intercept filesystem
 *   calls — statically-linked binaries (e.g. musl/Go) ignore it entirely.
 *
 *   Instead, we use the kernel's mount namespace feature:
 *
 *   1. Parse the glob patterns and scan the filesystem to find matching
 *      files and directories (e.g. ~/.claude, ~/.claude.json).
 *
 *   2. For each match, create an empty target (mkdir or touch) under the
 *      target directory so we have something to mount over.
 *
 *   3. Call unshare(CLONE_NEWUSER | CLONE_NEWNS) to create a private
 *      mount namespace.  CLONE_NEWUSER gives us an unprivileged user
 *      namespace (no root needed); CLONE_NEWNS gives us a private mount
 *      table that only this process (and its children) can see.
 *
 *   4. Write UID/GID mappings so the kernel maps our real UID/GID into
 *      the new namespace (otherwise we'd appear as "nobody").
 *
 *   5. Bind-mount each target path over the original path.  A bind mount
 *      makes a file or directory appear at a different location — like a
 *      hard link that works across filesystems and on directories.  Since
 *      we're in a private namespace, these mounts are invisible to other
 *      processes.
 *
 *   6. exec() the program.  It sees the bind-mounted paths as if they
 *      were the originals.  This works on ALL binaries — static, dynamic,
 *      scripts, anything — because the redirection happens at the VFS
 *      layer in the kernel.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>

/*** Debug logging ********************************/

static FILE *g_debug_fp = NULL;

#define DEBUG(fmt, ...) do { \
    if (g_debug_fp) { \
        fprintf(g_debug_fp, "[remapper] " fmt "\n", ##__VA_ARGS__); \
        fflush(g_debug_fp); \
    } \
} while (0)

/*** Helpers **************************************/

// Thread-safe home directory lookup: try $HOME, fall back to getpwuid_r.
static const char *get_home_dir(char *buf, size_t bufsize) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;

    struct passwd pw, *result = NULL;
    if (getpwuid_r(geteuid(), &pw, buf, bufsize, &result) == 0 && result)
        return result->pw_dir;

    return NULL;
}

// Expand leading ~ or ~/ to $HOME.  Caller must free() the result.
static char *expand_tilde(const char *path) {
    if (!path || path[0] != '~')
        return strdup(path);
    if (path[1] != '/' && path[1] != '\0')
        return strdup(path);   // ~user form not supported

    char pwbuf[1024];
    const char *home = get_home_dir(pwbuf, sizeof(pwbuf));
    if (!home) return strdup(path);

    size_t hlen = strlen(home);
    size_t rest = strlen(path + 1);          // skip the '~'
    char *out = malloc(hlen + rest + 1);
    if (!out) { perror("malloc"); exit(1); }
    memcpy(out, home, hlen);
    memcpy(out + hlen, path + 1, rest + 1);  // includes '\0'
    return out;
}

// Make a path absolute.  If it's already absolute, return a copy.
// Otherwise prepend CWD.  Caller must free().
static char *make_absolute(const char *path) {
    if (!path) return NULL;
    char *expanded = expand_tilde(path);
    if (expanded[0] == '/') return expanded;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); exit(1); }

    size_t clen = strlen(cwd);
    size_t plen = strlen(expanded);
    char *out = malloc(clen + 1 + plen + 1);
    if (!out) { perror("malloc"); exit(1); }
    sprintf(out, "%s/%s", cwd, expanded);
    free(expanded);
    return out;
}

// Create directory path recursively (like mkdir -p).
static void mkdirs(const char *path, mode_t mode) {
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

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--debug-log <file>] <target-dir> <mapping>... -- <program> [args...]\n"
        "\n"
        "Redirect filesystem paths matching <mapping> into <target-dir>.\n"
        "\n"
        "Mappings are full paths with optional globs in the last component.\n"
        "Single-quote mappings to prevent shell glob expansion.\n"
        "If '--' is absent, exactly one mapping is expected.\n"
        "\n"
        "Options:\n"
        "  --debug-log <file>   Log debug output to <file>\n"
        "\n"
        "Examples:\n"
        "  %s ~/v1 '~/.claude*' -- claude\n"
        "  %s ~/v1 '~/.codex*' codex --model X\n"
        "  %s --debug-log /tmp/rmp.log ~/v1 '~/.claude*' -- claude\n"
        "\n"
        "Environment variables:\n"
        "  RMP_DEBUG_LOG   Log file (enables debug when set)\n",
        prog, prog, prog, prog);
    exit(1);
}

/*** Pattern storage ******************************/

#define MAX_PATTERNS 64

typedef struct {
    char parent[PATH_MAX];   // parent directory, e.g. "/home/user/"
    size_t parent_len;
    char glob[256];          // glob for the last component, e.g. ".claude*"
} pattern_t;

/*** Bind mount list ******************************/

// Each entry represents one bind mount to set up: mount source over original.
#define MAX_MOUNTS 256

typedef struct {
    char original[PATH_MAX]; // the real path (mount point)
    char target[PATH_MAX];   // path under target-dir (mount source)
    int is_dir;              // 1 = directory, 0 = file
} mount_entry_t;

static mount_entry_t g_mounts[MAX_MOUNTS];
static int g_num_mounts = 0;

// Add a bind mount entry.  `original` is the real path (e.g. /home/user/.claude),
// `target_dir` is the base target directory, `rest` is the path component after
// the parent (e.g. ".claude" or ".claude/config").
static void add_mount(const char *original, const char *target_dir, const char *rest, int is_dir) {
    if (g_num_mounts >= MAX_MOUNTS) {
        fprintf(stderr, "remapper: too many mount entries (max %d)\n", MAX_MOUNTS);
        exit(1);
    }

    mount_entry_t *m = &g_mounts[g_num_mounts];

    snprintf(m->original, sizeof(m->original), "%s", original);
    snprintf(m->target, sizeof(m->target), "%s/%s", target_dir, rest);
    m->is_dir = is_dir;

    DEBUG("mount entry: %s -> %s (%s)", m->target, m->original,
          is_dir ? "dir" : "file");

    g_num_mounts++;
}

/*** Argument parsing *****************************/

// Parse CLI arguments.  Sets *target (malloc'd, absolute path) and
// fills `patterns` array.  Returns the argv index where the command starts.
static int parse_args(int argc, char **argv,
                      char **target, const char **debug_log,
                      pattern_t *patterns, int *num_patterns) {
    int arg_idx = 1;
    *debug_log = getenv("RMP_DEBUG_LOG");

    while (arg_idx < argc && argv[arg_idx][0] == '-' && strcmp(argv[arg_idx], "--") != 0) {
        if (strncmp(argv[arg_idx], "--debug-log=", 12) == 0) {
            *debug_log = argv[arg_idx] + 12;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--debug-log") == 0 && arg_idx + 1 < argc) {
            *debug_log = argv[arg_idx + 1];
            arg_idx += 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[arg_idx]);
            usage(argv[0]);
        }
    }

    // Need at least: target, mapping, command
    if (argc - arg_idx < 3) usage(argv[0]);

    // argv[arg_idx] = target directory
    *target = make_absolute(argv[arg_idx]);
    mkdirs(*target, 0755);

    // Find '--' separator
    int sep_idx = -1;
    for (int i = arg_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { sep_idx = i; break; }
    }

    int map_start = arg_idx + 1;
    int map_end, cmd_start;

    if (sep_idx >= 0) {
        map_end   = sep_idx;
        cmd_start = sep_idx + 1;
    } else {
        map_end   = arg_idx + 2;
        cmd_start = arg_idx + 2;
    }

    if (cmd_start >= argc) {
        fprintf(stderr, "Error: no command specified\n\n");
        usage(argv[0]);
    }
    if (map_end <= map_start) {
        fprintf(stderr, "Error: no mappings specified\n\n");
        usage(argv[0]);
    }

    // Parse each mapping into (parent_dir, glob_component)
    // Same format as macOS: split at last '/' to get parent + glob
    *num_patterns = 0;
    for (int i = map_start; i < map_end; i++) {
        if (*num_patterns >= MAX_PATTERNS) {
            fprintf(stderr, "Error: too many patterns (max %d)\n", MAX_PATTERNS);
            exit(1);
        }

        char *abs = make_absolute(argv[i]);
        char *last_slash = strrchr(abs, '/');

        if (last_slash && last_slash != abs) {
            size_t plen = (size_t)(last_slash - abs + 1);  // includes '/'
            size_t glen = strlen(last_slash + 1);

            if (plen < PATH_MAX && glen < 256) {
                memcpy(patterns[*num_patterns].parent, abs, plen);
                patterns[*num_patterns].parent[plen] = '\0';
                patterns[*num_patterns].parent_len = plen;
                strcpy(patterns[*num_patterns].glob, last_slash + 1);
                (*num_patterns)++;
            }
        }

        free(abs);
    }

    return cmd_start;
}

/*** Glob resolution ******************************/

// Scan each pattern's parent directory and find entries matching the glob.
// For each match, add a mount entry.
static void resolve_globs(const pattern_t *patterns, int num_patterns,
                          const char *target_dir) {
    for (int i = 0; i < num_patterns; i++) {
        const pattern_t *pat = &patterns[i];

        DEBUG("scanning '%s' for '%s'", pat->parent, pat->glob);

        DIR *dp = opendir(pat->parent);
        if (!dp) {
            DEBUG("  opendir failed: %s", strerror(errno));
            continue;
        }

        struct dirent *ent;
        while ((ent = readdir(dp)) != NULL) {
            // Skip . and ..
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            // Check if this entry matches the glob pattern
            if (fnmatch(pat->glob, ent->d_name, 0) != 0)
                continue;

            // Build the full original path
            char original[PATH_MAX];
            snprintf(original, sizeof(original), "%s%s", pat->parent, ent->d_name);

            // stat to determine if it's a file or directory
            struct stat sb;
            if (stat(original, &sb) != 0) {
                DEBUG("  stat failed for '%s': %s", original, strerror(errno));
                continue;
            }

            add_mount(original, target_dir, ent->d_name, S_ISDIR(sb.st_mode));
        }

        closedir(dp);
    }
}

/*** Target creation ******************************/

// For each mount entry, ensure the target path exists.
// Directories are created with mkdir -p.  Files are created with open().
static void create_targets(void) {
    for (int i = 0; i < g_num_mounts; i++) {
        mount_entry_t *m = &g_mounts[i];

        if (m->is_dir) {
            mkdirs(m->target, 0755);
            DEBUG("created target dir: %s", m->target);
        } else {
            // Ensure parent directory exists
            char parent[PATH_MAX];
            strncpy(parent, m->target, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = '\0';
                mkdirs(parent, 0755);
            }

            // Create empty file if it doesn't exist (touch)
            int fd = open(m->target, O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) {
                close(fd);
                DEBUG("created target file: %s", m->target);
            } else {
                fprintf(stderr, "remapper: cannot create %s: %s\n",
                        m->target, strerror(errno));
            }
        }
    }
}

/*** Namespace setup ******************************/

// Write a single string to a file.  Used for /proc/self/uid_map etc.
static int write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(data);
    ssize_t written = write(fd, data, (size_t)len);
    close(fd);
    return (written == len) ? 0 : -1;
}

// Enter a new user + mount namespace and set up UID/GID mappings.
//
// unshare(CLONE_NEWUSER) creates a new user namespace where this process
// has full capabilities (including CAP_SYS_ADMIN for mounting).  No root
// privileges are needed — the kernel allows any unprivileged user to create
// a user namespace.
//
// unshare(CLONE_NEWNS) creates a private mount table.  Any mounts we make
// are only visible to this process and its children.
//
// After unshare, we must write UID/GID mappings into /proc/self/uid_map and
// /proc/self/gid_map so the kernel knows how to translate our real UID/GID
// into the namespace.  Without this, we'd appear as "nobody" (65534).
//
// We also write "deny" to /proc/self/setgroups, which is required by the
// kernel before writing gid_map in an unprivileged user namespace (prevents
// a process from granting itself supplementary groups it doesn't have).
static int setup_namespace(void) {
    uid_t uid = getuid();
    gid_t gid = getgid();

    // Create new user namespace + mount namespace in one call.
    // CLONE_NEWUSER: new user namespace (gives us CAP_SYS_ADMIN inside it)
    // CLONE_NEWNS:   new mount namespace (private mount table)
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        fprintf(stderr, "remapper: unshare(CLONE_NEWUSER | CLONE_NEWNS) failed: %s\n",
                strerror(errno));
        if (errno == EPERM) {
            fprintf(stderr,
                "  Unprivileged user namespaces may be disabled on this system.\n"
                "  Try: sudo sysctl -w kernel.unprivileged_userns_clone=1\n");
        }
        return -1;
    }

    // Deny setgroups — required before writing gid_map in an unprivileged
    // user namespace.  This prevents the process from calling setgroups()
    // to grant itself supplementary groups it shouldn't have.
    if (write_file("/proc/self/setgroups", "deny") != 0) {
        // Some kernels don't have this file (pre-3.19); continue anyway
        DEBUG("warning: could not write /proc/self/setgroups: %s", strerror(errno));
    }

    // Map our real UID to UID 0 inside the namespace.
    // Format: "<inner_uid> <outer_uid> <count>"
    // This means: UID 0 inside the namespace corresponds to our real UID outside.
    // We map to 0 because we need CAP_SYS_ADMIN (root) inside the namespace
    // to perform bind mounts.
    char uid_map[64];
    snprintf(uid_map, sizeof(uid_map), "0 %u 1", uid);
    if (write_file("/proc/self/uid_map", uid_map) != 0) {
        fprintf(stderr, "remapper: failed to write uid_map: %s\n", strerror(errno));
        return -1;
    }

    // Same for GID mapping.
    char gid_map[64];
    snprintf(gid_map, sizeof(gid_map), "0 %u 1", gid);
    if (write_file("/proc/self/gid_map", gid_map) != 0) {
        fprintf(stderr, "remapper: failed to write gid_map: %s\n", strerror(errno));
        return -1;
    }

    DEBUG("namespace created: uid %u -> 0, gid %u -> 0", uid, gid);
    return 0;
}

/*** Bind mounts **********************************/

// Perform bind mounts: for each entry, mount the target path over the
// original path.
//
// A bind mount (MS_BIND) makes a file or directory appear at a second
// location in the filesystem tree.  Unlike symlinks, bind mounts are
// transparent to applications — they see the mounted content as if it
// were the original path.
//
// Because we're inside a private mount namespace (from setup_namespace),
// these mounts are completely invisible to other processes on the system.
// When this process exits, the namespace is destroyed and the mounts
// vanish automatically.
static int perform_mounts(void) {
    for (int i = 0; i < g_num_mounts; i++) {
        mount_entry_t *m = &g_mounts[i];

        // Ensure the original path exists as a mount point.
        // Bind mounts require the target (mount point) to exist.
        if (m->is_dir) {
            mkdirs(m->original, 0755);
        } else {
            // For files, ensure parent exists and create an empty file
            // if the original doesn't exist (so we have a mount point).
            struct stat sb;
            if (stat(m->original, &sb) != 0) {
                char parent[PATH_MAX];
                strncpy(parent, m->original, sizeof(parent) - 1);
                parent[sizeof(parent) - 1] = '\0';
                char *slash = strrchr(parent, '/');
                if (slash) {
                    *slash = '\0';
                    mkdirs(parent, 0755);
                }
                int fd = open(m->original, O_CREAT | O_WRONLY, 0644);
                if (fd >= 0) close(fd);
            }
        }

        // MS_BIND: create a bind mount — the target directory/file appears
        //          at the original path location.
        // MS_REC:  if the target is a directory, also bind-mount any
        //          sub-mounts within it (recursive bind).
        if (mount(m->target, m->original, NULL, MS_BIND | MS_REC, NULL) != 0) {
            fprintf(stderr, "remapper: bind mount %s -> %s failed: %s\n",
                    m->target, m->original, strerror(errno));
            return -1;
        }

        DEBUG("mounted: %s -> %s", m->target, m->original);
    }

    return 0;
}

/*** Main *****************************************/

int main(int argc, char **argv) {
    char *target;
    const char *debug_log;
    pattern_t patterns[MAX_PATTERNS];
    int num_patterns = 0;

    int cmd_start = parse_args(argc, argv, &target, &debug_log,
                               patterns, &num_patterns);

    // Open debug log
    if (debug_log) {
        g_debug_fp = fopen(debug_log, "we");
        if (!g_debug_fp) g_debug_fp = stderr;
    }

    DEBUG("target: %s", target);
    for (int i = 0; i < num_patterns; i++)
        DEBUG("pattern[%d]: parent='%s' glob='%s'",
              i, patterns[i].parent, patterns[i].glob);
    DEBUG("command:");
    for (int i = cmd_start; i < argc; i++)
        DEBUG("  argv[%d] = '%s'", i - cmd_start, argv[i]);

    // Step 1: Scan the filesystem to find entries matching our glob patterns.
    // We enumerate matches BEFORE entering the namespace because the program
    // must have been run at least once to create its config files/dirs.
    resolve_globs(patterns, num_patterns, target);

    if (g_num_mounts == 0) {
        DEBUG("no matching paths found — executing without remapping");
        fprintf(stderr,
            "remapper: warning: no paths matched the given patterns.\n"
            "  Has the program been run at least once to create its config files?\n"
            "  Executing without remapping.\n");
        execvp(argv[cmd_start], &argv[cmd_start]);
        perror(argv[cmd_start]);
        return 127;
    }

    DEBUG("%d mount(s) to set up", g_num_mounts);

    // Step 2: Create target files/directories so we have content to mount.
    // If a target file doesn't exist yet, we create an empty one.
    // If a target directory doesn't exist, we mkdir it.
    create_targets();

    // Step 3: Enter a new user + mount namespace.
    // This gives us a private mount table and the ability to perform
    // bind mounts without root privileges.
    if (setup_namespace() != 0) {
        fprintf(stderr, "remapper: failed to set up namespace\n");
        return 1;
    }

    // Step 4: Bind-mount each target path over the original.
    // After this, any access to the original path (by this process or
    // its children) will transparently see the target content instead.
    if (perform_mounts() != 0) {
        fprintf(stderr, "remapper: failed to set up bind mounts\n");
        return 1;
    }

    // Step 5: Exec the program.  It inherits our mount namespace, so it
    // (and all its children) will see the remapped paths.
    DEBUG("exec: %s", argv[cmd_start]);
    execvp(argv[cmd_start], &argv[cmd_start]);
    perror(argv[cmd_start]);
    return 127;
}
