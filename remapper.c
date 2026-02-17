/*
 * remapper - redirect filesystem paths for any program
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
 *   remapper --debug-log /tmp/rmp.log /tmp/test '~/.claude*' '~/.config*' -- claude
 *
 * Mappings must be single-quoted to prevent shell glob expansion.
 *
 * Environment variables:
 *   RMP_CONFIG     Base directory (default: ~/.remapper/)
 *   RMP_CACHE      Cache directory (default: $RMP_CONFIG/cache/) [macOS only]
 *   RMP_DEBUG_LOG  Log file path (enables debug logging when set)
 *   RMP_TARGET     Set by CLI for the interpose library
 *   RMP_MAPPINGS   Set by CLI for the interpose library (colon-separated)
 *
 * The interpose library is embedded inside this binary at build time.
 * On macOS: -sectcreate __DATA __interpose_lib <dylib>
 * On Linux: ld -r -b binary -o interpose_so.o interpose.so
 *
 * This means `remapper` is a single self-contained binary -- no need to
 * keep the interpose library alongside it.  On first run (or when the
 * embedded version changes), we extract it to $RMP_CONFIG/ so that
 * DYLD_INSERT_LIBRARIES / LD_PRELOAD can load it from disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <stdint.h>
#include "rmp_shared.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>

/* Declared by the linker -- the Mach-O header of this executable */
extern const struct mach_header_64 _mh_execute_header;

#define LIB_NAME    "interpose.dylib"
#define LIB_ENVVAR  "DYLD_INSERT_LIBRARIES"

#else /* Linux */

/*
 * The interpose.so is embedded at build time via ld -r -b binary.
 * The linker creates these symbols automatically from the filename.
 */
extern const uint8_t _binary_interpose_so_start[];
extern const uint8_t _binary_interpose_so_end[];

#define LIB_NAME    "interpose.so"
#define LIB_ENVVAR  "LD_PRELOAD"

#endif

#ifdef __APPLE__
/*
 * resolve_sip_shebang - handle #!/path/to/interpreter shebangs on macOS
 *
 * If the interpreter is SIP-protected (/usr/, /bin/, /sbin/) or has
 * hardened runtime, create a cached re-signed copy and rewrite exec_argv
 * to use it.  Returns 1 if exec_argv was rewritten, 0 otherwise.
 */
static int resolve_sip_shebang(rmp_ctx_t *ctx, FILE *debug_fp,
                                const char *interp, const char *cmd_resolved,
                                char **exec_argv, int argc, char **argv,
                                int cmd_start) {
    // Parse interpreter path and optional arg
    static char shebang_interp[PATH_MAX];
    static char shebang_arg_buf[256];
    char *shebang_arg = NULL;
    char *sp = strchr(interp, ' ');
    if (sp) {
        size_t ilen = (size_t)(sp - interp);
        if (ilen >= sizeof(shebang_interp)) ilen = sizeof(shebang_interp) - 1;
        memcpy(shebang_interp, interp, ilen);
        shebang_interp[ilen] = '\0';
        char *a = sp + 1;
        while (*a == ' ') a++;
        if (*a) {
            strncpy(shebang_arg_buf, a, sizeof(shebang_arg_buf) - 1);
            shebang_arg_buf[sizeof(shebang_arg_buf) - 1] = '\0';
            shebang_arg = shebang_arg_buf;
        }
    } else {
        strncpy(shebang_interp, interp, sizeof(shebang_interp) - 1);
        shebang_interp[sizeof(shebang_interp) - 1] = '\0';
    }

    // Check if interpreter needs re-signing
    int need_resign = (strncmp(shebang_interp, "/usr/", 5) == 0 ||
                       strncmp(shebang_interp, "/bin/", 5) == 0 ||
                       strncmp(shebang_interp, "/sbin/", 6) == 0);
    if (!need_resign)
        need_resign = rmp_is_hardened(ctx, shebang_interp);

    if (!need_resign)
        return 0;

    struct stat interp_sb;
    static char cached_interp[PATH_MAX];
    int resign_ok = 0;

    if (stat(shebang_interp, &interp_sb) == 0) {
        rmp_cache_path(ctx->cache_dir, shebang_interp,
                       cached_interp, sizeof(cached_interp));
        if (rmp_cache_valid(cached_interp, interp_sb.st_mtime, interp_sb.st_size) ||
            rmp_cache_create(ctx, shebang_interp, cached_interp,
                             interp_sb.st_mtime, interp_sb.st_size) == 0) {
            resign_ok = 1;
        }
    }

    if (resign_ok) {
        int ai = 0;
        exec_argv[ai++] = cached_interp;
        if (shebang_arg) exec_argv[ai++] = shebang_arg;
        exec_argv[ai++] = (char *)cmd_resolved;
        for (int i = cmd_start + 1; i < argc && ai < 255; i++)
            exec_argv[ai++] = argv[i];
        exec_argv[ai] = NULL;

        if (debug_fp) {
            fprintf(debug_fp, "[remapper] shebang resign: %s → %s\n",
                    shebang_interp, cached_interp);
            fprintf(debug_fp, "[remapper] rewritten:");
            for (int i = 0; exec_argv[i]; i++)
                fprintf(debug_fp, " %s", exec_argv[i]);
            fprintf(debug_fp, "\n");
            fflush(debug_fp);
        }
        return 1;
    }

    fprintf(stderr,
        "[remapper] WARNING: %s has shebang '%s' that needs re-signing\n"
        "  Failed to create cached copy. Interposition may NOT work.\n",
        cmd_resolved, interp);
    return 0;
}
#endif /* __APPLE__ */

