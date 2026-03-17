# Copyright (C) 2026 TG11
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
# 
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

TARGET := x86_64-elf
CC := $(TARGET)-gcc

CFLAGS := -Iinclude -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -fno-pie -fno-stack-protector
LDFLAGS := -T linker.ld -ffreestanding -O2 -nostdlib -no-pie -Wl,-z,max-page-size=0x1000

BUILD_DIR := build
ISO_DIR := iso
ISO_NAME := TG11-OS.iso
BOOT_DISK_NAME := TG11-DISK.img
DATA_DISK_NAME := TG11-DATA.img

OBJS := \
	$(BUILD_DIR)/boot32.o \
	$(BUILD_DIR)/longmode64.o \
	$(BUILD_DIR)/interrupts.o \
	$(BUILD_DIR)/idt.o \
	$(BUILD_DIR)/kernel.o \
	$(BUILD_DIR)/terminal.o \
	$(BUILD_DIR)/screen.o \
	$(BUILD_DIR)/serial.o \
	$(BUILD_DIR)/ata.o \
	$(BUILD_DIR)/blockdev.o \
	$(BUILD_DIR)/mouse.o \
	$(BUILD_DIR)/memmap.o \
	$(BUILD_DIR)/fs.o \
	$(BUILD_DIR)/fat32.o \
	$(BUILD_DIR)/basic.o

all: $(ISO_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot32.o: arch/x86_64/boot32.s Makefile | $(BUILD_DIR)
	$(CC) -c $< -o $@

$(BUILD_DIR)/longmode64.o: arch/x86_64/longmode64.s Makefile | $(BUILD_DIR)
	$(CC) -c $< -o $@

$(BUILD_DIR)/interrupts.o: arch/x86_64/interrupts.s Makefile | $(BUILD_DIR)
	$(CC) -c $< -o $@

$(BUILD_DIR)/idt.o: arch/x86_64/idt.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel.o: kernel/kernel.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/terminal.o: kernel/terminal.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/screen.o: drivers/screen.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: drivers/serial.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ata.o: drivers/ata.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/blockdev.o: drivers/blockdev.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/mouse.o: drivers/mouse.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memmap.o: kernel/memmap.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs.o: kernel/fs.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: kernel/fat32.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/basic.o: kernel/basic.c Makefile | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(ISO_DIR)/boot/kernel.elf: $(OBJS) linker.ld
	mkdir -p $(ISO_DIR)/boot/grub
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	grub-file --is-x86-multiboot2 $@

$(ISO_NAME): $(ISO_DIR)/boot/kernel.elf iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

$(BOOT_DISK_NAME): $(ISO_NAME)
	cp $(ISO_NAME) $(BOOT_DISK_NAME)

prepare-data-disk:
	@if [ ! -f $(DATA_DISK_NAME) ]; then truncate -s 64M $(DATA_DISK_NAME); fi
	@if ! command -v mkfs.fat >/dev/null 2>&1; then echo "mkfs.fat not found (install dosfstools)"; exit 1; fi
	@if ! file $(DATA_DISK_NAME) | grep -qi "FAT"; then mkfs.fat -F 32 $(DATA_DISK_NAME); fi

format-data-disk:
	@if [ ! -f $(DATA_DISK_NAME) ]; then truncate -s 64M $(DATA_DISK_NAME); fi
	@if ! command -v mkfs.fat >/dev/null 2>&1; then echo "mkfs.fat not found (install dosfstools)"; exit 1; fi
	mkfs.fat -F 32 $(DATA_DISK_NAME)

run: $(ISO_NAME) prepare-data-disk
	rm -f QEMU.log
	qemu-system-x86_64 -boot d -cdrom $(ISO_NAME) -drive file=$(DATA_DISK_NAME),format=raw,if=ide,index=0,media=disk -d int,cpu_reset >> QEMU.log 2>&1

run-debug: $(ISO_NAME) prepare-data-disk
	rm -f QEMU.log
	qemu-system-x86_64 -no-reboot -boot d -cdrom $(ISO_NAME) -drive file=$(DATA_DISK_NAME),format=raw,if=ide,index=0,media=disk -d int,cpu_reset >> QEMU.log 2>&1

run-disk: $(BOOT_DISK_NAME) prepare-data-disk
	rm -f QEMU.log
	qemu-system-x86_64 -boot c -drive file=$(BOOT_DISK_NAME),format=raw,if=ide,index=0,media=disk -drive file=$(DATA_DISK_NAME),format=raw,if=ide,index=1,media=disk -d int,cpu_reset >> QEMU.log 2>&1

run-disk-debug: $(BOOT_DISK_NAME) prepare-data-disk
	rm -f QEMU.log
	qemu-system-x86_64 -no-reboot -boot c -drive file=$(BOOT_DISK_NAME),format=raw,if=ide,index=0,media=disk -drive file=$(DATA_DISK_NAME),format=raw,if=ide,index=1,media=disk -d int,cpu_reset >> QEMU.log 2>&1

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)/boot/kernel.elf $(ISO_NAME) $(BOOT_DISK_NAME) $(DATA_DISK_NAME) QEMU.log

.PHONY: all run run-debug run-disk run-disk-debug prepare-data-disk format-data-disk clean