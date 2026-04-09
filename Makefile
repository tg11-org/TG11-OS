#  Copyright  (C)  2026  TG11
#  
#  This  program  is  free  software:  you  can  redistribute  it  and/or  modify
#  it  under  the  terms  of  the  GNU  Affero  General  Public  License  as
#  published  by  the  Free  Software  Foundation,  either  version  3  of  the
#  License,  or  (at  your  option)  any  later  version.
#  
#  This  program  is  distributed  in  the  hope  that  it  will  be  useful,
#  but  WITHOUT  ANY  WARRANTY;  without  even  the  implied  warranty  of
#  MERCHANTABILITY  or  FITNESS  FOR  A  PARTICULAR  PURPOSE.    See  the
#  GNU  Affero  General  Public  License  for  more  details.
#  
#  You  should  have  received  a  copy  of  the  GNU  Affero  General  Public  License
#  along  with  this  program.    If  not,  see  <https://www.gnu.org/licenses/>.

TARGET  :=  x86_64-elf
CC  ?=  $(TARGET)-gcc
NM  ?=  nm

CFLAGS  :=  -Iinclude  -ffreestanding  -O2  -Wall  -Wextra  -m64  -mno-red-zone  -mno-mmx  -mno-sse  -mno-sse2  -fno-pic  -fno-pie  -fno-stack-protector
LDFLAGS  :=  -T  linker.ld  -ffreestanding  -O2  -nostdlib  -no-pie  -Wl,-z,max-page-size=0x1000

BUILD_DIR  :=  build
ISO_DIR  :=  iso
ISO_NAME  :=  TG11-OS.iso
BOOT_DISK_NAME  :=  TG11-DISK.img
DATA_DISK_NAME  :=  TG11-DATA.vhd
DATA_DISK1_NAME  :=  TG11-DATA-1.vhd
DATA_DISK2_NAME  :=  TG11-DATA-2.vhd
DATA_DISK3_NAME  :=  TG11-DATA-3.vhd
OVMF_CODE  :=  /usr/share/OVMF/OVMF_CODE.fd
BOOT_DISK_PATH  :=  $(abspath  $(BOOT_DISK_NAME))
DATA_DISK_PATH  :=  $(abspath  $(DATA_DISK_NAME))
DATA_DISK1_PATH  :=  $(abspath  $(DATA_DISK1_NAME))
DATA_DISK2_PATH  :=  $(abspath  $(DATA_DISK2_NAME))
DATA_DISK3_PATH  :=  $(abspath  $(DATA_DISK3_NAME))
QEMU_DRIVE_CACHE  :=  cache=writethrough
QEMU_DATA_DRIVE_ISO  :=  -drive  file=$(DATA_DISK_PATH),format=raw,if=ide,index=0,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_DATA1_DRIVE_ISO  :=  -drive  file=$(DATA_DISK1_PATH),format=raw,if=ide,index=1,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_DATA2_DRIVE_ISO  :=  -drive  file=$(DATA_DISK2_PATH),format=raw,if=ide,index=2,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_DATA3_DRIVE_ISO  :=  -drive  file=$(DATA_DISK3_PATH),format=raw,if=ide,index=3,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_DATA_DRIVES_ISO_4  :=  $(QEMU_DATA_DRIVE_ISO)  $(QEMU_DATA1_DRIVE_ISO)  $(QEMU_DATA2_DRIVE_ISO)  $(QEMU_DATA3_DRIVE_ISO)
QEMU_CDROM_AHCI  :=  -drive  file=$(ISO_NAME),format=raw,if=none,id=cdrom4,media=cdrom  -device  ahci,id=ahci4  -device  ide-cd,drive=cdrom4,bus=ahci4.0
QEMU_BOOT_DISK_AHCI  :=  -drive  file=$(BOOT_DISK_PATH),format=raw,if=none,id=bootdisk4,media=disk,$(QEMU_DRIVE_CACHE)  -device  ahci,id=ahci_boot4  -device  ide-hd,drive=bootdisk4,bus=ahci_boot4.0,bootindex=0
QEMU_BOOT_DRIVE  :=  -drive  file=$(BOOT_DISK_PATH),format=raw,if=ide,index=0,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_DATA_DRIVE_DISK  :=  -drive  file=$(DATA_DISK_PATH),format=raw,if=ide,index=1,media=disk,$(QEMU_DRIVE_CACHE)
QEMU_NET := -netdev user,id=net0 -device e1000,netdev=net0
QEMU_USB := -device piix3-usb-uhci -device usb-tablet

