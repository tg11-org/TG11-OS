# TG11-OS

Small x86_64 hobby OS kernel with:
- GRUB multiboot2 boot
- VGA and framebuffer text terminal shell
- Keyboard IRQ input with history/editing/tab completion
- ATA PIO block I/O
- RAM filesystem commands
- FAT32 mount/read/write (8.3 names) with path-based shell operations

## Build

Requires a cross compiler (`x86_64-elf-gcc`) and GRUB tools.

```bash
make
```

## Run Modes

### ISO boot + FAT data disk (recommended)

```bash
make format-data-disk
make run
```

- Boots from ISO (`-boot d`) and attaches `TG11-DATA.img` as ATA disk.
- In the shell, run `fatmount` before FAT commands.

### Disk boot image workflow

```bash
make run-disk
```

- Boots from `TG11-DISK.img` (`-boot c`) and also attaches `TG11-DATA.img`.
- Uses the high-res-friendly path (`std` VGA + kernel framebuffer upscale when available).

### Safe fallback run targets

```bash
make run-safe
make run-disk-safe
```

- Uses conservative VGA launch settings if you need a fallback for host-specific QEMU issues.

### Host paste-friendly run mode (serial stdio)

```bash
make run-disk-serial
```

- Opens a host terminal-connected serial console via QEMU `-serial stdio`.
- You can paste from your host directly into the guest shell through that terminal.
- Current on-screen text selection inside the guest display is not implemented yet.

## Shell Quickstart

### General
- `help`, `help fs`, `help disk`
- `version`
- `color preview [text|prompt]`
- `display [show|vga25|vga50|fb|mode <show|list|1080p|900p|768p|720p|WIDTHxHEIGHT[xBPP]>]`
- `fbfont [show|list|style|size|reset|glyph|save|load]`
- `clear`, `reboot`, `shutdown`
- `run [-x] <path>` (shell script runner)
- `basic <path>` (Tiny BASIC text program runner)

### User-mode ELF smoke tests
- `exec /hello.elf` prints `hello`
- `exec /argc.elf a b c` prints `4`
- `exec /brk.elf` prints `OK` after growing and shrinking its heap with `SYS_BRK`
- `elfselftest` covers loader-only fixtures; the syscall-driven ELFs above are still best validated through `exec`

### Editor keys
- `Ctrl+S` save
- `Ctrl+F` find (Enter for next match, Shift+Enter for previous, Esc to cancel)
- `Ctrl+C/X/V` copy/cut/paste selection
- `Shift+Arrow` selection, `Ctrl+Arrow` word jumps
- Exiting editor restores the previous terminal screen context (alternate-screen style)

### Filesystem (auto-routes to FAT after `fatmount`)
- `pwd`, `ls [path]`, `cd <path>`
- `mkdir <path>`, `touch <path>`, `write <path> <text>`
- `cat <path>`, `rm <path>`, `cp <src> <dst>`, `mv <src> <dst>`

### FAT-specific
- `fatmount`
- `fatls`
- `fatcat <path>`
- `fattouch <path>`
- `fatwrite <path> <text>`
- `fatrm <path>`

## Notes

- FAT currently expects 8.3-compatible path components.
- FAT-mode `cp`/`mv` currently use an internal 4 KiB transfer buffer.
- Base RAMFS includes example scripts under `/scripts`.
- User-mode `exec` runs now get a fixed stack window plus a dynamic heap window. `SYS_BRK` (syscall `12`) returns the current break with argument `0`, and grows or shrinks the heap when passed a new break value inside that window.
- Do not mount `TG11-DATA.vhd` in Windows while QEMU is running; attach it only after the guest is shut down to avoid host-side caching/coherency issues.

## Bundled Demo Scripts

- [scripts/demo.sh](scripts/demo.sh)
- [scripts/c64-demo.sh](scripts/c64-demo.sh)
- [scripts/demo.bas](scripts/demo.bas)
- [scripts/tiny-basic-demo.bas](scripts/tiny-basic-demo.bas)

## Tiny BASIC

Supported statements:
- `PRINT "text"`, `PRINT A`, or mixed `PRINT "A=", A`
- `PRINT ...;` (suppress newline), `TAB(n)`, `SPC(n)`
- `LET A = (B + 2) * 3` (supports `+ - * / %` and parentheses)
- `DIM A(32)` and indexed access via `A(I)`
- `LET A$ = "hello"` and `PRINT A$` for simple string variables (`A$..Z$`)
- String helpers: `CHR$(n)`, `STR$(n)` and concatenation in string expressions
- `ADD A n`, `SUB A n`, `MUL A n`, `DIV A n`, `MOD A n`
- `FOR I = 1 TO 10 [STEP n]` with `NEXT [I]`
- `INPUT A` / `INPUT A$` and `INPUT "Prompt: "; A`
- Numeric built-ins: `ABS(x)`, `RND(n)`, `LEN(s$)`, `VAL(s$)`, `ASC(s$)`
- `IF <expr> <op> <expr> THEN <label>` (`=`, `==`, `!=`, `<>`, `<`, `<=`, `>`, `>=`)
- `ON <expr> GOTO <l1,l2,...>` and `ON <expr> GOSUB <l1,l2,...>`
- `DATA ...`, `READ ...`, `RESTORE [label]`
- `GOTO <label>`, `GOSUB <label>`, `RETURN`
- `LIST`, `RUN`, `END`, `STOP`

Example file: [scripts/tiny-basic-demo.bas](scripts/tiny-basic-demo.bas)
