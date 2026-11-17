// A tiny LD_PRELOAD shim that teaches POSIX file calls that a backslash means a directory separator.
//
// The engine's paths are Windows-shaped: the config, the session store, the library and libobs itself
// join names with '\\'. On Linux that is a legal file-name byte, so without this every second path
// would be a file in the current directory. The shim rewrites the separator at the syscall boundary;
// strings, comparisons and the config keep their Windows shape, and a real backslash in a name (which
// this app never creates) is the only casualty.
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#define BX_PATH_MAX 8192

static _Thread_local char path_buffer[BX_PATH_MAX];

static const char* fix(const char* path) {
  if (!path) return path;
  const char* slash = strchr(path, '\\');
  if (!slash) return path;
  size_t length = strlen(path);
  if (length >= BX_PATH_MAX) return path;
  memcpy(path_buffer, path, length + 1);
  for (char* p = path_buffer; *p; ++p)
    if (*p == '\\') *p = '/';
  return path_buffer;
}

typedef int (*open_fn)(const char*, int, ...);
typedef int (*openat_fn)(int, const char*, int, ...);
typedef FILE* (*fopen_fn)(const char*, const char*);
typedef int (*stat_fn)(const char*, struct stat*);
typedef int (*fstatat_fn)(int, const char*, struct stat*, int);
typedef int (*access_fn)(const char*, int);
typedef int (*faccessat_fn)(int, const char*, int, int);
typedef int (*mkdir_fn)(const char*, mode_t);
typedef int (*mkdirat_fn)(int, const char*, mode_t);
typedef int (*unlink_fn)(const char*);
typedef int (*unlinkat_fn)(int, const char*, int);
typedef int (*rename_fn)(const char*, const char*);
typedef int (*renameat_fn)(int, const char*, int, const char*);
typedef int (*rmdir_fn)(const char*);
typedef DIR* (*opendir_fn)(const char*);
typedef int (*chdir_fn)(const char*);
typedef char* (*realpath_fn)(const char*, char*);
typedef ssize_t (*readlink_fn)(const char*, char*, size_t);
typedef ssize_t (*readlinkat_fn)(int, const char*, char*, size_t);
typedef int (*symlink_fn)(const char*, const char*);
typedef int (*link_fn)(const char*, const char*);
typedef int (*truncate_fn)(const char*, off_t);
typedef int (*chmod_fn)(const char*, mode_t);

#define REAL(name, type) \
  static type real_##name = NULL; \
  if (!real_##name) real_##name = (type)dlsym(RTLD_NEXT, #name)

int open(const char* path, int flags, ...) {
  REAL(open, open_fn);
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return real_open(fix(path), flags, mode);
}
int open64(const char* path, int flags, ...) {
  REAL(open64, open_fn);
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return real_open64(fix(path), flags, mode);
}
int openat(int dirfd, const char* path, int flags, ...) {
  REAL(openat, openat_fn);
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return real_openat(dirfd, fix(path), flags, mode);
}
FILE* fopen(const char* path, const char* mode) {
  REAL(fopen, fopen_fn);
  return real_fopen(fix(path), mode);
}
FILE* fopen64(const char* path, const char* mode) {
  REAL(fopen64, fopen_fn);
  return real_fopen64(fix(path), mode);
}
int stat(const char* path, struct stat* info) {
  REAL(stat, stat_fn);
  return real_stat(fix(path), info);
}
int stat64(const char* path, struct stat64* info) {
  REAL(stat64, stat_fn);
  return real_stat64(fix(path), (struct stat*)info);
}
int lstat(const char* path, struct stat* info) {
  REAL(lstat, stat_fn);
  return real_lstat(fix(path), info);
}
int lstat64(const char* path, struct stat64* info) {
  REAL(lstat64, stat_fn);
  return real_lstat64(fix(path), (struct stat*)info);
}
int fstatat(int dirfd, const char* path, struct stat* info, int flags) {
  REAL(fstatat, fstatat_fn);
  return real_fstatat(dirfd, fix(path), info, flags);
}
int access(const char* path, int mode) {
  REAL(access, access_fn);
  return real_access(fix(path), mode);
}
int faccessat(int dirfd, const char* path, int mode, int flags) {
  REAL(faccessat, faccessat_fn);
  return real_faccessat(dirfd, fix(path), mode, flags);
}
int mkdir(const char* path, mode_t mode) {
  REAL(mkdir, mkdir_fn);
  return real_mkdir(fix(path), mode);
}
int mkdirat(int dirfd, const char* path, mode_t mode) {
  REAL(mkdirat, mkdirat_fn);
  return real_mkdirat(dirfd, fix(path), mode);
}
int unlink(const char* path) {
  REAL(unlink, unlink_fn);
  return real_unlink(fix(path));
}
int unlinkat(int dirfd, const char* path, int flags) {
  REAL(unlinkat, unlinkat_fn);
  return real_unlinkat(dirfd, fix(path), flags);
}
int rename(const char* from, const char* to) {
  REAL(rename, rename_fn);
  return real_rename(fix(from), fix(to));
}
int rmdir(const char* path) {
  REAL(rmdir, rmdir_fn);
  return real_rmdir(fix(path));
}
DIR* opendir(const char* path) {
  REAL(opendir, opendir_fn);
  return real_opendir(fix(path));
}
int chdir(const char* path) {
  REAL(chdir, chdir_fn);
  return real_chdir(fix(path));
}
char* realpath(const char* path, char* resolved) {
  REAL(realpath, realpath_fn);
  return real_realpath(fix(path), resolved);
}
ssize_t readlink(const char* path, char* buffer, size_t size) {
  REAL(readlink, readlink_fn);
  return real_readlink(fix(path), buffer, size);
}
int symlink(const char* target, const char* linkpath) {
  REAL(symlink, symlink_fn);
  return real_symlink(fix(target), fix(linkpath));
}
int link(const char* oldpath, const char* newpath) {
  REAL(link, link_fn);
  return real_link(fix(oldpath), fix(newpath));
}
int truncate(const char* path, off_t length) {
  REAL(truncate, truncate_fn);
  return real_truncate(fix(path), length);
}
int truncate64(const char* path, off64_t length) {
  REAL(truncate64, truncate_fn);
  return real_truncate64(fix(path), (off_t)length);
}
int chmod(const char* path, mode_t mode) {
  REAL(chmod, chmod_fn);
  return real_chmod(fix(path), mode);
}
