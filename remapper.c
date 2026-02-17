/*
 * remapper - redirect filesystem paths for any program on macOS
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
 *   RMP_CACHE      Cache directory (default: $RMP_CONFIG/cache/)
 *   RMP_DEBUG_LOG  Log file path (enables debug logging when set)
 *   RMP_TARGET     Set by CLI for the dylib
 *   RMP_MAPPINGS   Set by CLI for the dylib (colon-separated)
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
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <errno.h>
#include "rmp_shared.h"

/*
 * The interpose.dylib is embedded inside this binary at build time using
 * the linker flag: -sectcreate __DATA __interpose_lib <dylib-file>
 *
 * This means `remapper` is a single self-contained binary -- no need to
 * keep interpose.dylib alongside it.  On first run (or when the embedded
 * version changes), we extract the dylib to $RMP_CONFIG/interpose.dylib
 * so that DYLD_INSERT_LIBRARIES can load it from disk.
 *
 * Why not load it directly from memory?  DYLD_INSERT_LIBRARIES requires
 * a filesystem path -- the kernel needs to mmap the dylib from a real file.
 *
 * We use getsectiondata() to get a pointer to the embedded blob and its
 * size, then write it to disk only when the on-disk copy is missing or
 * has a different size (i.e., after rebuilding remapper with a new dylib).
 */

