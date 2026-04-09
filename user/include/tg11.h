/**
 * Copyright (C) 2026 TG11
 *
 * Minimal C library for TG11-OS user programs.
 */
#ifndef TG11_USER_TG11_H
#define TG11_USER_TG11_H

#include "syscall.h"

typedef unsigned long size_t;
typedef long          ssize_t;

/* ── Process control ────────────────────────────────────────────── */

static inline void exit(int code)
{
    syscall1(SYS_EXIT, (long)code);
    __builtin_unreachable();
}

static inline long getpid(void)
{
    return syscall0(SYS_GETPID);
}

static inline long kill(long pid, long sig)
{
    return syscall2(SYS_KILL, pid, sig);
}

/* ── File I/O ───────────────────────────────────────────────────── */

static inline ssize_t write(int fd, const void *buf, size_t count)
{
    return (ssize_t)syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static inline ssize_t read(int fd, void *buf, size_t count)
{
    return (ssize_t)syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

static inline long open(const char *path)
{
    return syscall1(SYS_OPEN, (long)path);
}

static inline long close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

static inline long lseek(int fd, long offset, int whence)
{
    return syscall3(SYS_LSEEK, (long)fd, offset, (long)whence);
}

/* ── Memory ─────────────────────────────────────────────────────── */

static inline long brk(unsigned long addr)
{
    return syscall1(SYS_BRK, (long)addr);
}

static inline void *mmap(void *addr, size_t length, int prot, int flags)
{
    return (void *)syscall4(SYS_MMAP, (long)addr, (long)length, (long)prot, (long)flags);
}

static inline long munmap(void *addr, size_t length)
{
    return syscall2(SYS_MUNMAP, (long)addr, (long)length);
}

/* ── IPC ────────────────────────────────────────────────────────── */

static inline long pipe(long fds[2])
{
    return syscall1(SYS_PIPE, (long)fds);
}

static inline long poll(int fd, long timeout_ms)
{
    return syscall2(SYS_POLL, (long)fd, timeout_ms);
}

/* ── String helpers ─────────────────────────────────────────────── */

static inline size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ── Convenience ────────────────────────────────────────────────── */

static inline ssize_t puts(const char *s)
{
    size_t len = strlen(s);
    ssize_t r = write(1, s, len);
    write(1, "\n", 1);
    return r;
}

static inline ssize_t print(const char *s)
{
    return write(1, s, strlen(s));
}

/* Print an unsigned long in decimal. */
static inline void print_ulong(unsigned long v)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) { write(1, "0", 1); return; }
    while (v > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    write(1, buf + i, (size_t)(20 - i));
}

#endif /* TG11_USER_TG11_H */