/*** Helpers ***********************************/

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
        "  RMP_CONFIG      Base directory (default: ~/.remapper/)\n"
#ifdef __APPLE__
        "  RMP_CACHE       Cache directory (default: $RMP_CONFIG/cache/)\n"
#endif
        "  RMP_DEBUG_LOG   Log file (enables debug when set)\n",
        prog, prog, prog, prog);
    exit(1);
}

/*** Main **************************************/

int main(int argc, char **argv) {
    // Parse optional flags
    int arg_idx = 1;
    const char *debug_log = getenv("RMP_DEBUG_LOG");

    while (arg_idx < argc && argv[arg_idx][0] == '-' && strcmp(argv[arg_idx], "--") != 0) {
        if (strncmp(argv[arg_idx], "--debug-log=", 12) == 0) {
            debug_log = argv[arg_idx] + 12;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--debug-log") == 0 && arg_idx + 1 < argc) {
            debug_log = argv[arg_idx + 1];
            arg_idx += 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[arg_idx]);
            usage(argv[0]);
        }
    }

    // Need at least: target, mapping, command
    if (argc - arg_idx < 3) usage(argv[0]);

    // argv[arg_idx] = target directory
    char *target = make_absolute(argv[arg_idx]);

    // Create target directory if it doesn't exist
    rmp_mkdirs(target, 0755);

    // Find '--' separator
    int sep_idx = -1;
    for (int i = arg_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            sep_idx = i;
            break;
        }
    }

    int map_start = arg_idx + 1;
    int map_end, cmd_start;

    if (sep_idx >= 0) {
        map_end   = sep_idx;          // mappings: argv[map_start .. sep_idx-1]
        cmd_start = sep_idx + 1;      // command:  argv[sep_idx+1 ..]
    } else {
        map_end   = arg_idx + 2;      // single mapping: argv[arg_idx+1]
        cmd_start = arg_idx + 2;      // command: argv[arg_idx+2 ..]
    }

    if (cmd_start >= argc) {
        fprintf(stderr, "Error: no command specified\n\n");
        usage(argv[0]);
    }
    if (map_end <= map_start) {
        fprintf(stderr, "Error: no mappings specified\n\n");
        usage(argv[0]);
    }

    // Build colon-separated mappings string for the interpose library
    char mappings_buf[65536];
    size_t mlen = 0;

    for (int i = map_start; i < map_end; i++) {
        char *pat = strdup(argv[i]);
        if (!pat) { perror("strdup"); exit(1); }

        char *abs = make_absolute(pat);
        free(pat);

        if (mlen > 0) {
            if (mlen + 1 >= sizeof(mappings_buf)) {
                fprintf(stderr, "Error: mappings too long\n");
                exit(1);
            }
            mappings_buf[mlen++] = ':';
        }

        size_t alen = strlen(abs);
        if (mlen + alen >= sizeof(mappings_buf)) {
            fprintf(stderr, "Error: mappings too long\n");
            exit(1);
        }
        memcpy(mappings_buf + mlen, abs, alen);
        mlen += alen;
        free(abs);
    }
    mappings_buf[mlen] = '\0';

    /*** Resolve config/cache directories **********/

    char home_pwbuf[1024];
    const char *home_dir = get_home_dir(home_pwbuf, sizeof(home_pwbuf));

    // RMP_CONFIG defaults to ~/.remapper/
    char config_dir[PATH_MAX];
    const char *cfg_env = getenv("RMP_CONFIG");
    if (cfg_env && cfg_env[0]) {
        char *abs_cfg = make_absolute(cfg_env);
        strncpy(config_dir, abs_cfg, sizeof(config_dir) - 1);
        config_dir[sizeof(config_dir) - 1] = '\0';
        free(abs_cfg);
    } else if (home_dir) {
        snprintf(config_dir, sizeof(config_dir), "%s/.remapper", home_dir);
    } else {
        snprintf(config_dir, sizeof(config_dir), "/tmp/.remapper");
    }

