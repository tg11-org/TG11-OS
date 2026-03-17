# TG11-OS

Small x86_64 hobby OS kernel with:
- GRUB multiboot2 boot
- VGA terminal shell
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

## Shell Quickstart

### General
- `help`, `help fs`, `help disk`
- `version`
- `clear`, `reboot`, `shutdown`
- `run [-x] <path>` (shell script runner)
- `basic <path>` (Tiny BASIC text program runner)

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

## Tiny BASIC (initial)

Supported statements:
- `PRINT "text"` or `PRINT A`
- `LET A = 10`
- `ADD A 1`
- `IF A < 5 THEN 30`
- `GOTO 30`
- `END`

Example file: [scripts/demo.bas](scripts/demo.bas)
