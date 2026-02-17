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
 * Each real_* pointer is resolved lazily on first call.
 */

/*** open / openat / creat ************************/

int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
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
    static int (*real_open64)(const char *, int, ...) = NULL;
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
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
    static int (*real_openat)(int, const char *, int, ...) = NULL;
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
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
    static int (*real_openat64)(int, const char *, int, ...) = NULL;
    if (!real_openat64) real_openat64 = dlsym(RTLD_NEXT, "openat64");
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
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    REWRITE_1(actual, path);
    return real_open(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int creat64(const char *path, mode_t mode) {
    static int (*real_open64)(const char *, int, ...) = NULL;
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    REWRITE_1(actual, path);
    return real_open64(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

/*** stat / lstat / fstatat ***********************/

int __xstat(int ver, const char *path, struct stat *sb) {
    static int (*real___xstat)(int, const char *, struct stat *) = NULL;
    if (!real___xstat) real___xstat = dlsym(RTLD_NEXT, "__xstat");
    REWRITE_1(actual, path);
    return real___xstat(ver, actual, sb);
}

int __lxstat(int ver, const char *path, struct stat *sb) {
    static int (*real___lxstat)(int, const char *, struct stat *) = NULL;
    if (!real___lxstat) real___lxstat = dlsym(RTLD_NEXT, "__lxstat");
    REWRITE_1(actual, path);
    return real___lxstat(ver, actual, sb);
}

int __fxstatat(int ver, int fd, const char *path, struct stat *sb, int flag) {
    static int (*real___fxstatat)(int, int, const char *, struct stat *, int) = NULL;
    if (!real___fxstatat) real___fxstatat = dlsym(RTLD_NEXT, "__fxstatat");
    REWRITE_ABS(actual, path);
    return real___fxstatat(ver, fd, actual, sb, flag);
}

/* glibc >= 2.33 (e.g. Ubuntu 22.04+) also exposes direct stat/lstat/fstatat */
int stat(const char *path, struct stat *sb) {
    static int (*real_stat)(const char *, struct stat *) = NULL;
    if (!real_stat) real_stat = dlsym(RTLD_NEXT, "stat");
    if (!real_stat) {
        /* Fallback: older glibc only has __xstat */
        return __xstat(1, path, sb);
    }
    REWRITE_1(actual, path);
    return real_stat(actual, sb);
}

int lstat(const char *path, struct stat *sb) {
    static int (*real_lstat)(const char *, struct stat *) = NULL;
    if (!real_lstat) real_lstat = dlsym(RTLD_NEXT, "lstat");
    if (!real_lstat) {
        return __lxstat(1, path, sb);
    }
    REWRITE_1(actual, path);
    return real_lstat(actual, sb);
}

int fstatat(int fd, const char *path, struct stat *sb, int flag) {
    static int (*real_fstatat)(int, const char *, struct stat *, int) = NULL;
    if (!real_fstatat) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    if (!real_fstatat) {
        return __fxstatat(1, fd, path, sb, flag);
    }
    REWRITE_ABS(actual, path);
    return real_fstatat(fd, actual, sb, flag);
}

/*** access / faccessat ***************************/

int access(const char *path, int mode) {
    static int (*real_access)(const char *, int) = NULL;
    if (!real_access) real_access = dlsym(RTLD_NEXT, "access");
    REWRITE_1(actual, path);
    return real_access(actual, mode);
}

int faccessat(int fd, const char *path, int mode, int flag) {
    static int (*real_faccessat)(int, const char *, int, int) = NULL;
    if (!real_faccessat) real_faccessat = dlsym(RTLD_NEXT, "faccessat");
    REWRITE_ABS(actual, path);
    return real_faccessat(fd, actual, mode, flag);
}

/*** mkdir / mkdirat ******************************/

int mkdir(const char *path, mode_t mode) {
    static int (*real_mkdir)(const char *, mode_t) = NULL;
    if (!real_mkdir) real_mkdir = dlsym(RTLD_NEXT, "mkdir");
    REWRITE_1(actual, path);
    return real_mkdir(actual, mode);
}

int mkdirat(int fd, const char *path, mode_t mode) {
    static int (*real_mkdirat)(int, const char *, mode_t) = NULL;
    if (!real_mkdirat) real_mkdirat = dlsym(RTLD_NEXT, "mkdirat");
    REWRITE_ABS(actual, path);
    return real_mkdirat(fd, actual, mode);
}

/*** unlink / unlinkat ****************************/

int unlink(const char *path) {
    static int (*real_unlink)(const char *) = NULL;
    if (!real_unlink) real_unlink = dlsym(RTLD_NEXT, "unlink");
    REWRITE_1(actual, path);
    return real_unlink(actual);
}

int unlinkat(int fd, const char *path, int flag) {
    static int (*real_unlinkat)(int, const char *, int) = NULL;
    if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    REWRITE_ABS(actual, path);
    return real_unlinkat(fd, actual, flag);
}

/*** rename / renameat ****************************/

int rename(const char *oldp, const char *newp) {
    static int (*real_rename)(const char *, const char *) = NULL;
    if (!real_rename) real_rename = dlsym(RTLD_NEXT, "rename");
    REWRITE_1(aold, oldp);
    REWRITE_1(anew, newp);
    return real_rename(aold, anew);
}

int renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    static int (*real_renameat)(int, const char *, int, const char *) = NULL;
    if (!real_renameat) real_renameat = dlsym(RTLD_NEXT, "renameat");
    REWRITE_ABS(aold, oldp);
    REWRITE_ABS(anew, newp);
    return real_renameat(ofd, aold, nfd, anew);
}

/*** rmdir ****************************************/

int rmdir(const char *path) {
    static int (*real_rmdir)(const char *) = NULL;
    if (!real_rmdir) real_rmdir = dlsym(RTLD_NEXT, "rmdir");
    REWRITE_1(actual, path);
    return real_rmdir(actual);
}

/*** opendir **************************************/

DIR *opendir(const char *path) {
    static DIR *(*real_opendir)(const char *) = NULL;
    if (!real_opendir) real_opendir = dlsym(RTLD_NEXT, "opendir");
    REWRITE_1(actual, path);
    return real_opendir(actual);
}

/*** chdir ****************************************/

int chdir(const char *path) {
    static int (*real_chdir)(const char *) = NULL;
    if (!real_chdir) real_chdir = dlsym(RTLD_NEXT, "chdir");
    REWRITE_1(actual, path);
    return real_chdir(actual);
}

/*** readlink / readlinkat ************************/

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*real_readlink)(const char *, char *, size_t) = NULL;
    if (!real_readlink) real_readlink = dlsym(RTLD_NEXT, "readlink");
    REWRITE_1(actual, path);
    return real_readlink(actual, buf, bufsiz);
}

