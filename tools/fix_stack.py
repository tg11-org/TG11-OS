import sys

with open('/mnt/u/Projects/TG11-OS/kernel/terminal.c', 'r') as f:
    content = f.read()

old = (
    '\t\t/* Build a minimal userspace stack frame: argc, argv[0]=NULL, envp[0]=NULL.\n'
    '\t\t * This is sufficient for current built-in test ELFs and avoids fragile\n'
    '\t\t * argv string packing while we stabilize user stack handling. */\n'
    '\t\tuser_rsp &= ~0xFUL;\n'
    '\t\tif (user_rsp < user_stack_base + 8UL * 4UL)\n'
    '\t\t{\n'
    '\t\t\tterminal_write_line("[exec] insufficient stack for startup frame");\n'
    '\t\t\ttask_user_heap_reset();\n'
    '\t\t\tfor (i = 0; i < USER_STACK_PAGES; i++)\n'
    '\t\t\t{\n'
    '\t\t\t\tpage = user_stack_base + (unsigned long)i * 4096UL;\n'
    '\t\t\t\tpaging_unmap_page(page);\n'
    '\t\t\t}\n'
    '\t\t\tvirt_free_pages(stack_alias_base, USER_STACK_PAGES);\n'
    '\t\t\telf_unload(elf_shell_io_buf, size);\n'
    '\t\t\treturn;\n'
    '\t\t}\n'
    '\t\tuser_rsp -= 8UL;\n'
    '\t\t{\n'
    '\t\t\tunsigned long stack_slot = user_rsp - user_stack_base;\n'
    '\t\t\t*((volatile unsigned long *)(stack_alias_base + stack_slot)) = 0UL; /* envp[0] = NULL */\n'
    '\t\t}\n'
    '\t\tuser_rsp -= 8UL;\n'
    '\t\t{\n'
    '\t\t\tunsigned long stack_slot = user_rsp - user_stack_base;\n'
    '\t\t\t*((volatile unsigned long *)(stack_alias_base + stack_slot)) = 0UL; /* argv[argc] = NULL */\n'
    '\t\t}\n'
    '\t\tuser_rsp -= 8UL;\n'
    '\t\t{\n'
    '\t\t\tunsigned long stack_slot = user_rsp - user_stack_base;\n'
    '\t\t\t*((volatile unsigned long *)(stack_alias_base + stack_slot)) = 0UL; /* argv[0] = NULL */\n'
    '\t\t}\n'
    '\t\tuser_rsp -= 8UL;\n'
    '\t\t{\n'
    '\t\t\tunsigned long stack_slot = user_rsp - user_stack_base;\n'
    '\t\t\t*((volatile unsigned long *)(stack_alias_base + stack_slot)) = argc;\n'
    '\t\t}'
)