#ifdef __APPLE__
    // RMP_CACHE defaults to $RMP_CONFIG/cache/
    char cache_dir[PATH_MAX];
    const char *cache_env = getenv("RMP_CACHE");
    if (cache_env && cache_env[0]) {
        char *abs_cache = make_absolute(cache_env);
        strncpy(cache_dir, abs_cache, sizeof(cache_dir) - 1);
        cache_dir[sizeof(cache_dir) - 1] = '\0';
        free(abs_cache);
    } else {
        snprintf(cache_dir, sizeof(cache_dir), "%s/cache", config_dir);
    }
#endif

    /*** Extract embedded interpose library ********/
    //
    // We extract it to: $RMP_CONFIG/<LIB_NAME>
    //
    // We only rewrite the file when:
    //   - it doesn't exist on disk yet (first run), OR
    //   - its size differs from the embedded blob (remapper was rebuilt)
    //
    // This avoids unnecessary writes on every invocation while ensuring
    // an updated remapper binary always deploys its matching library.

#ifdef __APPLE__
    // Read the embedded dylib blob from our own Mach-O section.
    // getsectiondata() returns a pointer into our already-mapped
    // executable image -- no allocation or I/O needed.
    unsigned long embed_size = 0;
    const uint8_t *embed_data = getsectiondata(
        &_mh_execute_header, "__DATA", "__interpose_lib", &embed_size);

    if (!embed_data || embed_size == 0) {
        fprintf(stderr,
            "Error: no embedded " LIB_NAME " found in this binary.\n"
            "  The binary may have been built without -sectcreate.\n");
        exit(1);
    }
#else
    // Get the embedded blob pointer and size from linker symbols.
    const uint8_t *embed_data = _binary_interpose_so_start;
    unsigned long embed_size =
        (unsigned long)(_binary_interpose_so_end - _binary_interpose_so_start);

    if (embed_size == 0) {
        fprintf(stderr,
            "Error: no embedded " LIB_NAME " found in this binary.\n"
            "  The binary may have been built without the embed step.\n");
        exit(1);
    }
