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
    REWRITE_1_F(actual, path, "open");
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
    REWRITE_ABS_F(actual, path, "openat");
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
    REWRITE_1_F(actual, path, "creat");
    return open(actual, O_CREAT | O_WRONLY | O_TRUNC, mode);
}
DYLD_INTERPOSE(my_creat, creat)

//stat / lstat / fstatat
static int my_stat(const char *path, struct stat *sb) {
    REWRITE_1_F(actual, path, "stat");
    return stat(actual, sb);
}
DYLD_INTERPOSE(my_stat, stat)

static int my_lstat(const char *path, struct stat *sb) {
    REWRITE_1_F(actual, path, "lstat");
    return lstat(actual, sb);
}
DYLD_INTERPOSE(my_lstat, lstat)

static int my_fstatat(int fd, const char *path, struct stat *sb, int flag) {
    REWRITE_ABS_F(actual, path, "fstatat");
    return fstatat(fd, actual, sb, flag);
}
DYLD_INTERPOSE(my_fstatat, fstatat)

//access / faccessat
static int my_access(const char *path, int mode) {
    REWRITE_1_F(actual, path, "access");
    return access(actual, mode);
}
DYLD_INTERPOSE(my_access, access)

static int my_faccessat(int fd, const char *path, int mode, int flag) {
    REWRITE_ABS_F(actual, path, "faccessat");
    return faccessat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_faccessat, faccessat)

//mkdir / mkdirat
static int my_mkdir(const char *path, mode_t mode) {
    REWRITE_1_F(actual, path, "mkdir");
    return mkdir(actual, mode);
}
DYLD_INTERPOSE(my_mkdir, mkdir)

static int my_mkdirat(int fd, const char *path, mode_t mode) {
    REWRITE_ABS_F(actual, path, "mkdirat");
    return mkdirat(fd, actual, mode);
}
DYLD_INTERPOSE(my_mkdirat, mkdirat)

//unlink / unlinkat
static int my_unlink(const char *path) {
    REWRITE_1_F(actual, path, "unlink");
    return unlink(actual);
}
DYLD_INTERPOSE(my_unlink, unlink)

static int my_unlinkat(int fd, const char *path, int flag) {
    REWRITE_ABS_F(actual, path, "unlinkat");
    return unlinkat(fd, actual, flag);
}
DYLD_INTERPOSE(my_unlinkat, unlinkat)

//rename / renameat
static int my_rename(const char *oldp, const char *newp) {
    REWRITE_1_F(aold, oldp, "rename");
    REWRITE_1_F(anew, newp, "rename");
    return rename(aold, anew);
}
DYLD_INTERPOSE(my_rename, rename)

static int my_renameat(int ofd, const char *oldp, int nfd, const char *newp) {
    REWRITE_ABS_F(aold, oldp, "renameat");
    REWRITE_ABS_F(anew, newp, "renameat");
    return renameat(ofd, aold, nfd, anew);
}
DYLD_INTERPOSE(my_renameat, renameat)

//rmdir
static int my_rmdir(const char *path) {
    REWRITE_1_F(actual, path, "rmdir");
    return rmdir(actual);
}
DYLD_INTERPOSE(my_rmdir, rmdir)

//opendir
static DIR *my_opendir(const char *path) {
    REWRITE_1_F(actual, path, "opendir");
    return opendir(actual);
}
DYLD_INTERPOSE(my_opendir, opendir)

//chdir
static int my_chdir(const char *path) {
    REWRITE_1_F(actual, path, "chdir");
    return chdir(actual);
}
DYLD_INTERPOSE(my_chdir, chdir)

//readlink / readlinkat
static ssize_t my_readlink(const char *path, char *buf, size_t bufsiz) {
    REWRITE_1_F(actual, path, "readlink");
    return readlink(actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlink, readlink)

static ssize_t my_readlinkat(int fd, const char *path, char *buf, size_t bufsiz) {
    REWRITE_ABS_F(actual, path, "readlinkat");
    return readlinkat(fd, actual, buf, bufsiz);
}
DYLD_INTERPOSE(my_readlinkat, readlinkat)

//chmod / fchmodat
static int my_chmod(const char *path, mode_t mode) {
    REWRITE_1_F(actual, path, "chmod");
    return chmod(actual, mode);
}
DYLD_INTERPOSE(my_chmod, chmod)

static int my_fchmodat(int fd, const char *path, mode_t mode, int flag) {
    REWRITE_ABS_F(actual, path, "fchmodat");
    return fchmodat(fd, actual, mode, flag);
}
DYLD_INTERPOSE(my_fchmodat, fchmodat)

//chown / lchown / fchownat
static int my_chown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1_F(actual, path, "chown");
    return chown(actual, owner, group);
}
DYLD_INTERPOSE(my_chown, chown)

