# Copyright (C) 2026 TG11
#
# Makefile for TG11-OS user-space programs.
#
# Usage:
#   make                    — build all example programs
#   make install            — copy ELFs to the FAT32 data disk
#   make clean              — remove build artifacts

TARGET := x86_64-elf
CC  ?= $(TARGET)-gcc
AS  := $(CC)
LD  := $(CC)

CFLAGS  := -Iinclude -ffreestanding -O2 -Wall -Wextra -m64 \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -fno-pic -fno-pie -fno-stack-protector -nostdinc
ASFLAGS := -c
LDFLAGS := -T link.ld -ffreestanding -O2 -nostdlib -no-pie \
           -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none

BUILD     := build
EXAMPLES  := examples
CRT0      := $(BUILD)/crt0.o

# Automatically find all .c files in examples/
SOURCES := $(wildcard $(EXAMPLES)/*.c)
ELFS    := $(patsubst $(EXAMPLES)/%.c,$(BUILD)/%.elf,$(SOURCES))

DATA_DISK := ../TG11-DATA.vhd

.PHONY: all clean install

all: $(ELFS)

$(BUILD):
	mkdir -p $(BUILD)

$(CRT0): crt0.s | $(BUILD)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/%.o: $(EXAMPLES)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/%.o $(CRT0) link.ld
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $<

install: all
	@if [ ! -f $(DATA_DISK) ]; then echo "error: $(DATA_DISK) not found"; exit 1; fi
	@command -v mcopy >/dev/null 2>&1 || { echo "error: mcopy not found (install mtools)"; exit 1; }
	@for elf in $(ELFS); do \
		name=$$(basename $$elf); \
		echo "  MCOPY $$name -> $(DATA_DISK):/$$name"; \
		mcopy -o -i $(DATA_DISK) $$elf ::/$${name}; \
	done

clean:
	rm -rf $(BUILD)