CORE_OBJS  :=  \
	$(BUILD_DIR)/longmode64.o  \
	$(BUILD_DIR)/interrupts.o  \
	$(BUILD_DIR)/gdt.o  \
	$(BUILD_DIR)/idt.o  \
	$(BUILD_DIR)/kernel.o  \
	$(BUILD_DIR)/terminal.o  \
	$(BUILD_DIR)/screen.o  \
	$(BUILD_DIR)/serial.o  \
	$(BUILD_DIR)/ata.o  \
	$(BUILD_DIR)/blockdev.o  \
	$(BUILD_DIR)/mouse.o  \
	$(BUILD_DIR)/memmap.o  \
	$(BUILD_DIR)/memory.o  \
	$(BUILD_DIR)/timer.o  \
	$(BUILD_DIR)/elf.o  \
	$(BUILD_DIR)/framebuffer.o  \
	$(BUILD_DIR)/fs.o  \
	$(BUILD_DIR)/fat32.o  \
	$(BUILD_DIR)/basic.o  \
	$(BUILD_DIR)/task_switch.o  \
	$(BUILD_DIR)/task.o  \
	$(BUILD_DIR)/serial_console.o  \
	$(BUILD_DIR)/syscall_asm.o  \
	$(BUILD_DIR)/syscall.o  \
	$(BUILD_DIR)/pci.o  \
	$(BUILD_DIR)/e1000.o  \
	$(BUILD_DIR)/net.o  \
	$(BUILD_DIR)/usb.o

OBJS  :=  \
	$(BUILD_DIR)/boot32.o  \
	$(CORE_OBJS)

OBJS_FB  :=  \
	$(BUILD_DIR)/boot32_fb.o  \
	$(CORE_OBJS)

all:  check-toolchain  $(ISO_NAME)

check-toolchain:
	@command  -v  $(CC)  >/dev/null  2>&1  ||  {  echo  "error:  $(CC)  not  found  in  PATH";  exit  1;  }
	@cc_path=$$(command  -v  $(CC));  test  -x  "$$cc_path"  ||  {  echo  "error:  $(CC)  is  not  executable:  $$cc_path";  exit  1;  }

$(BUILD_DIR):
	mkdir  -p  $(BUILD_DIR)

$(BUILD_DIR)/boot32.o:  arch/x86_64/boot32.s  Makefile  |  $(BUILD_DIR)
	$(CC)  -c  $<  -o  $@

$(BUILD_DIR)/boot32_fb.o:  arch/x86_64/boot32_fb.s  Makefile  |  $(BUILD_DIR)
	$(CC)  -c  $<  -o  $@

$(BUILD_DIR)/longmode64.o:  arch/x86_64/longmode64.s  Makefile  |  $(BUILD_DIR)
	$(CC)  -c  $<  -o  $@

$(BUILD_DIR)/interrupts.o:  arch/x86_64/interrupts.s  Makefile  |  $(BUILD_DIR)
	$(CC)  -c  $<  -o  $@

$(BUILD_DIR)/gdt.o:  arch/x86_64/gdt.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/idt.o:  arch/x86_64/idt.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/kernel.o:  kernel/kernel.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/terminal.o:  kernel/terminal.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/screen.o:  drivers/screen.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/serial.o:  drivers/serial.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/ata.o:  drivers/ata.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/blockdev.o:  drivers/blockdev.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/mouse.o:  drivers/mouse.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/memmap.o:  kernel/memmap.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/memory.o:  kernel/memory.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/timer.o:  kernel/timer.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/elf.o:  kernel/elf.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/task_switch.o:  arch/x86_64/task_switch.s  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/task.o:  kernel/task.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/serial_console.o:  kernel/serial_console.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/syscall_asm.o:  arch/x86_64/syscall.s  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/syscall.o:  kernel/syscall.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/framebuffer.o:  kernel/framebuffer.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/fs.o:  kernel/fs.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/fat32.o:  kernel/fat32.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/basic.o:  kernel/basic.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/pci.o:  drivers/pci.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/e1000.o:  drivers/e1000.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/net.o:  kernel/net.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/usb.o:  drivers/usb.c  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/ksym.o:  kernel/ksym.c  include/ksym.h  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

