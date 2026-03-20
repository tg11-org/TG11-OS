.intel_syntax noprefix

.section .multiboot
.align 8
mb2_header:
    .long 0xE85250D6                    # magic
    .long 0                             # architecture (i386)
    .long mb2_header_end - mb2_header   # header length
    .long -(0xE85250D6 + 0 + (mb2_header_end - mb2_header)) # checksum

    .align 8
    .short 5                            # framebuffer request
    .short 0
    .long 20
    .long 1024
    .long 768
    .long 32

    # end tag
    .align 8
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .text
.code32
.global _start
.extern long_mode_start

_start:
    cli

    mov esp, offset stack_top32

    # Build identity map for first 4 GiB using 2 MiB pages.
    # This keeps high framebuffer memory accessible during early boot.
    mov edi, offset pd_table0
    mov eax, 0x00000083          # present | writable | huge page
    mov ecx, 2048                # 2048 * 2 MiB = 4 GiB

map_pd_entries:
    mov [edi], eax
    mov dword ptr [edi + 4], 0
    add eax, 0x00200000
    add edi, 8
    dec ecx
    jnz map_pd_entries

    # Load 64-bit GDT
    lgdt [gdt64_ptr]

    # Load page table base into CR3
    mov eax, offset pml4_table
    mov cr3, eax

    # Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    # Enable long mode via EFER MSR
    mov ecx, 0xC0000080         # IA32_EFER
    rdmsr
    or eax, 1 << 8              # LME
    wrmsr

    # Enable paging + keep protected mode enabled
    mov eax, cr0
    or eax, 0x80000001          # PG | PE
    mov cr0, eax

    # Far jump into 64-bit code segment
    mov [mb2_info_ptr], ebx
    jmp 0x08:long_mode_start

hang32:
    hlt
    jmp hang32

.section .rodata
.align 8
gdt64:
    .quad 0x0000000000000000    # null
    .quad 0x00AF9A000000FFFF    # 64-bit code
    .quad 0x00AF92000000FFFF    # data
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .data
.align 4096
pml4_table:
    .quad pdpt_table + 0x003
    .fill 511, 8, 0

.align 4096
pdpt_table:
    .quad pd_table0 + 0x003
    .quad pd_table1 + 0x003
    .quad pd_table2 + 0x003
    .quad pd_table3 + 0x003
    .fill 508, 8, 0

.align 4096
pd_table0:
    .fill 512, 8, 0

.align 4096
pd_table1:
    .fill 512, 8, 0

.align 4096
pd_table2:
    .fill 512, 8, 0

.align 4096
pd_table3:
    .fill 512, 8, 0

.section .bss
.align 16
stack_bottom32:
    .skip 16384
stack_top32:

.global mb2_info_ptr
mb2_info_ptr:
    .long 0
