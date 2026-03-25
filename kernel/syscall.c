/**
 * Copyright (C) 2026 TG11
 *
 * SYSCALL initialisation and dispatch.
 */
#include "syscall.h"
#include "arch.h"
#include "task.h"
#include "terminal.h"

/* ── MSR addresses ─────────────────────────────────────────────────── */
#define MSR_EFER   0xC0000080UL
#define MSR_STAR   0xC0000081UL
#define MSR_LSTAR  0xC0000082UL
#define MSR_SFMASK 0xC0000084UL

#define EFER_SCE   (1UL << 0) /* System Call Enable */

/* ── syscall_entry is defined in syscall.s ─────────────────────────── */
extern void syscall_entry(void);

/* ── Initialise SYSCALL/SYSRETQ MSRs ──────────────────────────────── */

void syscall_init(void)
{
    unsigned long efer;
    unsigned long star;

    /* Enable SCE (SYSCALL enable) in the EFER MSR. */
    efer = arch_rdmsr(MSR_EFER);
    arch_wrmsr(MSR_EFER, efer | EFER_SCE);

    /*
     * STAR[47:32] = 0x0008 → SYSCALL: CS = 0x08 (kcode), SS = 0x10 (kdata)
     * STAR[63:48] = 0x0018 → SYSRETQ: CS = 0x18+16|3 = 0x2B, SS = 0x18+8|3 = 0x23
     */
    star = (0x0018UL << 48) | (0x0008UL << 32);
    arch_wrmsr(MSR_STAR, star);

    /* LSTAR = kernel entry point for SYSCALL */
    arch_wrmsr(MSR_LSTAR, (unsigned long)syscall_entry);

    /* SFMASK: clear IF (bit 9) on entry to prevent interrupts during entry stub */
    arch_wrmsr(MSR_SFMASK, 0x200UL);
}

/* ── Dispatch table ────────────────────────────────────────────────── */

void syscall_dispatch(unsigned long num, unsigned long a1, unsigned long a2)
{
    (void)a2;
    switch (num) {
    case SYS_WRITE:
        /* a1 = pointer to NUL-terminated string in user space */
        terminal_write((const char *)a1);
        break;
    case SYS_EXIT:
        task_exit();
        break;
    default:
        break;
    }
}