#endif

    // Determine the output path: $RMP_CONFIG/<LIB_NAME>
    char lib_path[PATH_MAX];
    snprintf(lib_path, sizeof(lib_path), "%s/" LIB_NAME, config_dir);

    // Check if the on-disk copy is already up to date.
    // Compare file size against the embedded blob size -- if they
    // match, the library hasn't changed and we skip the write.
    int need_extract = 0;
    struct stat lib_sb;
    if (stat(lib_path, &lib_sb) != 0) {
        need_extract = 1;
    } else if ((unsigned long)lib_sb.st_size != embed_size) {
        need_extract = 1;
    }

    // Write the embedded library to disk if needed.
    // We write to a temp file and rename() into place so that
    // a concurrent remapper invocation never sees a half-written
    // library (rename is atomic on the same filesystem).
    if (need_extract) {
        rmp_mkdirs(config_dir, 0755);

        char lib_tmp[PATH_MAX];
        snprintf(lib_tmp, sizeof(lib_tmp), "%s.tmp.%d", lib_path, getpid());

        int dfd = open(lib_tmp, O_CREAT | O_WRONLY | O_TRUNC, 0755);
        if (dfd < 0) {
            fprintf(stderr, "Error: cannot write %s: %s\n",
                    lib_tmp, strerror(errno));
            exit(1);
        }

        const uint8_t *p = embed_data;
        unsigned long remaining = embed_size;
        while (remaining > 0) {
            ssize_t written = write(dfd, p, remaining);
            if (written <= 0) {
                fprintf(stderr, "Error: write to %s failed: %s\n",
                        lib_tmp, strerror(errno));
                close(dfd);
                unlink(lib_tmp);
                exit(1);
            }
            p += written;
            remaining -= (unsigned long)written;
        }
        close(dfd);

        if (rename(lib_tmp, lib_path) != 0) {
            fprintf(stderr, "Error: cannot install %s: %s\n",
                    lib_path, strerror(errno));
            unlink(lib_tmp);
            exit(1);
        }
    }

    // Open debug log
    FILE *debug_fp = NULL;
    if (debug_log) {
        debug_fp = fopen(debug_log, "we");
        if (!debug_fp) debug_fp = stderr;
    }

#ifdef __APPLE__
    // Initialize shared context (resolves codesign, creates dirs, writes entitlements)
    rmp_ctx_t ctx;
    rmp_ctx_init(&ctx, config_dir, cache_dir, debug_fp);

    if (!ctx.codesign_path[0]) {
        fprintf(stderr, "Error: cannot find 'codesign' in PATH\n");
        exit(1);
    }
#endif

    /*** Set environment variables *****************/

    setenv("RMP_TARGET", target, 1);
    setenv("RMP_MAPPINGS", mappings_buf, 1);

#ifdef __APPLE__
    setenv("DYLD_INSERT_LIBRARIES", lib_path, 1);
    setenv("RMP_CACHE", cache_dir, 1);
#else
    // LD_PRELOAD: prepend our .so to any existing value
    const char *existing_preload = getenv("LD_PRELOAD");
    if (existing_preload && existing_preload[0]) {
        char preload_buf[PATH_MAX * 2];
        snprintf(preload_buf, sizeof(preload_buf), "%s:%s", lib_path, existing_preload);
        setenv("LD_PRELOAD", preload_buf, 1);
    } else {
        setenv("LD_PRELOAD", lib_path, 1);
    }
