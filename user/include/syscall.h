/**
 * Copyright (C) 2026 TG11
 *
 * User-space syscall wrappers for TG11-OS.
 *
 * Syscall ABI:
 *   rax = syscall number
 *   rdi = arg1, rsi = arg2, rdx = arg3, r10 = arg4
 *   Return value in rax.
 */
#ifndef TG11_USER_SYSCALL_H
#define TG11_USER_SYSCALL_H

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_POLL   7
#define SYS_LSEEK  8
#define SYS_MMAP   9
#define SYS_MUNMAP 11
#define SYS_BRK   12
#define SYS_PIPE  22
#define SYS_GETPID 39
#define SYS_EXIT  60
#define SYS_KILL  62

static inline long syscall0(long num)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall1(long num, long a1)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall2(long num, long a1, long a2)
{
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall3(long num, long a1, long a2, long a3)
{
    long ret;
    register long r_a3 __asm__("rdx") = a3;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "r"(r_a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long syscall4(long num, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r_a3 __asm__("rdx") = a3;
    register long r_a4 __asm__("r10") = a4;
    __asm__ volatile("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "r"(r_a3), "r"(r_a4)
        : "rcx", "r11", "memory");
    return ret;
}

#endif /* TG11_USER_SYSCALL_H */