#  Two-pass  kernel  build:  first  link  produces  a  partial  binary  used  to  extract
#  symbol  addresses  via  nm;  those  symbols  are  compiled  into  kernel_syms.o  and
#  linked  back  in  for  the  final  binary.
#
#  ksym.o  is  included  in  the  partial  link  so  that  idt.o's  call  to  ksym_lookup
#  resolves.    ksym.o's  three  data  externals  (ksym_names/ksym_table/count)  are
#  provided  only  by  the  generated  kernel_syms.o,  so  we  allow  unresolved  symbols
#  during  the  partial  link  only.

$(BUILD_DIR)/kernel.partial.elf:  $(OBJS)  $(BUILD_DIR)/ksym.o  linker.ld
	$(CC)  $(LDFLAGS)  -Wl,--unresolved-symbols=ignore-all  -o  $@  $(OBJS)  $(BUILD_DIR)/ksym.o  -lgcc

$(BUILD_DIR)/kernel_fb.partial.elf:  $(OBJS_FB)  $(BUILD_DIR)/ksym.o  linker.ld
	$(CC)  $(LDFLAGS)  -Wl,--unresolved-symbols=ignore-all  -o  $@  $(OBJS_FB)  $(BUILD_DIR)/ksym.o  -lgcc

$(BUILD_DIR)/kernel_syms.c:  $(BUILD_DIR)/kernel.partial.elf  tools/gen_kernel_syms.sh
	sh  tools/gen_kernel_syms.sh  $(NM)  $<  >  $@

$(BUILD_DIR)/kernel_syms_fb.c:  $(BUILD_DIR)/kernel_fb.partial.elf  tools/gen_kernel_syms.sh
	sh  tools/gen_kernel_syms.sh  $(NM)  $<  >  $@

$(BUILD_DIR)/kernel_syms.o:  $(BUILD_DIR)/kernel_syms.c  include/ksym.h  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(BUILD_DIR)/kernel_syms_fb.o:  $(BUILD_DIR)/kernel_syms_fb.c  include/ksym.h  Makefile  |  $(BUILD_DIR)
	$(CC)  $(CFLAGS)  -c  $<  -o  $@

$(ISO_DIR)/boot/kernel.elf:  $(OBJS)  $(BUILD_DIR)/ksym.o  $(BUILD_DIR)/kernel_syms.o  linker.ld
	mkdir  -p  $(ISO_DIR)/boot/grub
	$(CC)  $(LDFLAGS)  -o  $@  $(OBJS)  $(BUILD_DIR)/ksym.o  $(BUILD_DIR)/kernel_syms.o  -lgcc
	grub-file  --is-x86-multiboot2  $@

$(ISO_DIR)/boot/kernel-fb.elf:  $(OBJS_FB)  $(BUILD_DIR)/ksym.o  $(BUILD_DIR)/kernel_syms_fb.o  linker.ld
	mkdir  -p  $(ISO_DIR)/boot/grub
	$(CC)  $(LDFLAGS)  -o  $@  $(OBJS_FB)  $(BUILD_DIR)/ksym.o  $(BUILD_DIR)/kernel_syms_fb.o  -lgcc
	grub-file  --is-x86-multiboot2  $@

$(ISO_NAME):  $(ISO_DIR)/boot/kernel.elf  $(ISO_DIR)/boot/kernel-fb.elf  iso/boot/grub/grub.cfg
	grub-mkrescue  -o  $@  $(ISO_DIR)

$(BOOT_DISK_NAME):  $(ISO_NAME)
	cp  $(ISO_NAME)  $(BOOT_DISK_NAME)