/* Declared by the linker -- the Mach-O header of this executable */
extern const struct mach_header_64 _mh_execute_header;

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
        "  RMP_CACHE       Cache directory (default: $RMP_CONFIG/cache/)\n"
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

    // Build colon-separated mappings string for the dylib
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

    /*** Extract embedded interpose.dylib **********/
    //
    // The dylib is embedded in this binary's __DATA,__interpose_lib section
    // (put there at build time by the linker's -sectcreate flag).
    //
    // We extract it to: $RMP_CONFIG/interpose.dylib
    //
    // We only rewrite the file when:
    //   - it doesn't exist on disk yet (first run), OR
    //   - its size differs from the embedded blob (remapper was rebuilt)
    //
    // This avoids unnecessary writes on every invocation while ensuring
    // an updated remapper binary always deploys its matching dylib.

    // Step 1: Read the embedded dylib blob from our own Mach-O section.
    //         getsectiondata() returns a pointer into our already-mapped
    //         executable image -- no allocation or I/O needed.
    unsigned long embed_size = 0;
    const uint8_t *embed_data = getsectiondata(
        &_mh_execute_header, "__DATA", "__interpose_lib", &embed_size);

    if (!embed_data || embed_size == 0) {
        fprintf(stderr,
            "Error: no embedded interpose.dylib found in this binary.\n"
            "  The binary may have been built without -sectcreate.\n");
        exit(1);
    }

    // Step 2: Determine the output path: $RMP_CONFIG/interpose.dylib
    char dylib_path[PATH_MAX];
    snprintf(dylib_path, sizeof(dylib_path), "%s/interpose.dylib", config_dir);

    // Step 3: Check if the on-disk copy is already up to date.
    //         Compare file size against the embedded blob size -- if they
    //         match, the dylib hasn't changed and we skip the write.
    int need_extract = 0;
    struct stat dylib_sb;
    if (stat(dylib_path, &dylib_sb) != 0) {
        // File doesn't exist -- need to create it
        need_extract = 1;
    } else if ((unsigned long)dylib_sb.st_size != embed_size) {
        // File exists but size differs -- remapper was rebuilt with a
        // new dylib, so overwrite the stale copy
        need_extract = 1;
    }

    // Step 4: Write the embedded dylib to disk if needed.
    //         We write to a temp file and rename() into place so that
    //         a concurrent remapper invocation never sees a half-written
    //         dylib (rename is atomic on the same filesystem).
    if (need_extract) {
        // Ensure the config directory exists
        rmp_mkdirs(config_dir, 0755);

        // Write to a temp file first, then atomically rename into place
        char dylib_tmp[PATH_MAX];
        snprintf(dylib_tmp, sizeof(dylib_tmp), "%s.tmp.%d", dylib_path, getpid());

        int dfd = open(dylib_tmp, O_CREAT | O_WRONLY | O_TRUNC, 0755);
        if (dfd < 0) {
            fprintf(stderr, "Error: cannot write %s: %s\n",
                    dylib_tmp, strerror(errno));
            exit(1);
        }

        // Write the entire blob in a loop to handle partial writes
        const uint8_t *p = embed_data;
        unsigned long remaining = embed_size;
        while (remaining > 0) {
            ssize_t written = write(dfd, p, remaining);
            if (written <= 0) {
                fprintf(stderr, "Error: write to %s failed: %s\n",
                        dylib_tmp, strerror(errno));
                close(dfd);
                unlink(dylib_tmp);
                exit(1);
            }
            p += written;
            remaining -= (unsigned long)written;
        }
        close(dfd);

        // Atomic rename: if two remapper processes race, the last one
        // wins (both have identical content, so this is harmless)
        if (rename(dylib_tmp, dylib_path) != 0) {
            fprintf(stderr, "Error: cannot install %s: %s\n",
                    dylib_path, strerror(errno));
            unlink(dylib_tmp);
            exit(1);
        }
    }

    // Initialize shared context (resolves codesign, creates dirs, writes entitlements)
    FILE *debug_fp = NULL;
    if (debug_log) {
        debug_fp = fopen(debug_log, "w");
        if (!debug_fp) debug_fp = stderr;
    }
    rmp_ctx_t ctx;
    rmp_ctx_init(&ctx, config_dir, cache_dir, debug_fp);

    if (!ctx.codesign_path[0]) {
        fprintf(stderr, "Error: cannot find 'codesign' in PATH\n");
        exit(1);
    }

    /*** Set environment variables *****************/

    setenv("RMP_TARGET", target, 1);
    setenv("RMP_MAPPINGS", mappings_buf, 1);
    setenv("DYLD_INSERT_LIBRARIES", dylib_path, 1);

    // Propagate config/cache dirs so the dylib uses the same ones
    setenv("RMP_CONFIG", config_dir, 1);
    setenv("RMP_CACHE", cache_dir, 1);

    // Propagate debug log if set via CLI flag
    if (debug_log) {
        setenv("RMP_DEBUG_LOG", debug_log, 1);
    }

    /*** Debug output ******************************/

    if (debug_fp) {
        fprintf(debug_fp, "[remapper] target:   %s\n", target);
        fprintf(debug_fp, "[remapper] mappings: %s\n", mappings_buf);
        fprintf(debug_fp, "[remapper] config:   %s\n", config_dir);
        fprintf(debug_fp, "[remapper] cache:    %s\n", cache_dir);
        fprintf(debug_fp, "[remapper] dylib:    %s\n", dylib_path);
        fprintf(debug_fp, "[remapper] codesign: %s\n", ctx.codesign_path);
        fprintf(debug_fp, "[remapper] command: ");
        for (int i = cmd_start; i < argc; i++)
            fprintf(debug_fp, " %s", argv[i]);
        fprintf(debug_fp, "\n");

        // Check dylib arch
        {
            char *file_argv[] = {"file", dylib_path, NULL};
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

        fflush(debug_fp);
    }

    /*** Shebang resolution ************************/
    //
    // If the command is a script with #!/usr/bin/env <prog>, SIP will
    // strip DYLD_INSERT_LIBRARIES when /usr/bin/env runs.  Detect this
    // and exec the interpreter directly instead.

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
                    char interp_resolved[PATH_MAX] = "";
                    char *path_env2 = getenv("PATH");
                    if (path_env2) {
                        char *pc = strdup(path_env2);
                        char *sp = NULL;
                        char *d = strtok_r(pc, ":", &sp);
                        while (d) {
                            char try[PATH_MAX];
                            snprintf(try, sizeof(try), "%s/%s", d, prog_name);
                            if (access(try, X_OK) == 0) {
                                strncpy(interp_resolved, try, sizeof(interp_resolved) - 1);
                                break;
                            }
                            d = strtok_r(NULL, ":", &sp);
                        }
                        free(pc);
                    }

                    if (interp_resolved[0]) {
                        int ai = 0;
                        exec_argv[ai++] = interp_resolved;

                        if (space) {
                            char *extra = space + 1;
                            while (*extra == ' ') extra++;
                            if (*extra) exec_argv[ai++] = extra;
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
                // #!/path/to/interpreter — if SIP-protected or hardened, copy+re-sign
                else {
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
                        need_resign = rmp_is_hardened(&ctx, shebang_interp);

                    if (need_resign) {
                        struct stat interp_sb;
                        static char cached_interp[PATH_MAX];
                        int resign_ok = 0;

                        if (stat(shebang_interp, &interp_sb) == 0) {
                            rmp_cache_path(ctx.cache_dir, shebang_interp,
                                           cached_interp, sizeof(cached_interp));
                            if (rmp_cache_valid(cached_interp, interp_sb.st_mtime, interp_sb.st_size) ||
                                rmp_cache_create(&ctx, shebang_interp, cached_interp,
                                                 interp_sb.st_mtime, interp_sb.st_size) == 0) {
                                resign_ok = 1;
                            }
                        }

                        if (resign_ok) {
                            int ai = 0;
                            exec_argv[ai++] = cached_interp;
                            if (shebang_arg) exec_argv[ai++] = shebang_arg;
                            exec_argv[ai++] = cmd_resolved;
                            for (int i = cmd_start + 1; i < argc && ai < 255; i++)
                                exec_argv[ai++] = argv[i];
                            exec_argv[ai] = NULL;
                            use_rewritten = 1;

                            if (debug_fp) {
                                fprintf(debug_fp, "[remapper] shebang resign: %s → %s\n",
                                        shebang_interp, cached_interp);
                                fprintf(debug_fp, "[remapper] rewritten:");
                                for (int i = 0; exec_argv[i]; i++)
                                    fprintf(debug_fp, " %s", exec_argv[i]);
                                fprintf(debug_fp, "\n");
                                fflush(debug_fp);
                            }
                        } else {
                            fprintf(stderr,
                                "[remapper] WARNING: %s has shebang '%s' that needs re-signing\n"
                                "  Failed to create cached copy. Interposition may NOT work.\n",
                                cmd_resolved, interp);
                        }
                    }
                }
            }
        }
    }

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