static int my_lchown(const char *path, uid_t owner, gid_t group) {
    REWRITE_1_F(actual, path, "lchown");
    return lchown(actual, owner, group);
}
DYLD_INTERPOSE(my_lchown, lchown)

static int my_fchownat(int fd, const char *path, uid_t owner, gid_t group, int flag) {
    REWRITE_ABS_F(actual, path, "fchownat");
    return fchownat(fd, actual, owner, group, flag);
}
DYLD_INTERPOSE(my_fchownat, fchownat)

//symlink / symlinkat
static int my_symlink(const char *target, const char *linkpath) {
    REWRITE_1_F(atarget, target, "symlink");
    REWRITE_1_F(alink, linkpath, "symlink");
    return symlink(atarget, alink);
}
DYLD_INTERPOSE(my_symlink, symlink)

static int my_symlinkat(const char *target, int fd, const char *linkpath) {
    REWRITE_1_F(atarget, target, "symlinkat");
    REWRITE_ABS_F(alink, linkpath, "symlinkat");
    return symlinkat(atarget, fd, alink);
}
DYLD_INTERPOSE(my_symlinkat, symlinkat)

//link / linkat
static int my_link(const char *p1, const char *p2) {
    REWRITE_1_F(a1, p1, "link");
    REWRITE_1_F(a2, p2, "link");
    return link(a1, a2);
}
DYLD_INTERPOSE(my_link, link)

static int my_linkat(int fd1, const char *p1, int fd2, const char *p2, int flag) {
    REWRITE_ABS_F(a1, p1, "linkat");
    REWRITE_ABS_F(a2, p2, "linkat");
    return linkat(fd1, a1, fd2, a2, flag);
}
DYLD_INTERPOSE(my_linkat, linkat)

//truncate
static int my_truncate(const char *path, off_t length) {
    REWRITE_1_F(actual, path, "truncate");
    return truncate(actual, length);
}
DYLD_INTERPOSE(my_truncate, truncate)

//realpath
static char *my_realpath(const char *path, char *resolved) {
    REWRITE_1_F(actual, path, "realpath");
    return realpath(actual, resolved);
}
DYLD_INTERPOSE(my_realpath, realpath)

/*** macOS variant symbols *************************/
//
// Some binaries (notably statically-linked Node.js / libuv) import
// variant symbols like openat$NOCANCEL or fopen$DARWIN_EXTSN instead
// of the standard names. DYLD interposition is per-symbol, so we must
// interpose each variant explicitly.

// open$NOCANCEL
extern int open$NOCANCEL(const char *, int, ...) __asm("_open$NOCANCEL");

static int my_open_nocancel(const char *path, int flags, ...) {
    REWRITE_1_F(actual, path, "open$NOCANCEL");
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return open$NOCANCEL(actual, flags, mode);
    }
    return open$NOCANCEL(actual, flags);
}
DYLD_INTERPOSE(my_open_nocancel, open$NOCANCEL)

// openat$NOCANCEL
extern int openat$NOCANCEL(int, const char *, int, ...) __asm("_openat$NOCANCEL");

static int my_openat_nocancel(int fd, const char *path, int flags, ...) {
    REWRITE_ABS_F(actual, path, "openat$NOCANCEL");
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode_t mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        return openat$NOCANCEL(fd, actual, flags, mode);
    }
    return openat$NOCANCEL(fd, actual, flags);
}
DYLD_INTERPOSE(my_openat_nocancel, openat$NOCANCEL)

// fopen
static FILE *my_fopen(const char *path, const char *mode) {
    REWRITE_1_F(actual, path, "fopen");
    return fopen(actual, mode);
}
DYLD_INTERPOSE(my_fopen, fopen)

// fopen$DARWIN_EXTSN — extended fopen on macOS
extern FILE *fopen$DARWIN_EXTSN(const char *, const char *) __asm("_fopen$DARWIN_EXTSN");

static FILE *my_fopen_darwin(const char *path, const char *mode) {
    REWRITE_1_F(actual, path, "fopen$DARWIN_EXTSN");
    return fopen$DARWIN_EXTSN(actual, mode);
}
DYLD_INTERPOSE(my_fopen_darwin, fopen$DARWIN_EXTSN)

// realpath$DARWIN_EXTSN — extended realpath on macOS
extern char *realpath$DARWIN_EXTSN(const char *, char *) __asm("_realpath$DARWIN_EXTSN");

static char *my_realpath_darwin(const char *path, char *resolved) {
    REWRITE_1_F(actual, path, "realpath$DARWIN_EXTSN");
    return realpath$DARWIN_EXTSN(actual, resolved);
}
DYLD_INTERPOSE(my_realpath_darwin, realpath$DARWIN_EXTSN)
