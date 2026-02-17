/*
 * interpose_exec_linux.c - Exec/spawn interpose functions for Linux (LD_PRELOAD)
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Linux doesn't have hardened runtime or SIP, so these wrappers just
 * log the call (if debug is enabled) and pass through to the real function.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include "interpose.h"

/*** Real function pointers (resolved at load time) ***/

static int (*real_posix_spawn)(pid_t *, const char *,
                               const posix_spawn_file_actions_t *,
                               const posix_spawnattr_t *,
                               char *const[], char *const[]);
static int (*real_posix_spawnp)(pid_t *, const char *,
                                const posix_spawn_file_actions_t *,
                                const posix_spawnattr_t *,
                                char *const[], char *const[]);
static int (*real_execve)(const char *, char *const[], char *const[]);
static int (*real_execv)(const char *, char *const[]);
static int (*real_execvp)(const char *, char *const[]);

#define RESOLVE(name) real_##name = dlsym(RTLD_NEXT, #name)

// Honestly just to get rid of the intellisense warnings about constructor 
// not taking arguments in Darwin. 
#ifdef __linux__ 
__attribute__((constructor(201)))
static void resolve_exec_symbols(void) {
    RESOLVE(posix_spawn);
    RESOLVE(posix_spawnp);
    RESOLVE(execve);
    RESOLVE(execv);
    RESOLVE(execvp);
}
#endif 
#undef RESOLVE

/*** posix_spawn / posix_spawnp *******************/

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *sa,
                char *const argv[], char *const envp[]) {
    RMP_DEBUG("posix_spawn: %s", path);
    return real_posix_spawn(pid, path, fa, sa, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *sa,
                 char *const argv[], char *const envp[]) {
    RMP_DEBUG("posix_spawnp: %s", file);
    return real_posix_spawnp(pid, file, fa, sa, argv, envp);
}

/*** execve / execv / execvp **********************/

int execve(const char *path, char *const argv[], char *const envp[]) {
    RMP_DEBUG("execve: %s", path);
    return real_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    RMP_DEBUG("execv: %s", path);
    return real_execv(path, argv);
}

int execvp(const char *file, char *const argv[]) {
    RMP_DEBUG("execvp: %s", file);
    return real_execvp(file, argv);
}
