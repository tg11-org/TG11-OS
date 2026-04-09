/**
 * Copyright (C) 2026 TG11
 *
 * SYSCALL initialisation and dispatch.
 */
#include "syscall.h"
#include "arch.h"
#include "task.h"
#include "terminal.h"
#include "memory.h"
#include "fs.h"

#define SYS_WRITE_MAX_BYTES 4096UL
#define SYS_READ_MAX_BYTES  4096UL
#define SYSCALL_FD_MAX      8
#define SYSCALL_PATH_MAX    128

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

/* ── File descriptor types ──────────────────────────────────────────── */
#define FD_TYPE_NONE        0
#define FD_TYPE_FILE        1
#define FD_TYPE_PIPE_READ   2
#define FD_TYPE_PIPE_WRITE  3

/* ── Pipe infrastructure ───────────────────────────────────────────── */
#define PIPE_BUF_SIZE       4096
#define PIPE_MAX            4

struct pipe_buf
{
    unsigned char data[PIPE_BUF_SIZE];
    unsigned long head;       /* next read position  */
    unsigned long count;      /* bytes available     */
    int           read_open;
    int           write_open;
};

static struct pipe_buf pipes[PIPE_MAX];

struct syscall_fd_entry
{
    int           type;       /* FD_TYPE_* */
    unsigned long pos;        /* file read position  */
    char          path[SYSCALL_PATH_MAX]; /* for FD_TYPE_FILE */
    int           pipe_idx;   /* for FD_TYPE_PIPE_*  */
};

static struct syscall_fd_entry syscall_fds[SYSCALL_FD_MAX];

static int addr_is_canonical(unsigned long addr)
{
    unsigned long sign = addr >> 47;
    return sign == 0x0UL || sign == 0x1FFFFUL;
}

static int user_addr_readable(unsigned long addr)
{
    if (!addr_is_canonical(addr)) return 0;
    return paging_get_phys(addr) != 0;
}