#endif

    setenv("RMP_CONFIG", config_dir, 1);

    if (debug_log) {
        setenv("RMP_DEBUG_LOG", debug_log, 1);
    }

    /*** Debug output ******************************/

    if (debug_fp) {
        fprintf(debug_fp, "[remapper] target:   %s\n", target);
        fprintf(debug_fp, "[remapper] mappings: %s\n", mappings_buf);
        fprintf(debug_fp, "[remapper] config:   %s\n", config_dir);
#ifdef __APPLE__
        fprintf(debug_fp, "[remapper] cache:    %s\n", cache_dir);
        fprintf(debug_fp, "[remapper] dylib:    %s\n", lib_path);
        fprintf(debug_fp, "[remapper] codesign: %s\n", ctx.codesign_path);
#else
        fprintf(debug_fp, "[remapper] so:       %s\n", lib_path);
#endif
        fprintf(debug_fp, "[remapper] command: ");
        for (int i = cmd_start; i < argc; i++)
            fprintf(debug_fp, " %s", argv[i]);
        fprintf(debug_fp, "\n");

#ifdef __APPLE__
        // Check dylib arch
        {
            char *file_argv[] = {"file", lib_path, NULL};
            rmp_pipe_t proc = rmp_pipe_open("/usr/bin/file", file_argv);
            if (proc.fp) {
                char line[1024];
                if (fgets(line, sizeof(line), proc.fp))
                    fprintf(debug_fp, "[remapper] dylib:    %s", line);
                rmp_pipe_close(&proc);
            }
        }

        // Resolve the target binary and check its arch/signing
        char resolved_cmd[PATH_MAX] = "";
        {
            char *which_argv[] = {"which", argv[cmd_start], NULL};
            rmp_pipe_t proc = rmp_pipe_open("/usr/bin/which", which_argv);
            if (proc.fp) {
                if (fgets(resolved_cmd, sizeof(resolved_cmd), proc.fp))
                    resolved_cmd[strcspn(resolved_cmd, "\n")] = '\0';
                rmp_pipe_close(&proc);
            }
        }
        if (resolved_cmd[0]) {
            char *file2_argv[] = {"file", resolved_cmd, NULL};
            rmp_pipe_t proc = rmp_pipe_open("/usr/bin/file", file2_argv);
            if (proc.fp) {
                char line[1024];
                if (fgets(line, sizeof(line), proc.fp))
                    fprintf(debug_fp, "[remapper] binary:   %s", line);
                rmp_pipe_close(&proc);
            }

            char *cs_argv[] = {"codesign", "-dvvv", resolved_cmd, NULL};
            rmp_pipe_t proc2 = rmp_pipe_open(ctx.codesign_path, cs_argv);
            if (proc2.fp) {
                char line[1024];
                int found_any = 0;
                while (fgets(line, sizeof(line), proc2.fp)) {
                    if (strstr(line, "runtime") || strstr(line, "Signature")) {
                        fprintf(debug_fp, "[remapper] codesign: %s", line);
                        found_any = 1;
                    }
                }
                if (!found_any)
                    fprintf(debug_fp, "[remapper] codesign: not signed\n");
                rmp_pipe_close(&proc2);
            }
        }
#endif /* __APPLE__ */

        fflush(debug_fp);
    }

    /*** Shebang resolution ************************/
    //
    // If the command is a script with #!/usr/bin/env <prog>, resolve it
    // so we exec the interpreter directly.
    //
    // On macOS this is critical: SIP will strip DYLD_INSERT_LIBRARIES
    // when /usr/bin/env runs.  On Linux it's useful for debug logging
    // to show the real interpreter path.

    // Resolve the command to a full (absolute) path
    char cmd_resolved[PATH_MAX] = "";
    if (strchr(argv[cmd_start], '/')) {
        // Contains '/' — absolute or relative path; resolve to absolute
        if (!realpath(argv[cmd_start], cmd_resolved))
            cmd_resolved[0] = '\0';
    } else {
        // Bare filename — search PATH
        resolve_in_path(argv[cmd_start], cmd_resolved, sizeof(cmd_resolved));
    }

    // Check if it's a script with a shebang
    char *exec_argv[256];
    int use_rewritten = 0;

    if (cmd_resolved[0]) {
        int fd = open(cmd_resolved, O_RDONLY);
        if (fd >= 0) {
            char shebang[512];
            ssize_t n = read(fd, shebang, sizeof(shebang) - 1);
            close(fd);
            if (n > 2 && shebang[0] == '#' && shebang[1] == '!') {
                shebang[n] = '\0';
                char *nl = strchr(shebang, '\n');
                if (nl) *nl = '\0';

                char *interp = shebang + 2;
                while (*interp == ' ') interp++;

                // Handle #!/usr/bin/env <prog> [args]
                if (strncmp(interp, "/usr/bin/env ", 13) == 0) {
                    char *prog = interp + 13;
                    while (*prog == ' ') prog++;

                    char *space = strchr(prog, ' ');
                    char prog_name[256];
                    if (space) {
                        size_t pn_len = (size_t)(space - prog);
                        if (pn_len >= sizeof(prog_name)) pn_len = sizeof(prog_name) - 1;
                        memcpy(prog_name, prog, pn_len);
                        prog_name[pn_len] = '\0';
                    } else {
                        strncpy(prog_name, prog, sizeof(prog_name) - 1);
                        prog_name[sizeof(prog_name) - 1] = '\0';
                    }

                    // Resolve prog_name via PATH
                    // Must be static: exec_argv holds a pointer to this buffer
                    // and execv() runs after this block goes out of scope.
                    static char interp_resolved[PATH_MAX] = "";
                    resolve_in_path(prog_name, interp_resolved, sizeof(interp_resolved));

                    if (interp_resolved[0]) {
                        int ai = 0;
                        exec_argv[ai++] = interp_resolved;

                        // extra arg (e.g. #!/usr/bin/env -S node) — copy
                        // to a static buffer since shebang[] is stack-local
                        static char env_extra_buf[256];
                        if (space) {
                            char *extra = space + 1;
                            while (*extra == ' ') extra++;
                            if (*extra) {
                                strncpy(env_extra_buf, extra, sizeof(env_extra_buf) - 1);
                                env_extra_buf[sizeof(env_extra_buf) - 1] = '\0';
                                exec_argv[ai++] = env_extra_buf;
                            }
                        }

                        exec_argv[ai++] = cmd_resolved;

                        for (int i = cmd_start + 1; i < argc && ai < 255; i++)
                            exec_argv[ai++] = argv[i];
                        exec_argv[ai] = NULL;

                        use_rewritten = 1;

                        if (debug_fp) {
                            fprintf(debug_fp, "[remapper] shebang:  '#!/usr/bin/env %s' → %s\n",
                                    prog_name, interp_resolved);
                            fprintf(debug_fp, "[remapper] rewritten:");
                            for (int i = 0; exec_argv[i]; i++)
                                fprintf(debug_fp, " %s", exec_argv[i]);
                            fprintf(debug_fp, "\n");
                            fflush(debug_fp);
                        }
                    }
                }
#ifdef __APPLE__
                // #!/path/to/interpreter — if SIP-protected or hardened, copy+re-sign
                else {
                    use_rewritten = resolve_sip_shebang(
                        &ctx, debug_fp, interp, cmd_resolved,
                        exec_argv, argc, argv, cmd_start);
                }
#endif /* __APPLE__ */
            }
        }
    }

