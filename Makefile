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

CFLAGS := -ffreestanding -O2 -Wall -Wextra -m64 -mno-red-zone -fno-pic -fno-pie -fno-stack-protector
LDFLAGS := -T linker.ld -ffreestanding -O2 -nostdlib -no-pie -Wl,-z,max-page-size=0x1000

BUILD_DIR := build
ISO_DIR := iso
ISO_NAME := TG11-OS.iso

OBJS := \
	$(BUILD_DIR)/boot32.o \
	$(BUILD_DIR)/longmode64.o \
	$(BUILD_DIR)/kernel.o

all: $(ISO_NAME)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot32.o: boot/boot32.s | $(BUILD_DIR)
	$(CC) -c $< -o $@

$(BUILD_DIR)/longmode64.o: boot/longmode64.s | $(BUILD_DIR)
	$(CC) -c $< -o $@

$(BUILD_DIR)/kernel.o: kernel/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(ISO_DIR)/boot/kernel.elf: $(OBJS) linker.ld
	mkdir -p $(ISO_DIR)/boot/grub
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc
	grub-file --is-x86-multiboot2 $@

$(ISO_NAME): $(ISO_DIR)/boot/kernel.elf iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(ISO_DIR)

run: $(ISO_NAME)
	qemu-system-x86_64 -cdrom $(ISO_NAME)

clean:
	rm -rf $(BUILD_DIR) $(ISO_DIR)/boot/kernel.elf $(ISO_NAME)

.PHONY: all run clean