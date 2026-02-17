/*
 * remapper - redirect filesystem paths for any program on macOS
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
#include "rmp_shared.h"

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

// Open debug log for appending (or writing if first call).
// Returns NULL if debug is not enabled.
static FILE *open_debug_log(const char *debug_log, const char *mode) {
    if (!debug_log) return NULL;
    FILE *fp = fopen(debug_log, mode);
    return fp;
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

    // Locate the interpose dylib next to this executable
    char exe_path[PATH_MAX];
    uint32_t exe_size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &exe_size) != 0) {
        fprintf(stderr, "Error: cannot determine executable path\n");
        exit(1);
    }
    char real_exe[PATH_MAX];
    if (!realpath(exe_path, real_exe)) {
        strncpy(real_exe, exe_path, sizeof(real_exe) - 1);
    }

    char *exe_dir = dirname(real_exe);
    char dylib_path[PATH_MAX];
    snprintf(dylib_path, sizeof(dylib_path), "%s/interpose.dylib", exe_dir);

    if (access(dylib_path, R_OK) != 0) {
        fprintf(stderr, "Error: cannot find interpose.dylib at %s\n", dylib_path);
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

    if (debug_log) {
        FILE *dfp = open_debug_log(debug_log, "w");
        if (!dfp) dfp = stderr;

        fprintf(dfp, "[remapper] target:   %s\n", target);
        fprintf(dfp, "[remapper] mappings: %s\n", mappings_buf);
        fprintf(dfp, "[remapper] config:   %s\n", config_dir);
        fprintf(dfp, "[remapper] cache:    %s\n", cache_dir);
        fprintf(dfp, "[remapper] dylib:    %s\n", dylib_path);
        fprintf(dfp, "[remapper] command: ");
        for (int i = cmd_start; i < argc; i++)
            fprintf(dfp, " %s", argv[i]);
        fprintf(dfp, "\n");

        // Check dylib arch
        char cmd_buf[PATH_MAX + 64];
        snprintf(cmd_buf, sizeof(cmd_buf), "file '%s' 2>&1", dylib_path);
        FILE *p = popen(cmd_buf, "r");
        if (p) {
            char line[1024];
            if (fgets(line, sizeof(line), p))
                fprintf(dfp, "[remapper] dylib:    %s", line);
            pclose(p);
        }

        // Resolve the target binary and check its arch/signing
        snprintf(cmd_buf, sizeof(cmd_buf), "which '%s' 2>/dev/null", argv[cmd_start]);
        p = popen(cmd_buf, "r");
        char resolved_cmd[PATH_MAX] = "";
        if (p) {
            if (fgets(resolved_cmd, sizeof(resolved_cmd), p))
                resolved_cmd[strcspn(resolved_cmd, "\n")] = '\0';
            pclose(p);
        }
        if (resolved_cmd[0]) {
            snprintf(cmd_buf, sizeof(cmd_buf), "file '%s' 2>&1", resolved_cmd);
            p = popen(cmd_buf, "r");
            if (p) {
                char line[1024];
                if (fgets(line, sizeof(line), p))
                    fprintf(dfp, "[remapper] binary:   %s", line);
                pclose(p);
            }
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "codesign -dvvv '%s' 2>&1 | grep -E '(runtime|Signature)' || echo 'not signed'",
                     resolved_cmd);
            p = popen(cmd_buf, "r");
            if (p) {
                char line[1024];
                while (fgets(line, sizeof(line), p))
                    fprintf(dfp, "[remapper] codesign: %s", line);
                pclose(p);
            }
        }

        fflush(dfp);
        if (dfp != stderr) fclose(dfp);
    }

    /*** Shebang resolution ************************/
    //
    // If the command is a script with #!/usr/bin/env <prog>, SIP will
    // strip DYLD_INSERT_LIBRARIES when /usr/bin/env runs.  Detect this
    // and exec the interpreter directly instead.

    // Resolve the command to a full path
    char cmd_resolved[PATH_MAX] = "";
    if (argv[cmd_start][0] == '/') {
        strncpy(cmd_resolved, argv[cmd_start], sizeof(cmd_resolved) - 1);
    } else {
        char *path_env = getenv("PATH");
        if (path_env) {
            char *path_copy = strdup(path_env);
            char *saveptr = NULL;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir) {
                char try[PATH_MAX];
                snprintf(try, sizeof(try), "%s/%s", dir, argv[cmd_start]);
                if (access(try, X_OK) == 0) {
                    strncpy(cmd_resolved, try, sizeof(cmd_resolved) - 1);
                    break;
                }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
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

                        if (debug_log) {
                            FILE *dfp = open_debug_log(debug_log, "a");
                            if (!dfp) dfp = stderr;
                            fprintf(dfp, "[remapper] shebang:  '#!/usr/bin/env %s' → %s\n",
                                    prog_name, interp_resolved);
                            fprintf(dfp, "[remapper] rewritten:");
                            for (int i = 0; exec_argv[i]; i++)
                                fprintf(dfp, " %s", exec_argv[i]);
                            fprintf(dfp, "\n");
                            fflush(dfp);
                            if (dfp != stderr) fclose(dfp);
                        }
                    }
                }
                // #!/path/to/interpreter — check if SIP-protected
                else if (strncmp(interp, "/usr/", 5) == 0 ||
                         strncmp(interp, "/bin/", 5) == 0 ||
                         strncmp(interp, "/sbin/", 6) == 0) {
                    fprintf(stderr,
                        "[remapper] WARNING: %s uses shebang '%s'\n"
                        "  This interpreter is SIP-protected and will strip DYLD_INSERT_LIBRARIES.\n"
                        "  Interposition will NOT work.\n",
                        cmd_resolved, shebang + 2);
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
        FILE *ctx_debug = NULL;
        if (debug_log) {
            ctx_debug = open_debug_log(debug_log, "a");
        }

        rmp_ctx_t ctx;
        rmp_ctx_init(&ctx, config_dir, cache_dir, ctx_debug);

        int was_cached = 0;
        const char *resolved = rmp_resolve_hardened(&ctx, final_binary, &was_cached);

        if (was_cached) {
            if (debug_log) {
                FILE *dfp = ctx_debug ? ctx_debug : stderr;
                fprintf(dfp, "[remapper] hardened binary detected: %s\n", final_binary);
                fprintf(dfp, "[remapper] using cached copy: %s\n", resolved);
                fflush(dfp);
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

        if (ctx_debug && ctx_debug != stderr) fclose(ctx_debug);
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