#ifdef __APPLE__
    /*** Hardened binary check *********************/
    //
    // If the binary to exec has hardened runtime without the
    // allow-dyld-environment-variables entitlement, DYLD_INSERT_LIBRARIES
    // will be silently stripped.  Create a cached re-signed copy.

    const char *final_binary = use_rewritten ? exec_argv[0] : cmd_resolved;

    if (final_binary[0]) {
        int was_cached = 0;
        const char *resolved = rmp_resolve_hardened(&ctx, final_binary, &was_cached);

        if (was_cached) {
            if (debug_fp) {
                fprintf(debug_fp, "[remapper] hardened binary detected: %s\n", final_binary);
                fprintf(debug_fp, "[remapper] using cached copy: %s\n", resolved);
                fflush(debug_fp);
            }

            // Update the exec target
            if (use_rewritten) {
                exec_argv[0] = (char *)resolved;
            } else {
                int ai = 0;
                exec_argv[ai++] = (char *)resolved;
                for (int i = cmd_start + 1; i < argc && ai < 255; i++)
                    exec_argv[ai++] = argv[i];
                exec_argv[ai] = NULL;
                use_rewritten = 1;
            }
        }
    }
#endif /* __APPLE__ */

    // Exec the command
    if (use_rewritten) {
        execv(exec_argv[0], exec_argv);
        perror(exec_argv[0]);
    } else {
        execvp(argv[cmd_start], &argv[cmd_start]);
        perror(argv[cmd_start]);
    }

    free(target);
    return 127;
}
