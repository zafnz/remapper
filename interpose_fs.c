/*
 * interpose_fs.c - Filesystem interpose functions (open, stat, mkdir, etc.)
 * Copyright (c) 2026 Nick Clifford <nick@nickclifford.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "interpose.h"

/*** Interposed filesystem functions **************/

static int my_open(const char *path, int flags, ...) {
    REWRITE_1(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return open(actual, flags, mode);
    }
    return open(actual, flags);
}
DYLD_INTERPOSE(my_open, open)

static int my_openat(int fd, const char *path, int flags, ...) {
    REWRITE_ABS(actual, path);
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return openat(fd, actual, flags, mode);
    }
    return openat(fd, actual, flags);
}
DYLD_INTERPOSE(my_openat, openat)

//creat
static int my_creat(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return open(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}
DYLD_INTERPOSE(my_creat, creat)

//stat / lstat / fstatat
static int my_stat(const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return stat(actual, sb);
}
DYLD_INTERPOSE(my_stat, stat)

static int my_lstat(const char *path, struct stat *sb) {
    REWRITE_1(actual, path);
    return lstat(actual, sb);
}
DYLD_INTERPOSE(my_lstat, lstat)

static int my_fstatat(int fd, const char *path, struct stat *sb, int flag) {
    REWRITE_ABS(actual, path);
    return fstatat(fd, actual, sb, flag);
}
DYLD_INTERPOSE(my_fstatat, fstatat)

//access / faccessat
static int my_access(const char *path, int mode) {
    REWRITE_1(actual, path);
    return access(actual, mode);
}
DYLD_INTERPOSE(my_access, access)

static int my_faccessat(int fd, const char *path, int mode, int flag) {
    REWRITE_ABS(actual, path);
    return faccessat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_faccessat, faccessat)

//mkdir / mkdirat
static int my_mkdir(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return mkdir(actual, mode);
}
DYLD_INTERPOSE(my_mkdir, mkdir)

static int my_mkdirat(int fd, const char *path, mode_t mode) {
    REWRITE_ABS(actual, path);
    return mkdirat(fd, actual, mode);
}
DYLD_INTERPOSE(my_mkdirat, mkdirat)

//unlink / unlinkat
static int my_unlink(const char *path) {
    REWRITE_1(actual, path);
    return unlink(actual);
}
DYLD_INTERPOSE(my_unlink, unlink)

static int my_unlinkat(int fd, const char *path, int flag) {
    REWRITE_ABS(actual, path);
    return unlinkat(fd, actual, flag);
}
DYLD_INTERPOSE(my_unlinkat, unlinkat)

//rename / renameat
static int my_rename(const char *oldp, const char *newp) {
    REWRITE_1(aold, oldp);
    REWRITE_1(anew, newp);
    return rename(aold, anew);
}
DYLD_INTERPOSE(my_rename, rename)

static int my_renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    REWRITE_ABS(aold, oldp);
    REWRITE_ABS(anew, newp);
    return renameat(ofd, aold, nfd, anew);
}
DYLD_INTERPOSE(my_renameat, renameat)

//rmdir
static int my_rmdir(const char *path) {
    REWRITE_1(actual, path);
    return rmdir(actual);
}
DYLD_INTERPOSE(my_rmdir, rmdir)

//opendir
static DIR *my_opendir(const char *path) {
    REWRITE_1(actual, path);
    return opendir(actual);
}
DYLD_INTERPOSE(my_opendir, opendir)

//chdir
static int my_chdir(const char *path) {
    REWRITE_1(actual, path);
    return chdir(actual);
}
DYLD_INTERPOSE(my_chdir, chdir)

//readlink / readlinkat
static ssize_t my_readlink(const char *path, char *buf, size_t bufsiz) {
    REWRITE_1(actual, path);
    return readlink(actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlink, readlink)

static ssize_t my_readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    REWRITE_ABS(actual, path);
    return readlinkat(fd, actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlinkat, readlinkat)

//chmod / fchmodat
static int my_chmod(const char *path, mode_t mode) {
    REWRITE_1(actual, path);
    return chmod(actual, mode);
}
DYLD_INTERPOSE(my_chmod, chmod)

static int my_fchmodat(int fd, const char *path, mode_t mode, int flag) {
    REWRITE_ABS(actual, path);
    return fchmodat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_fchmodat, fchmodat)

//chown / lchown / fchownat
static int my_chown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return chown(actual, owner, group);
}
DYLD_INTERPOSE(my_chown, chown)

static int my_lchown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1(actual, path);
    return lchown(actual, owner, group);
}
DYLD_INTERPOSE(my_lchown, lchown)

static int my_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag) {
    REWRITE_ABS(actual, path);
    return fchownat(fd, actual, owner, group, flag);
}
DYLD_INTERPOSE(my_fchownat, fchownat)

//symlink / symlinkat
static int my_symlink(const char *target, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_1(alink, linkpath);
    return symlink(atarget, alink);
}
DYLD_INTERPOSE(my_symlink, symlink)

static int my_symlinkat(const char *target, int fd, const char *linkpath) {
    REWRITE_1(atarget, target);
    REWRITE_ABS(alink, linkpath);
    return symlinkat(atarget, fd, alink);
}
DYLD_INTERPOSE(my_symlinkat, symlinkat)

//link / linkat
static int my_link(const char *p1, const char *p2) {
    REWRITE_1(a1, p1);
    REWRITE_1(a2, p2);
    return link(a1, a2);
}
DYLD_INTERPOSE(my_link, link)

static int my_linkat(int fd1, const char *p1, int fd2, const char *p2, int flag) {
    REWRITE_ABS(a1, p1);
    REWRITE_ABS(a2, p2);
    return linkat(fd1, a1, fd2, a2, flag);
}
DYLD_INTERPOSE(my_linkat, linkat)

//truncate
static int my_truncate(const char *path, off_t length) {
    REWRITE_1(actual, path);
    return truncate(actual, length);
}
DYLD_INTERPOSE(my_truncate, truncate)

//realpath
static char *my_realpath(const char *path, char *resolved) {
    REWRITE_1(actual, path);
    return realpath(actual, resolved);
}
DYLD_INTERPOSE(my_realpath, realpath)
