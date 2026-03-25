/**
 * Copyright (C) 2026 TG11
 *
 * Kernel symbol table lookup (binary search over generated flat table).
 *
 * The actual data (ksym_names, ksym_table, ksym_table_count) is provided by
 * build/kernel_syms.c which is auto-generated at build time by
 * tools/gen_kernel_syms.sh from the kernel ELF's nm output.
 */
#include "ksym.h"

/* Defined in build/kernel_syms.c (auto-generated) */
extern const char         *ksym_names;
extern const ksym_entry_t *ksym_table;
extern const unsigned long  ksym_table_count;

const char *ksym_lookup(unsigned long addr, unsigned long *out_offset)
{
    unsigned long lo, hi, mid;

    if (ksym_table_count == 0)
        return (void *)0;

    /* addr is below the first kernel symbol */
    if (addr < ksym_table[0].addr)
        return (void *)0;

    lo = 0;
    hi = ksym_table_count - 1;

    /*
     * Binary search for the last entry whose .addr <= addr.
     * Loop invariant: answer is in [lo, hi].
     * Use ceiling-division for mid to prevent infinite loop when hi = lo+1.
     */
    while (lo < hi) {
        mid = lo + (hi - lo + 1) / 2;
        if (ksym_table[mid].addr <= addr)
            lo = mid;
        else
            hi = mid - 1;
    }

    if (out_offset)
        *out_offset = addr - ksym_table[lo].addr;

    return ksym_names + ksym_table[lo].name_off;
}