static unsigned long str_len(const char *s)
{
    unsigned long n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void str_copy(char *dst, unsigned long dst_sz, const char *src)
{
    unsigned long i = 0;
    if (dst == (void *)0 || dst_sz == 0 || src == (void *)0) return;
    while (src[i] != '\0' && i + 1 < dst_sz)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int user_copy_ascii(unsigned long src, char *dst, unsigned long n)
{
    unsigned long i;
    if (dst == (void *)0) return -1;
    for (i = 0; i < n; i++)
    {
        if (!user_addr_readable(src + i)) return -1;
        dst[i] = *((volatile const char *)(src + i));
    }
    return 0;
}

static int user_copy_cstr(unsigned long src, char *dst, unsigned long dst_sz)
{
    unsigned long i;
    if (dst == (void *)0 || dst_sz == 0 || src == 0) return -1;
    for (i = 0; i + 1 < dst_sz; i++)
    {
        char ch;
        if (!user_addr_readable(src + i)) return -1;
        ch = *((volatile const char *)(src + i));
        dst[i] = ch;
        if (ch == '\0') return 0;
    }
    dst[dst_sz - 1] = '\0';
    return -1;
}

static unsigned long syscall_write_user(unsigned long user_ptr, unsigned long len_or_zero)
{
    char chunk[129];
    unsigned long remaining = len_or_zero;
    unsigned long cursor = user_ptr;
    unsigned long total = 0;

    if (user_ptr == 0) return (unsigned long)-1;

    if (len_or_zero == 0)
    {
        /* NUL-terminated mode with a hard safety cap. */
        unsigned long scanned = 0;
        while (scanned < SYS_WRITE_MAX_BYTES)
        {
            unsigned long take = 0;
            while (take < 128 && scanned + take < SYS_WRITE_MAX_BYTES)
            {
                char ch;
                if (!user_addr_readable(cursor + take)) return (unsigned long)-1;
                ch = *((volatile const char *)(cursor + take));
                if (ch == '\0')
                {
                    chunk[take] = '\0';
                    if (take != 0) terminal_write(chunk);
                    return total;
                }
                chunk[take] = ch;
                take++;
            }
            chunk[take] = '\0';
            if (take != 0)
            {
                terminal_write(chunk);
                total += take;
            }
            cursor += take;
            scanned += take;
            if (take == 0) return total;
        }
        return total;
    }

    if (remaining > SYS_WRITE_MAX_BYTES) remaining = SYS_WRITE_MAX_BYTES;
    while (remaining != 0)
    {
        unsigned long take = remaining > 128UL ? 128UL : remaining;
        if (user_copy_ascii(cursor, chunk, take) != 0) return (unsigned long)-1;
        chunk[take] = '\0';
        terminal_write(chunk);
        total += take;
        cursor += take;
        remaining -= take;
    }
    return total;
}

static unsigned long user_copy_out(unsigned long dst, const char *src, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
    {
        if (!user_addr_readable(dst + i)) return (unsigned long)-1;
        *((volatile char *)(dst + i)) = src[i];
    }
    return n;
}

static int syscall_fd_slot_from_fd(unsigned long fd)
{
    if (fd < 3UL || fd >= (unsigned long)(3 + SYSCALL_FD_MAX)) return -1;
    return (int)(fd - 3UL);
}

static unsigned long syscall_open_user(unsigned long user_path)
{
    char path[SYSCALL_PATH_MAX];
    int is_dir = 0;
    unsigned long size = 0;
    int i;

    if (user_copy_cstr(user_path, path, sizeof(path)) != 0 || path[0] == '\0') return (unsigned long)-1;
    if (fs_stat(path, &is_dir, &size) != 0 || is_dir) return (unsigned long)-1;

    for (i = 0; i < SYSCALL_FD_MAX; i++)
    {
        if (syscall_fds[i].type == FD_TYPE_NONE)
        {
            syscall_fds[i].type = FD_TYPE_FILE;
            syscall_fds[i].pos = 0;
            syscall_fds[i].pipe_idx = -1;
            str_copy(syscall_fds[i].path, sizeof(syscall_fds[i].path), path);
            return (unsigned long)(3 + i);
        }
    }
    return (unsigned long)-1;
}

static unsigned long syscall_close_fd(unsigned long fd)
{
    int slot = syscall_fd_slot_from_fd(fd);
    if (slot < 0 || syscall_fds[slot].type == FD_TYPE_NONE) return (unsigned long)-1;

    /* Close pipe ends and free the pipe when both ends are closed. */
    if (syscall_fds[slot].type == FD_TYPE_PIPE_READ ||
        syscall_fds[slot].type == FD_TYPE_PIPE_WRITE)
    {
        int pi = syscall_fds[slot].pipe_idx;
        if (pi >= 0 && pi < PIPE_MAX)
        {
            if (syscall_fds[slot].type == FD_TYPE_PIPE_READ)
                pipes[pi].read_open = 0;
            else
                pipes[pi].write_open = 0;
        }
    }

    syscall_fds[slot].type = FD_TYPE_NONE;
    syscall_fds[slot].pos = 0;
    syscall_fds[slot].pipe_idx = -1;
    syscall_fds[slot].path[0] = '\0';
    return 0;
}

static unsigned long syscall_lseek_fd(unsigned long fd, long offset, unsigned long whence)
{
    int slot;
    int is_dir;
    unsigned long file_size = 0;
    unsigned long new_pos;

    slot = syscall_fd_slot_from_fd(fd);
    if (slot < 0 || syscall_fds[slot].type != FD_TYPE_FILE) return (unsigned long)-1;
    if (fs_stat(syscall_fds[slot].path, &is_dir, &file_size) != 0 || is_dir)
        return (unsigned long)-1;

    switch (whence) {
    case 0: /* SEEK_SET */
        if (offset < 0) return (unsigned long)-1;
        new_pos = (unsigned long)offset;
        break;
    case 1: /* SEEK_CUR */
        if (offset < 0 && (unsigned long)(-offset) > syscall_fds[slot].pos)
            return (unsigned long)-1;
        new_pos = (unsigned long)((long)syscall_fds[slot].pos + offset);
        break;
    case 2: /* SEEK_END */
        if (offset < 0 && (unsigned long)(-offset) > file_size)
            return (unsigned long)-1;
        new_pos = (unsigned long)((long)file_size + offset);
        break;
    default:
        return (unsigned long)-1;
    }

    syscall_fds[slot].pos = new_pos;
    return new_pos;
}

/* ── Anonymous mmap ─────────────────────────────────────────────── */
#define MMAP_PROT_WRITE   0x2UL
#define MMAP_PROT_EXEC    0x4UL
#define MMAP_MAP_ANON     0x20UL
#define MMAP_MAX_ENTRIES  16
#define MMAP_VA_BASE      0x0000004000000000UL  /* 256 GB: above heap/stack */

struct mmap_entry
{
    unsigned long virt_addr;
    unsigned long page_count;
    int           used;
};

static struct mmap_entry syscall_mmap_tbl[MMAP_MAX_ENTRIES];
static unsigned long     syscall_mmap_bump = MMAP_VA_BASE;

static unsigned long syscall_do_mmap(unsigned long addr_hint, unsigned long len,
                                     unsigned long prot, unsigned long flags)
{
    unsigned long page_count, base, i;
    unsigned long map_flags;
    int slot;

    (void)addr_hint;

    if (!(flags & MMAP_MAP_ANON) || len == 0) return (unsigned long)-1;

    page_count = (len + MEMORY_PAGE_SIZE - 1) / MEMORY_PAGE_SIZE;
    if (page_count == 0 || page_count > 256) return (unsigned long)-1;

    slot = -1;
    for (i = 0; i < MMAP_MAX_ENTRIES; i++)
    {
        if (!syscall_mmap_tbl[i].used) { slot = (int)i; break; }
    }
    if (slot < 0) return (unsigned long)-1;

    base      = syscall_mmap_bump;
    map_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
    if (prot & MMAP_PROT_WRITE) map_flags |= PAGE_FLAG_WRITABLE;
    if (!(prot & MMAP_PROT_EXEC)) map_flags |= PAGE_FLAG_NO_EXECUTE;

    for (i = 0; i < page_count; i++)
    {
        unsigned long phys = phys_alloc_page();
        if (phys == 0)
        {
            unsigned long j;
            for (j = 0; j < i; j++)
            {
                unsigned long v = base + j * MEMORY_PAGE_SIZE;
                unsigned long p = paging_get_phys(v);
                paging_unmap_page(v);
                if (p) phys_free_page(p);
            }
            return (unsigned long)-1;
        }
        memory_zero_phys_page(phys);
        if (paging_map_page(base + i * MEMORY_PAGE_SIZE, phys, map_flags) != 0)
        {
            unsigned long j;
            phys_free_page(phys);
            for (j = 0; j < i; j++)
            {
                unsigned long v = base + j * MEMORY_PAGE_SIZE;
                unsigned long p = paging_get_phys(v);
                paging_unmap_page(v);
                if (p) phys_free_page(p);
            }
            return (unsigned long)-1;
        }
    }

    syscall_mmap_bump             += page_count * MEMORY_PAGE_SIZE;
    syscall_mmap_tbl[slot].used        = 1;
    syscall_mmap_tbl[slot].virt_addr   = base;
    syscall_mmap_tbl[slot].page_count  = page_count;
    return base;
}

static unsigned long syscall_do_munmap(unsigned long addr, unsigned long len)
{
    unsigned long i;
    (void)len;

    for (i = 0; i < MMAP_MAX_ENTRIES; i++)
    {
        unsigned long j;
        if (!syscall_mmap_tbl[i].used || syscall_mmap_tbl[i].virt_addr != addr) continue;
        for (j = 0; j < syscall_mmap_tbl[i].page_count; j++)
        {
            unsigned long v = addr + j * MEMORY_PAGE_SIZE;
            unsigned long p = paging_get_phys(v);
            paging_unmap_page(v);
            if (p) phys_free_page(p);
        }
        syscall_mmap_tbl[i].used       = 0;
        syscall_mmap_tbl[i].virt_addr  = 0;
        syscall_mmap_tbl[i].page_count = 0;
        return 0;
    }
    return (unsigned long)-1;
}

void syscall_mmap_cleanup(void)
{
    unsigned long i;
    for (i = 0; i < MMAP_MAX_ENTRIES; i++)
    {
        unsigned long j;
        if (!syscall_mmap_tbl[i].used) continue;
        for (j = 0; j < syscall_mmap_tbl[i].page_count; j++)
        {
            unsigned long v = syscall_mmap_tbl[i].virt_addr + j * MEMORY_PAGE_SIZE;
            unsigned long p = paging_get_phys(v);
            paging_unmap_page(v);
            if (p) phys_free_page(p);
        }
        syscall_mmap_tbl[i].used       = 0;
        syscall_mmap_tbl[i].virt_addr  = 0;
        syscall_mmap_tbl[i].page_count = 0;
    }
    syscall_mmap_bump = MMAP_VA_BASE;
}

void syscall_fd_cleanup(void)
{
    int i;
    for (i = 0; i < SYSCALL_FD_MAX; i++)
    {
        if (syscall_fds[i].type == FD_TYPE_NONE) continue;
        if (syscall_fds[i].type == FD_TYPE_PIPE_READ ||
            syscall_fds[i].type == FD_TYPE_PIPE_WRITE)
        {
            int pi = syscall_fds[i].pipe_idx;
            if (pi >= 0 && pi < PIPE_MAX)
            {
                if (syscall_fds[i].type == FD_TYPE_PIPE_READ)
                    pipes[pi].read_open = 0;
                else
                    pipes[pi].write_open = 0;
            }
        }
        syscall_fds[i].type = FD_TYPE_NONE;
        syscall_fds[i].pos = 0;
        syscall_fds[i].pipe_idx = -1;
        syscall_fds[i].path[0] = '\0';
    }
}

static unsigned long syscall_read_user(unsigned long user_ptr, unsigned long capacity)
{
    char line[257];
    unsigned long max_line;
    unsigned long n;

    if (user_ptr == 0 || capacity == 0) return (unsigned long)-1;
    if (capacity > SYS_READ_MAX_BYTES) capacity = SYS_READ_MAX_BYTES;
    max_line = capacity - 1;
    if (max_line > 256UL) max_line = 256UL;

    if (terminal_read_line(line, max_line + 1) != 0) return (unsigned long)-1;
    n = str_len(line);
    if (n > max_line) n = max_line;
    if (user_copy_out(user_ptr, line, n) == (unsigned long)-1) return (unsigned long)-1;
    if (user_copy_out(user_ptr + n, "\0", 1) == (unsigned long)-1) return (unsigned long)-1;
    return n;
}

static unsigned long syscall_read_fd(unsigned long fd, unsigned long user_ptr, unsigned long count)
{
    int slot;
    unsigned long avail;
    unsigned long take;

    if (fd == 0UL)
    {
        if (count == 0) return 0;
        return syscall_read_user(user_ptr, count);
    }

    slot = syscall_fd_slot_from_fd(fd);
    if (slot < 0 || syscall_fds[slot].type == FD_TYPE_NONE) return (unsigned long)-1;
    if (count > SYS_READ_MAX_BYTES) count = SYS_READ_MAX_BYTES;
    if (count == 0) return 0;

    /* ── Pipe read ──────────────────────────────────────────────── */
    if (syscall_fds[slot].type == FD_TYPE_PIPE_READ)
    {
        int pi = syscall_fds[slot].pipe_idx;
        if (pi < 0 || pi >= PIPE_MAX) return (unsigned long)-1;
        if (pipes[pi].count == 0)
            return pipes[pi].write_open ? 0 : 0; /* 0 = EOF / no data */
        take = count < pipes[pi].count ? count : pipes[pi].count;
        {
            unsigned long i;
            for (i = 0; i < take; i++)
            {
                unsigned long pos = (pipes[pi].head + i) % PIPE_BUF_SIZE;
                if (user_copy_out(user_ptr + i, (const char *)&pipes[pi].data[pos], 1)
                    == (unsigned long)-1)
                    return (unsigned long)-1;
            }
        }
        pipes[pi].head = (pipes[pi].head + take) % PIPE_BUF_SIZE;
        pipes[pi].count -= take;
        return take;
    }

    /* ── Regular file read ──────────────────────────────────────── */
    {
        unsigned char tmp[FS_MAX_FILE_SIZE];
        unsigned long size = 0;
        if (syscall_fds[slot].type != FD_TYPE_FILE) return (unsigned long)-1;
        if (fs_read_file(syscall_fds[slot].path, tmp, sizeof(tmp), &size) != 0) return (unsigned long)-1;
        if (syscall_fds[slot].pos >= size) return 0;
        avail = size - syscall_fds[slot].pos;
        take = count < avail ? count : avail;
        if (user_copy_out(user_ptr, (const char *)(tmp + syscall_fds[slot].pos), take) == (unsigned long)-1) return (unsigned long)-1;
        syscall_fds[slot].pos += take;
        return take;
    }
}

static unsigned long syscall_write_fd(unsigned long fd, unsigned long user_ptr, unsigned long len_or_zero)
{
    /* stdout / stderr → terminal */
    if (fd == 1UL || fd == 2UL)
        return syscall_write_user(user_ptr, len_or_zero);

    /* Pipe write */
    {
        int slot = syscall_fd_slot_from_fd(fd);
        if (slot < 0 || syscall_fds[slot].type != FD_TYPE_PIPE_WRITE) return (unsigned long)-1;
        {
            int pi = syscall_fds[slot].pipe_idx;
            unsigned long space, take, i;
            if (pi < 0 || pi >= PIPE_MAX) return (unsigned long)-1;
            if (!pipes[pi].read_open) return (unsigned long)-1; /* broken pipe */
            if (len_or_zero == 0) return 0;
            if (len_or_zero > SYS_WRITE_MAX_BYTES) len_or_zero = SYS_WRITE_MAX_BYTES;
            space = PIPE_BUF_SIZE - pipes[pi].count;
            take = len_or_zero < space ? len_or_zero : space;
            if (take == 0) return 0; /* pipe full */
            for (i = 0; i < take; i++)
            {
                unsigned long pos = (pipes[pi].head + pipes[pi].count + i) % PIPE_BUF_SIZE;
                char ch;
                if (!user_addr_readable(user_ptr + i)) return (unsigned long)-1;
                ch = *((volatile const char *)(user_ptr + i));
                pipes[pi].data[pos] = (unsigned char)ch;
            }
            pipes[pi].count += take;
            return take;
        }
    }
}

/* ── pipe(int fds[2]) ─────────────────────────────────────────────── */
static unsigned long syscall_do_pipe(unsigned long user_array)
{
    int pi, slot_r, slot_w, i;
    unsigned long read_fd, write_fd;

    if (user_array == 0) return (unsigned long)-1;

    /* Find a free pipe. */
    pi = -1;
    for (i = 0; i < PIPE_MAX; i++)
    {
        if (!pipes[i].read_open && !pipes[i].write_open)
        {
            pi = i;
            break;
        }
    }
    if (pi < 0) return (unsigned long)-1;

    /* Find two free FD slots. */
    slot_r = -1;
    slot_w = -1;
    for (i = 0; i < SYSCALL_FD_MAX; i++)
    {
        if (syscall_fds[i].type == FD_TYPE_NONE)
        {
            if (slot_r < 0) slot_r = i;
            else if (slot_w < 0) { slot_w = i; break; }
        }
    }
    if (slot_r < 0 || slot_w < 0) return (unsigned long)-1;

    /* Initialise pipe buffer. */
    pipes[pi].head = 0;
    pipes[pi].count = 0;
    pipes[pi].read_open = 1;
    pipes[pi].write_open = 1;

    /* Set up FD entries. */
    read_fd = (unsigned long)(3 + slot_r);
    write_fd = (unsigned long)(3 + slot_w);

    syscall_fds[slot_r].type = FD_TYPE_PIPE_READ;
    syscall_fds[slot_r].pipe_idx = pi;
    syscall_fds[slot_r].pos = 0;
    syscall_fds[slot_r].path[0] = '\0';

    syscall_fds[slot_w].type = FD_TYPE_PIPE_WRITE;
    syscall_fds[slot_w].pipe_idx = pi;
    syscall_fds[slot_w].pos = 0;
    syscall_fds[slot_w].path[0] = '\0';

    /* Write [read_fd, write_fd] to user array. */
    if (user_copy_out(user_array, (const char *)&read_fd, 8) == (unsigned long)-1 ||
        user_copy_out(user_array + 8, (const char *)&write_fd, 8) == (unsigned long)-1)
    {
        syscall_fds[slot_r].type = FD_TYPE_NONE;
        syscall_fds[slot_w].type = FD_TYPE_NONE;
        pipes[pi].read_open = 0;
        pipes[pi].write_open = 0;
        return (unsigned long)-1;
    }

    return 0;
}

/* ── kill(pid, sig) ───────────────────────────────────────────────── */
static unsigned long syscall_do_kill(unsigned long pid, unsigned long sig)
{
    unsigned int id = (unsigned int)pid;
    if (id == 0) return (unsigned long)-1;
    switch (sig)
    {
    case SIG_KILL:
    case SIG_TERM:
        return task_kill(id) == 0 ? 0 : (unsigned long)-1;
    case SIG_STOP:
        return task_stop(id) == 0 ? 0 : (unsigned long)-1;
    case SIG_CONT:
        return task_continue(id) == 0 ? 0 : (unsigned long)-1;
    default:
        return (unsigned long)-1;
    }
}

/* ── getpid() ─────────────────────────────────────────────────────── */
static unsigned long syscall_do_getpid(void)
{
    task_t *t = task_current();
    if (t == (void *)0) return 0;
    return (unsigned long)t->id;
}

/* ── poll(fd, timeout_ms) ─────────────────────────────────────────── */
/*
 * Returns 1 if data is available on fd, 0 if not.
 * For fd 0 (stdin) checks keyboard input queue.
 * For pipe read FDs checks the pipe buffer.
 * timeout_ms is currently ignored (always non-blocking).
 */
static unsigned long syscall_do_poll(unsigned long fd, unsigned long timeout_ms)
{
    (void)timeout_ms;

    /* stdin */
    if (fd == 0UL)
        return terminal_input_available() ? 1UL : 0UL;

    /* file / pipe FDs */
    {
        int slot = syscall_fd_slot_from_fd(fd);
        if (slot < 0 || syscall_fds[slot].type == FD_TYPE_NONE) return (unsigned long)-1;

        if (syscall_fds[slot].type == FD_TYPE_PIPE_READ)
        {
            int pi = syscall_fds[slot].pipe_idx;
            if (pi < 0 || pi >= PIPE_MAX) return (unsigned long)-1;
            return pipes[pi].count > 0 ? 1UL : 0UL;
        }

        /* Regular files always have data available (until EOF). */
        if (syscall_fds[slot].type == FD_TYPE_FILE)
            return 1UL;
    }

    return (unsigned long)-1;
}

unsigned long syscall_dispatch(unsigned long num, unsigned long a1, unsigned long a2,
                               unsigned long a3, unsigned long a4)
{
    /* Backward compatibility: old SYS_WRITE(ptr,len) callers (no fd arg). */
    if (num == SYS_WRITE && a1 >= 0x1000UL)
    {
        return syscall_write_user(a1, a2);
    }

    switch (num) {
    case SYS_READ:
        /* a1 = fd, a2 = user buffer, a3 = count */
        return syscall_read_fd(a1, a2, a3);
    case SYS_WRITE:
        /* a1 = fd, a2 = user pointer, a3 = byte length (0 means C-string mode). */
        return syscall_write_fd(a1, a2, a3);
    case SYS_OPEN:
        /* a1 = user pointer to NUL-terminated path */
        return syscall_open_user(a1);
    case SYS_CLOSE:
        /* a1 = fd */
        return syscall_close_fd(a1);
    case SYS_LSEEK:
        /* a1 = fd, a2 = offset (signed), a3 = whence */
        return syscall_lseek_fd(a1, (long)a2, a3);
    case SYS_MMAP:
        /* a1 = addr_hint, a2 = length, a3 = prot, a4 = flags */
        return syscall_do_mmap(a1, a2, a3, a4);
    case SYS_MUNMAP:
        /* a1 = addr, a2 = length */
        return syscall_do_munmap(a1, a2);
    case SYS_BRK:
        /* a1 = new break (0 queries current break) */
        return task_user_heap_brk(a1);
    case SYS_PIPE:
        /* a1 = user pointer to unsigned long[2] array */
        return syscall_do_pipe(a1);
    case SYS_KILL:
        /* a1 = pid, a2 = signal number */
        return syscall_do_kill(a1, a2);
    case SYS_GETPID:
        return syscall_do_getpid();
    case SYS_POLL:
        /* a1 = fd, a2 = timeout_ms (ignored for now) */
        return syscall_do_poll(a1, a2);
    case SYS_EXIT:
        task_exit();
        return 0;
    default:
        return (unsigned long)-1;
    }
}
