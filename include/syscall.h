/**
 * Copyright (C) 2026 TG11
 *
 * System call numbers for user programs.
 */
#ifndef TG11_SYSCALL_H
#define TG11_SYSCALL_H

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_LSEEK  8
#define SYS_MMAP   9
#define SYS_MUNMAP 11
#define SYS_BRK   12
#define SYS_PIPE  22
#define SYS_KILL  62
#define SYS_GETPID 39
#define SYS_POLL   7
#define SYS_EXIT  60

/* Signal numbers for SYS_KILL */
#define SIG_TERM   15
#define SIG_KILL   9
#define SIG_STOP   19
#define SIG_CONT   18

void syscall_init(void);
void syscall_mmap_cleanup(void);
void syscall_fd_cleanup(void);

#endif /* TG11_SYSCALL_H */
