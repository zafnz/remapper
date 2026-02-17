/*
 * interpose_fs_linux.c - Filesystem interpose functions for Linux (LD_PRELOAD)
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include "interpose.h"

/*
 * On Linux we use LD_PRELOAD: export functions with the same name as libc,
 * and call through to the real implementation via dlsym(RTLD_NEXT, ...).
 * All real_* pointers are resolved once in the constructor.
 */

/*** Real function pointers (resolved at load time) ***/

static int    (*real_open)(const char *, int, ...);
static int    (*real_open64)(const char *, int, ...);
static int    (*real_openat)(int, const char *, int, ...);
static int    (*real_openat64)(int, const char *, int, ...);
static int    (*real___xstat)(int, const char *, struct stat *);
static int    (*real___lxstat)(int, const char *, struct stat *);
static int    (*real___fxstatat)(int, int, const char *, struct stat *, int);
static int    (*real_stat)(const char *, struct stat *);
static int    (*real_lstat)(const char *, struct stat *);
static int    (*real_fstatat)(int, const char *, struct stat *, int);
static int    (*real_access)(const char *, int);
static int    (*real_faccessat)(int, const char *, int, int);
static int    (*real_mkdir)(const char *, mode_t);
static int    (*real_mkdirat)(int, const char *, mode_t);
static int    (*real_unlink)(const char *);
static int    (*real_unlinkat)(int, const char *, int);
static int    (*real_rename)(const char *, const char *);
static int    (*real_renameat)(int, const char *, int, const char *);
static int    (*real_rmdir)(const char *);
static DIR   *(*real_opendir)(const char *);
static int    (*real_chdir)(const char *);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);
static int    (*real_chmod)(const char *, mode_t);
static int    (*real_fchmodat)(int, const char *, mode_t, int);
static int    (*real_chown)(const char *, uid_t, gid_t);
static int    (*real_lchown)(const char *, uid_t, gid_t);
static int    (*real_fchownat)(int, const char *, uid_t, gid_t, int);
static int    (*real_symlink)(const char *, const char *);
static int    (*real_symlinkat)(const char *, int, const char *);
static int    (*real_link)(const char *, const char *);
static int    (*real_linkat)(int, const char *, int, const char *, int);
static int    (*real_truncate)(const char *, off_t);
static char  *(*real_realpath)(const char *, char *);
static char  *(*real___realpath_chk)(const char *, char *, size_t);

#define RESOLVE(name) real_##name = dlsym(RTLD_NEXT, #name)

// Honestly just to get rid of the intellisense warnings about constructor 
// not taking arguments in Darwin. 
#ifdef __linux__ 

__attribute__((constructor(200)))
static void resolve_fs_symbols(void) {
    RESOLVE(open);
    RESOLVE(open64);
    RESOLVE(openat);
    RESOLVE(openat64);
    RESOLVE(__xstat);
    RESOLVE(__lxstat);
    RESOLVE(__fxstatat);
    RESOLVE(stat);        /* NULL on older glibc — fallback handled below */
    RESOLVE(lstat);
    RESOLVE(fstatat);
    RESOLVE(access);
    RESOLVE(faccessat);
    RESOLVE(mkdir);
    RESOLVE(mkdirat);
    RESOLVE(unlink);
    RESOLVE(unlinkat);
    RESOLVE(rename);
    RESOLVE(renameat);
    RESOLVE(rmdir);
    RESOLVE(opendir);
    RESOLVE(chdir);
    RESOLVE(readlink);
    RESOLVE(readlinkat);
    RESOLVE(chmod);
    RESOLVE(fchmodat);
    RESOLVE(chown);
    RESOLVE(lchown);
    RESOLVE(fchownat);
    RESOLVE(symlink);
    RESOLVE(symlinkat);
    RESOLVE(link);
    RESOLVE(linkat);
    RESOLVE(truncate);
    RESOLVE(realpath);
    RESOLVE(__realpath_chk);
}
#endif 
#undef RESOLVE

/*** open / openat / creat ************************/

int open(const char *path, int flags, ...) {
    REWRITE_1(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_open(actual, flags, mode);
    }
    return real_open(actual, flags);
}

int open64(const char *path, int flags, ...) {
    REWRITE_1(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_open64(actual, flags, mode);
    }
    return real_open64(actual, flags);
}

int openat(int fd, const char *path, int flags, ...) {
    REWRITE_ABS(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_openat(fd, actual, flags, mode);
    }
    return real_openat(fd, actual, flags);
}

int openat64(int fd, const char *path, int flags, ...) {
    REWRITE_ABS(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return real_openat64(fd, actual, flags, mode);
    }
    return real_openat64(fd, actual, flags);
}