ssize_t readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    static ssize_t (*real_readlinkat)(int, const char *, char *, size_t) = NULL;
    if (!real_readlinkat) real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
    REWRITE_ABS(actual, path);
    return real_readlinkat(fd, actual, buf, bufsiz);
}

/*** chmod / fchmodat *****************************/

int chmod(const char *path, mode_t mode) {
    static int (*real_chmod)(const char *, mode_t) = NULL;
    if (!real_chmod) real_chmod = dlsym(RTLD_NEXT, "chmod");
    REWRITE_1(actual, path);
    return real_chmod(actual, mode);
}

int fchmodat(int fd, const char *path, mode_t mode, int flag) {
    static int (*real_fchmodat)(int, const char *, mode_t, int) = NULL;
    if (!real_fchmodat) real_fchmodat = dlsym(RTLD_NEXT, "fchmodat");
    REWRITE_ABS(actual, path);
    return real_fchmodat(fd, actual, mode, flag);
}

/*** chown / lchown / fchownat ********************/

int chown(const char *path, uid_t owner, gid_t group) {
    static int (*real_chown)(const char *, uid_t, gid_t) = NULL;
    if (!real_chown) real_chown = dlsym(RTLD_NEXT, "chown");
    REWRITE_1(actual, path);
    return real_chown(actual, owner, group);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    static int (*real_lchown)(const char *, uid_t, gid_t) = NULL;
    if (!real_lchown) real_lchown = dlsym(RTLD_NEXT, "lchown");
    REWRITE_1(actual, path);
    return real_lchown(actual, owner, group);
}

int fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag) {
    static int (*real_fchownat)(int, const char *, uid_t, gid_t, int) = NULL;
    if (!real_fchownat) real_fchownat = dlsym(RTLD_NEXT, "fchownat");
    REWRITE_ABS(actual, path);
    return real_fchownat(fd, actual, owner, group, flag);
}

/*** symlink / symlinkat **************************/

int symlink(const char *target, const char *linkpath) {
    static int (*real_symlink)(const char *, const char *) = NULL;
    if (!real_symlink) real_symlink = dlsym(RTLD_NEXT, "symlink");
    REWRITE_1(atarget, target);
    REWRITE_1(alink, linkpath);
    return real_symlink(atarget, alink);
}

int symlinkat(const char *target, int fd, const char *linkpath) {
    static int (*real_symlinkat)(const char *, int, const char *) = NULL;
    if (!real_symlinkat) real_symlinkat = dlsym(RTLD_NEXT, "symlinkat");
    REWRITE_1(atarget, target);
    REWRITE_ABS(alink, linkpath);
    return real_symlinkat(atarget, fd, alink);
}

/*** link / linkat ********************************/

int link(const char *p1, const char *p2) {
    static int (*real_link)(const char *, const char *) = NULL;
    if (!real_link) real_link = dlsym(RTLD_NEXT, "link");
    REWRITE_1(a1, p1);
    REWRITE_1(a2, p2);
    return real_link(a1, a2);
}

int linkat(int fd1, const char *p1, int fd2, const char *p2, int flag) {
    static int (*real_linkat)(int, const char *, int, const char *, int) = NULL;
    if (!real_linkat) real_linkat = dlsym(RTLD_NEXT, "linkat");
    REWRITE_ABS(a1, p1);
    REWRITE_ABS(a2, p2);
    return real_linkat(fd1, a1, fd2, a2, flag);
}

/*** truncate *************************************/

int truncate(const char *path, off_t length) {
    static int (*real_truncate)(const char *, off_t) = NULL;
    if (!real_truncate) real_truncate = dlsym(RTLD_NEXT, "truncate");
    REWRITE_1(actual, path);
    return real_truncate(actual, length);
}

/*** realpath *************************************/

char *realpath(const char *path, char *resolved) {
    static char *(*real_realpath)(const char *, char *) = NULL;
    if (!real_realpath) real_realpath = dlsym(RTLD_NEXT, "realpath");
    REWRITE_1(actual, path);
    return real_realpath(actual, resolved);
}

/* glibc fortified variant — gcc -O2 may redirect realpath() calls here */
char *__realpath_chk(const char *path, char *resolved, size_t resolvedlen) {
    static char *(*real___realpath_chk)(const char *, char *, size_t) = NULL;
    if (!real___realpath_chk) real___realpath_chk = dlsym(RTLD_NEXT, "__realpath_chk");
    REWRITE_1(actual, path);
    return real___realpath_chk(actual, resolved, resolvedlen);
}