prepare-data-disk:
	@if  [  !  -f  $(DATA_DISK_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK_NAME);  fi
	@if  !  command  -v  mkfs.fat  >/dev/null  2>&1;  then  echo  "mkfs.fat  not  found  (install  dosfstools)";  exit  1;  fi
	@if  !  file  $(DATA_DISK_NAME)  |  grep  -qi  "FAT";  then  mkfs.fat  -F  32  $(DATA_DISK_NAME);  fi

prepare-data-disks:  prepare-data-disk
	@if  [  !  -f  $(DATA_DISK1_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK1_NAME);  fi
	@if  [  !  -f  $(DATA_DISK2_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK2_NAME);  fi
	@if  [  !  -f  $(DATA_DISK3_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK3_NAME);  fi
	@if  !  command  -v  mkfs.fat  >/dev/null  2>&1;  then  echo  "mkfs.fat  not  found  (install  dosfstools)";  exit  1;  fi
	@if  !  file  $(DATA_DISK1_NAME)  |  grep  -qi  "FAT";  then  mkfs.fat  -F  32  $(DATA_DISK1_NAME);  fi
	@if  !  file  $(DATA_DISK2_NAME)  |  grep  -qi  "FAT";  then  mkfs.fat  -F  32  $(DATA_DISK2_NAME);  fi
	@if  !  file  $(DATA_DISK3_NAME)  |  grep  -qi  "FAT";  then  mkfs.fat  -F  32  $(DATA_DISK3_NAME);  fi

format-data-disk:
	@if  [  !  -f  $(DATA_DISK_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK_NAME);  fi
	@if  !  command  -v  mkfs.fat  >/dev/null  2>&1;  then  echo  "mkfs.fat  not  found  (install  dosfstools)";  exit  1;  fi
	mkfs.fat  -F  32  $(DATA_DISK_NAME)

format-data-disks:
	@if  [  !  -f  $(DATA_DISK1_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK1_NAME);  fi
	@if  [  !  -f  $(DATA_DISK2_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK2_NAME);  fi
	@if  [  !  -f  $(DATA_DISK3_NAME)  ];  then  truncate  -s  64M  $(DATA_DISK3_NAME);  fi
	mkfs.fat  -F  32  $(DATA_DISK1_NAME)
	mkfs.fat  -F  32  $(DATA_DISK2_NAME)
	mkfs.fat  -F  32  $(DATA_DISK3_NAME)

fatcheck-data-disk:
	@if  [  !  -f  $(DATA_DISK_NAME)  ];  then  echo  "$(DATA_DISK_NAME)  not  found";  exit  1;  fi
	@if  !  command  -v  fsck.fat  >/dev/null  2>&1;  then  echo  "fsck.fat  not  found  (install  dosfstools)";  exit  1;  fi
	fsck.fat  -vn  $(DATA_DISK_NAME)

fatcheck-data-disks:
	@if  [  !  -f  $(DATA_DISK1_NAME)  ];  then  echo  "$(DATA_DISK1_NAME)  not  found";  exit  1;  fi
	@if  [  !  -f  $(DATA_DISK2_NAME)  ];  then  echo  "$(DATA_DISK2_NAME)  not  found";  exit  1;  fi
	@if  [  !  -f  $(DATA_DISK3_NAME)  ];  then  echo  "$(DATA_DISK3_NAME)  not  found";  exit  1;  fi
	@if  !  command  -v  fsck.fat  >/dev/null  2>&1;  then  echo  "fsck.fat  not  found  (install  dosfstools)";  exit  1;  fi
	fsck.fat  -vn  $(DATA_DISK1_NAME)
	fsck.fat  -vn  $(DATA_DISK2_NAME)
	fsck.fat  -vn  $(DATA_DISK3_NAME)

run:  $(ISO_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(w)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -boot  d  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-big:  $(ISO_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -boot  d  -display  gtk,zoom-to-fit=on  -full-screen  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-debug:  $(ISO_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -no-reboot  -boot  d  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-4drives:  $(ISO_NAME)  prepare-data-disks
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -boot  d  $(QEMU_CDROM_AHCI)  $(QEMU_DATA_DRIVES_ISO_4)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-4drives-debug:  $(ISO_NAME)  prepare-data-disks
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -no-reboot  -boot  d  $(QEMU_CDROM_AHCI)  $(QEMU_DATA_DRIVES_ISO_4)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk:  $(BOOT_DISK_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -D  QEMU.log  -d  cpu_reset

run-disk-big:  $(BOOT_DISK_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  c  -display  gtk,zoom-to-fit=on  -full-screen  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -D  QEMU.log  -d  cpu_reset

run-disk-debug:  $(BOOT_DISK_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -no-reboot  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk-4drives:  $(BOOT_DISK_NAME)  prepare-data-disks
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1280  -global  VGA.yres=800  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -boot  c  $(QEMU_BOOT_DISK_AHCI)  $(QEMU_DATA_DRIVES_ISO_4)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk-4drives-debug:  $(BOOT_DISK_NAME)  prepare-data-disks
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -global  VGA.xres=1920  -global  VGA.yres=1080  -global  VGA.xmax=1920  -global  VGA.ymax=1080  -no-reboot  -boot  c  $(QEMU_BOOT_DISK_AHCI)  $(QEMU_DATA_DRIVES_ISO_4)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk-serial:  $(BOOT_DISK_NAME)  prepare-data-disk
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -serial  stdio  ||  {  code=$$?;  echo  "qemu  exited  with  $$code  (interactive  run  target,  ignoring)";  true;  }

run-safe:  $(ISO_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -boot  d  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -D  QEMU.log  -d  cpu_reset

run-disk-safe:  $(BOOT_DISK_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -D  QEMU.log  -d  cpu_reset

run-disk-safe-serial:  $(BOOT_DISK_NAME)  prepare-data-disk
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -serial  stdio  ||  {  code=$$?;  echo  "qemu  exited  with  $$code  (interactive  run  target,  ignoring)";  true;  }

run-hires:  $(ISO_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  d  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -D  QEMU.log  -d  cpu_reset

run-disk-hires:  $(BOOT_DISK_NAME)  prepare-data-disk
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -D  QEMU.log  -d  cpu_reset

run-disk-hires-serial:  $(BOOT_DISK_NAME)  prepare-data-disk
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -vga  std  -global  VGA.vgamem_mb=64  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -serial  stdio  ||  {  code=$$?;  echo  "qemu  exited  with  $$code  (interactive  run  target,  ignoring)";  true;  }

run-uefi-hires:  $(ISO_NAME)  prepare-data-disk
	@if  [  !  -f  $(OVMF_CODE)  ];  then  echo  "error:  OVMF  firmware  not  found  at  $(OVMF_CODE)";  echo  "install  package:  ovmf";  exit  1;  fi
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -machine  q35  -bios  $(OVMF_CODE)  -vga  none  -device  virtio-vga,xres=1920,yres=1080,max_outputs=1  -boot  d  -cdrom  $(ISO_NAME)  $(QEMU_DATA_DRIVE_ISO)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk-uefi-hires:  $(BOOT_DISK_NAME)  prepare-data-disk
	@if  [  !  -f  $(OVMF_CODE)  ];  then  echo  "error:  OVMF  firmware  not  found  at  $(OVMF_CODE)";  echo  "install  package:  ovmf";  exit  1;  fi
	rm  -f  QEMU.log
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -machine  q35  -bios  $(OVMF_CODE)  -vga  none  -device  virtio-vga,xres=1920,yres=1080,max_outputs=1  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -d  int,cpu_reset  >>  QEMU.log  2>&1

run-disk-uefi-hires-serial:  $(BOOT_DISK_NAME)  prepare-data-disk
	@if  [  !  -f  $(OVMF_CODE)  ];  then  echo  "error:  OVMF  firmware  not  found  at  $(OVMF_CODE)";  echo  "install  package:  ovmf";  exit  1;  fi
	qemu-system-x86_64  $(QEMU_NET) $(QEMU_USB)  -machine  q35  -bios  $(OVMF_CODE)  -vga  none  -device  virtio-vga,xres=1920,yres=1080,max_outputs=1  -boot  c  $(QEMU_BOOT_DRIVE)  $(QEMU_DATA_DRIVE_DISK)  -serial  stdio  ||  {  code=$$?;  echo  "qemu  exited  with  $$code  (interactive  run  target,  ignoring)";  true;  }

clean:
	rm  -rf  $(BUILD_DIR)  $(ISO_DIR)/boot/kernel.elf  $(ISO_DIR)/boot/kernel-fb.elf  $(ISO_NAME)  $(BOOT_DISK_NAME)  QEMU.log
	@if  [  "$(CLEAN_DATA)"  =  "1"  ];  then  \
		rm  -f  $(DATA_DISK_NAME)  $(DATA_DISK1_NAME)  $(DATA_DISK2_NAME)  $(DATA_DISK3_NAME);  \
		echo  "removed  data  disks  (CLEAN_DATA=1)";  \
	else  \
		echo  "preserved  data  disks  (set  CLEAN_DATA=1  to  remove)";  \
	fi

clean-data:
	rm  -f  $(DATA_DISK_NAME)  $(DATA_DISK1_NAME)  $(DATA_DISK2_NAME)  $(DATA_DISK3_NAME)

distclean:  clean  clean-data

user:
	$(MAKE)  -C  user

user-install:  user
	$(MAKE)  -C  user  install

user-clean:
	$(MAKE)  -C  user  clean

.PHONY:  all  run  run-big  run-debug  run-4drives  run-4drives-debug  run-disk  run-disk-4drives  run-disk-4drives-debug  run-disk-big  run-disk-debug  run-disk-serial  run-safe  run-disk-safe  run-disk-safe-serial  run-hires  run-disk-hires  run-disk-hires-serial  run-uefi-hires  run-disk-uefi-hires  run-disk-uefi-hires-serial  prepare-data-disk  prepare-data-disks  format-data-disk  format-data-disks  fatcheck-data-disk  fatcheck-data-disks  clean  clean-data  distclean  user  user-install  user-clean