int creat(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return real_open(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int creat64(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return real_open64(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/*** stat / lstat / fstatat ***********************/

int __xstat(int ver, const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return real___xstat(ver, actual, sb);
}

int __lxstat(int ver, const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return real___lxstat(ver, actual, sb);
}

int __fxstatat(int ver, int fd, const char *path, struct stat *sb, int flag) {
    REWRITE_ABS(actual, path);
    return real___fxstatat(ver, fd, actual, sb, flag);
}

/* glibc >= 2.33 (e.g. Ubuntu 22.04+) also exposes direct stat/lstat/fstatat */
int stat(const char *path, struct stat *sb) {
    if (!real_stat) {
        /* Fallback: older glibc only has __xstat */
        return __xstat(1, path, sb);
    }
    REWRITE_1(actual, path);
    return real_stat(actual, sb);
}

int lstat(const char *path, struct stat *sb) {
    if (!real_lstat) {
        return __lxstat(1, path, sb);
    }
    REWRITE_1(actual, path);
    return real_lstat(actual, sb);
}

int fstatat(int fd, const char *path, struct stat *sb, int flag) {
    if (!real_fstatat) {
        return __fxstatat(1, fd, path, sb, flag);
    }
    REWRITE_ABS(actual, path);
    return real_fstatat(fd, actual, sb, flag);
}

/*** access / faccessat ***************************/

int access(const char *path, int mode) {
    REWRITE_1(actual, path);
    return real_access(actual, mode);
}

int faccessat(int fd, const char *path, int mode, int flag) {
    REWRITE_ABS(actual, path);
    return real_faccessat(fd, actual, mode, flag);
}

/*** mkdir / mkdirat ******************************/

int mkdir(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return real_mkdir(actual, mode);
}

int mkdirat(int fd, const char *path, mode_t mode) {
    REWRITE_ABS(actual, path);
    return real_mkdirat(fd, actual, mode);
}

/*** unlink / unlinkat ****************************/

int unlink(const char *path) {
    REWRITE_1(actual, path);
    return real_unlink(actual);
}

int unlinkat(int fd, const char *path, int flag) {
    REWRITE_ABS(actual, path);
    return real_unlinkat(fd, actual, flag);
}

/*** rename / renameat ****************************/

int rename(const char *oldp, const char *newp) {
    REWRITE_1(aold, oldp);
    REWRITE_1(anew, newp);
    return real_rename(aold, anew);
}

int renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    REWRITE_ABS(aold, oldp);
    REWRITE_ABS(anew, newp);
    return real_renameat(ofd, aold, nfd, anew);
}

/*** rmdir ****************************************/

int rmdir(const char *path) {
    REWRITE_1(actual, path);
    return real_rmdir(actual);
}

/*** opendir **************************************/

DIR *opendir(const char *path) {
    REWRITE_1(actual, path);
    return real_opendir(actual);
}

/*** chdir ****************************************/

int chdir(const char *path) {
    REWRITE_1(actual, path);
    return real_chdir(actual);
}

/*** readlink / readlinkat ************************/

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    REWRITE_1(actual, path);
    return real_readlink(actual, buf, bufsiz);
}

ssize_t readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    REWRITE_ABS(actual, path);
    return real_readlinkat(fd, actual, buf, bufsiz);
}

/*** chmod / fchmodat *****************************/

int chmod(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return real_chmod(actual, mode);
}

int fchmodat(int fd, const char *path, mode_t mode, int flag) {
    REWRITE_ABS(actual, path);
    return real_fchmodat(fd, actual, mode, flag);
}

/*** chown / lchown / fchownat ********************/

int chown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return real_chown(actual, owner, group);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return real_lchown(actual, owner, group);
}

int fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag) {
    REWRITE_ABS(actual, path);
    return real_fchownat(fd, actual, owner, group, flag);
}

/*** symlink / symlinkat **************************/

int symlink(const char *target, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_1(alink, linkpath);
    return real_symlink(atarget, alink);
}

int symlinkat(const char *target, int fd, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_ABS(alink, linkpath);
    return real_symlinkat(atarget, fd, alink);
}

/*** link / linkat ********************************/

int link(const char *p1, const char *p2) {
    REWRITE_1(a1, p1);
    REWRITE_1(a2, p2);
    return real_link(a1, a2);
}

int linkat(int fd1, const char *p1, int fd2, const char *p2, int flag) {
    REWRITE_ABS(a1, p1);
    REWRITE_ABS(a2, p2);
    return real_linkat(fd1, a1, fd2, a2, flag);
}

/*** truncate *************************************/

int truncate(const char *path, off_t length) {
    REWRITE_1(actual, path);
    return real_truncate(actual, length);
}

/*** realpath *************************************/

char *realpath(const char *path, char *resolved) {
    REWRITE_1(actual, path);
    return real_realpath(actual, resolved);
}

/* glibc fortified variant — gcc -O2 may redirect realpath() calls here */
char *__realpath_chk(const char *path, char *resolved, size_t resolvedlen) {
    REWRITE_1(actual, path);
    return real___realpath_chk(actual, resolved, resolvedlen);
}