new = (
with open('/mnt/u/Projects/TG11-OS/kernel/terminal.c', 'rb') as f:
    raw = f.read()
crlf = b'\r\n' in raw
content = raw.decode('utf-8')
# Normalise to LF for matching, keep track for writing
content_lf = content.replace('\r\n', '\n')
    '\t\t *   argc, argv[0..argc-1], NULL, envp[0]=NULL,\n'
    '\t\t *   auxv (AT_PAGESZ=6, AT_ENTRY=9, AT_NULL=0), string data\n'
    '\t\t */\n'
    '\t\t{\n'
    '\t\t\tunsigned long j;\n'
    '\t\t\tunsigned long str_cursor = user_stack_top;\n'
    '\t\t\tunsigned long argv_user_ptrs[8]; /* max 1 + 7 */\n'
    '\t\t\t/* frame: argc(1) + argv ptrs(argc) + argv_null(1) +\n'
    '\t\t\t *        envp_null(1) + 3 auxv pairs(6) = argc + 9 qwords */\n'
    '\t\t\tunsigned long frame_qwords = argc + 9UL;\n'
    '\n'
    '\t\t\t/* Pack argv strings from the top of the stack downward */\n'
    '\t\t\tfor (j = argc; j-- > 0; )\n'
    '\t\t\t{\n'
    '\t\t\t\tconst char *s = (j == 0) ? path : exec_args[j - 1];\n'
    '\t\t\t\tunsigned long slen = string_length(s) + 1UL;\n'
    '\t\t\t\tunsigned long k;\n'
    '\t\t\t\tif (str_cursor < user_stack_base + slen)\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\tterminal_write_line("[exec] argv strings overflow stack");\n'
    '\t\t\t\t\ttask_user_heap_reset();\n'
    '\t\t\t\t\tfor (i = 0; i < USER_STACK_PAGES; i++)\n'
    '\t\t\t\t\t\tpaging_unmap_page(user_stack_base + (unsigned long)i * 4096UL);\n'
    '\t\t\t\t\tvirt_free_pages(stack_alias_base, USER_STACK_PAGES);\n'
    '\t\t\t\t\telf_unload(elf_shell_io_buf, size);\n'
    '\t\t\t\t\treturn;\n'
    '\t\t\t\t}\n'
    '\t\t\t\tstr_cursor -= slen;\n'
    '\t\t\t\targv_user_ptrs[j] = str_cursor;\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\tunsigned long off = str_cursor - user_stack_base;\n'
    '\t\t\t\t\tfor (k = 0; k < slen; k++)\n'
    '\t\t\t\t\t\t((unsigned char *)stack_alias_base)[off + k] = (unsigned char)s[k];\n'
    '\t\t\t\t}\n'
    '\t\t\t}\n'
    '\n'
    '\t\t\t/* 8-byte align, then adjust so RSP at argc is 16-byte aligned */\n'
    '\t\t\tstr_cursor &= ~7UL;\n'
    '\t\t\tif ((str_cursor & 15UL) != (frame_qwords * 8UL & 15UL))\n'
    '\t\t\t\tstr_cursor -= 8UL;\n'
    '\t\t\tuser_rsp = str_cursor;\n'
    '\n'
    '\t\t\tif (user_rsp < user_stack_base + frame_qwords * 8UL)\n'
    '\t\t\t{\n'
    '\t\t\t\tterminal_write_line("[exec] insufficient stack for startup frame");\n'
    '\t\t\t\ttask_user_heap_reset();\n'
    '\t\t\t\tfor (i = 0; i < USER_STACK_PAGES; i++)\n'
    '\t\t\t\t\tpaging_unmap_page(user_stack_base + (unsigned long)i * 4096UL);\n'
    '\t\t\t\tvirt_free_pages(stack_alias_base, USER_STACK_PAGES);\n'
    '\t\t\t\telf_unload(elf_shell_io_buf, size);\n'
    '\t\t\t\treturn;\n'
    '\t\t\t}\n'
    '\n'
    '#define EXEC_PUSH(v) do { \\\n'
    '\tuser_rsp -= 8UL; \\\n'
    '\t*((volatile unsigned long *)(stack_alias_base + (user_rsp - user_stack_base))) = (unsigned long)(v); \\\n'
    '} while (0)\n'
    '\t\t\t/* Auxv (nearest string data, highest VA in frame) */\n'
    '\t\t\tEXEC_PUSH(0UL);              /* AT_NULL val  */\n'
    '\t\t\tEXEC_PUSH(0UL);              /* AT_NULL type = 0 */\n'
    '\t\t\tEXEC_PUSH(prog.entry);       /* AT_ENTRY val */\n'
    '\t\t\tEXEC_PUSH(9UL);              /* AT_ENTRY type = 9 */\n'
    '\t\t\tEXEC_PUSH(4096UL);           /* AT_PAGESZ val */\n'
    '\t\t\tEXEC_PUSH(6UL);              /* AT_PAGESZ type = 6 */\n'
    '\t\t\t/* envp[0] = NULL */\n'
    '\t\t\tEXEC_PUSH(0UL);\n'
    '\t\t\t/* argv[argc] = NULL */\n'
    '\t\t\tEXEC_PUSH(0UL);\n'
    '\t\t\t/* argv[argc-1] .. argv[0] */\n'
    '\t\t\tfor (j = argc; j-- > 0; )\n'
    '\t\t\t\tEXEC_PUSH(argv_user_ptrs[j]);\n'
    '\t\t\t/* argc */\n'
    '\t\t\tEXEC_PUSH(argc);\n'
    '#undef EXEC_PUSH\n'
    '\t\t}'
)

if old in content:
    content = content.replace(old, new, 1)
    with open('/mnt/u/Projects/TG11-OS/kernel/terminal.c', 'w') as f:
        f.write(content)
    print('FIXED')
else:
    idx = content.find('argv string packing while we stabilize')
    if idx >= 0:
        print('Found at', idx)
        print(repr(content[idx-5:idx+30]))
    else:
        idx2 = content.find('minimal userspace stack frame')
        print('Minimal frame comment at', idx2)
        if idx2 >= 0:
            print(repr(content[idx2-3:idx2+60]))
        sys.exit(1)
