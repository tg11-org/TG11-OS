/**
 * Copyright (C) 2026 TG11
 *
 * System call numbers for user programs.
 */
#ifndef TG11_SYSCALL_H
#define TG11_SYSCALL_H

#define SYS_WRITE  1
#define SYS_EXIT  60

void syscall_init(void);

#endif /* TG11_SYSCALL_H */
