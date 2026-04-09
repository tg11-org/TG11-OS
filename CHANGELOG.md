# Changelog

## 0.0.8 - 2026-04-03

### Added
- Minimal user-mode heap growth via `SYS_BRK` (`12`) with per-`exec` heap window management and cleanup on return to the kernel.
- Bundled `/brk.elf` user-mode smoke test that expands its heap, writes into the new page, shrinks the heap, and exits.

### Changed
- `exec` now initializes a heap region after the loaded ELF image and before the fixed user stack window.
- README updated with the new ELF smoke-test workflow and `SYS_BRK` notes.

### Fixed
- Reduced pressure on hardcoded user-memory layout by giving user ELFs a syscall path to request heap space instead of relying only on loader-time mappings.

## 0.0.7 - 2026-03-19

### Added
- Framebuffer boot path with a working framebuffer text terminal.
- `display fb` framebuffer text mode switching.
- `fbfont` font controls with switchable styles, size presets, custom glyph editing, and RAMFS profile save/load.
- `color preview [text|prompt]` palette and current-attribute preview support.
- Bundled RAMFS demo content under `/scripts` including shell and Tiny BASIC examples.

### Changed
- README updated for framebuffer terminal usage and bundled demos.
- Version bumped to `v0.0.7`.

### Fixed
- Framebuffer boot stability issues caused by early page mapping limits.
- Framebuffer cursor trail and lowercase/punctuation glyph rendering.
- Framebuffer editor layout issues caused by hardcoded 80-column assumptions.

## 0.0.6 - 2026-03-18

### Added
- Theme commands and bundled prompt/text theme presets.
- `ramfs2fat` for copying RAMFS content to the mounted FAT volume.
- Dual-drive ATA/block-device support with `fatmount [0|1]` drive selection.
- Expanded help coverage with command paging via `help commands [page]`.
- `fatattr` for inspecting FAT file attributes.
- Serial terminal quality-of-life options including serial input, mirror toggles, compact mode, and RX echo control.
- VGA 80x50 display mode support.

### Changed
- QEMU run targets now use larger default display sizing for easier terminal use.
- Terminal input handling improved for arrow, `Home`, and `End` navigation.
- FAT/RAMFS workflows were expanded to better support copying and multi-disk setups.

### Fixed
- Serial responsiveness and shell interaction issues during interactive use.
- VGA 80x50 font clipping and related text rendering problems.

## 0.0.5 - 2026-03-17

### Added
- Tiny BASIC interpreter (`basic <path>`) for direct text program execution.
- Initial BASIC statements: `PRINT`, `LET`, `ADD`, `IF ... THEN`, `GOTO`, `END`, `REM`.
- Sample BASIC program: [scripts/demo.bas](scripts/demo.bas).
- Shell script runner command: `run [-x] <path>`.
- Script conditionals: `if`, `elif`, `else`, `fi`.
- Script loop syntax: `foreach` / `for` one-line form (`... do <cmd>`).
- Command substitution support via `$(...)` (including `$(version)`, `$(pwd)`, `$(cat <path>)`).
- Terminal editor command: `edit <path>` with `F10` save+exit, `Esc` cancel, `Ctrl+S` save.
- Demo shell script: [scripts/demo.sh](scripts/demo.sh).

### Changed
- `run -x` now traces executed script lines.
- Editor now has cursor-aware editing and navigation (`Left/Right/Up/Down`, `Home`, `End`).
- Editor now shows a live `[modified]` indicator in header.

### Fixed
- QEMU boot target handling improved via explicit `-boot` mode in run targets.
- FAT mount workflow/documentation updated for reliable ISO+data-disk runs.
- Editor redraw now clears stale characters after deletes.
- Reverted unstable low-level keyboard typematic programming that could block input in some VM setups.

## 0.0.4 - 2026-03-17

### Added
- `version` command in shell.
- Centralized OS version string via `TG11_OS_VERSION`.
- FAT32 path-based APIs for:
  - list directory
  - read/write file
  - create file
  - create directory
  - remove file/empty directory
- FAT cwd tracking in terminal.
- Help pagination by topic (`help`, `help fs`, `help disk`).

### Changed
- Terminal startup banner now reads version from a single source.
- Generic filesystem commands (`pwd`, `ls`, `cd`, `mkdir`, `touch`, `write`, `cat`, `rm`, `cp`, `mv`) route to FAT when mounted.
- `fatcat`, `fattouch`, `fatwrite`, `fatrm` now accept path arguments.
- Makefile run targets now force explicit boot device:
  - `run` / `run-debug`: `-boot d`
  - `run-disk` / `run-disk-debug`: `-boot c`

### Fixed
- FAT mount failures caused by running with boot image as primary non-FAT disk in some run modes.

## 0.0.3 - 2026-03-17

### Added
- ATA PIO storage driver and block device abstraction.
- FAT32 mount/list/read support and FAT shell commands (`fatmount`, `fatls`, `fatcat`).
- Writable FAT root operations (`fattouch`, `fatwrite`, `fatrm`).
- Disk-image workflows in Makefile (`run`, `run-disk`, data disk preparation/format targets).

### Changed
- Generic shell `ls`/`cat` behavior began preferring FAT after successful mount.

## 0.0.2 - 2026-03-16

### Added
- Improved terminal UX:
  - Shift/CapsLock support
  - line editing + arrow navigation
  - command history + tab completion
  - colored prompt + hardware cursor syncing
- Diagnostics and utility commands (`memmap`, `hexdump`, ATA sector helpers).
- RAM filesystem with shell commands (`pwd`, `ls`, `cd`, `mkdir`, `touch`, `write`, `cat`, `rm`, `cp`, `mv`).

## 0.0.1 - 2026-03-15

### Added
- Initial 64-bit kernel boot path with GRUB multiboot2.
- Basic VGA terminal and command loop.
- Early shell commands (`help`, `echo`, `clear`, `shutdown`) and reboot flow.
- Interrupt foundation (IDT/PIC) and keyboard input handling.