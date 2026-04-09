# TG11-OS User-Space Programs

Write and build real C programs that run on TG11-OS.

## Quick Start

```bash
# Build all programs
make CC=cc

# Install to FAT32 data disk (requires mtools)
sudo apt-get install mtools   # one-time
make CC=cc install
```

Then in TG11-OS:
```
exec /hello.elf
exec /echo.elf hello world
exec /cat.elf /some/file.txt
exec /pid.elf
```

## Writing a New Program

1. Create `examples/myapp.c`:

```c
#include "tg11.h"

int main(int argc, char **argv)
{
    puts("Hello from my app!");
    return 0;
}
```

2. Run `make CC=cc` — it automatically finds all `.c` files in `examples/`.

3. Run `make CC=cc install` to deploy to the FAT32 data disk.

## Available API (`#include "tg11.h"`)

### Process
- `void exit(int code)` — terminate process
- `long getpid(void)` — get process ID
- `long kill(long pid, long sig)` — send signal

### File I/O
- `ssize_t write(int fd, const void *buf, size_t count)`
- `ssize_t read(int fd, void *buf, size_t count)`
- `long open(const char *path)` — open file, returns fd
- `long close(int fd)`
- `long lseek(int fd, long offset, int whence)`

### Memory
- `long brk(unsigned long addr)` — adjust heap break
- `void *mmap(void *addr, size_t len, int prot, int flags)`
- `long munmap(void *addr, size_t len)`

### IPC
- `long pipe(long fds[2])` — create pipe
- `long poll(int fd, long timeout_ms)` — check if data available

### Convenience
- `ssize_t puts(const char *s)` — print string + newline
- `ssize_t print(const char *s)` — print string (no newline)
- `void print_ulong(unsigned long v)` — print decimal number

### String Helpers
- `size_t strlen(const char *s)`
- `int strcmp(const char *a, const char *b)`
- `void *memset(void *s, int c, size_t n)`
- `void *memcpy(void *dst, const void *src, size_t n)`

## Syscall ABI

For raw syscalls, `#include "syscall.h"`:

```c
long syscall0(long num);
long syscall1(long num, long a1);
long syscall2(long num, long a1, long a2);
long syscall3(long num, long a1, long a2, long a3);
long syscall4(long num, long a1, long a2, long a3, long a4);
```

Registers: `rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4`. Return in `rax`.
