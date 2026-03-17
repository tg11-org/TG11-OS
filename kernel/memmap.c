#include "memmap.h"
#include "terminal.h"

/* Multiboot2 tag types */
#define MB2_TAG_END      0
#define MB2_TAG_MEMMAP   6

/* Memory map entry types */
#define MB2_MEM_AVAILABLE        1
#define MB2_MEM_RESERVED         2
#define MB2_MEM_ACPI_RECLAIMABLE 3
#define MB2_MEM_NVS              4
#define MB2_MEM_BADRAM           5

#pragma pack(push, 1)

struct mb2_info_header
{
    unsigned int total_size;
    unsigned int reserved;
};

struct mb2_tag_header
{
    unsigned int type;
    unsigned int size;
};

struct mb2_memmap_tag
{
    unsigned int type;       /* = 6 */
    unsigned int size;
    unsigned int entry_size;
    unsigned int entry_version;
    /* entries follow */
};

struct mb2_memmap_entry
{
    unsigned long long base_addr;
    unsigned long long length;
    unsigned int       type;
    unsigned int       reserved;
};

#pragma pack(pop)

static struct mb2_memmap_tag *memmap_tag = (void *)0;

void memmap_init(unsigned long mb2_info_addr)
{
    struct mb2_tag_header *tag;
    unsigned long offset;

    if (mb2_info_addr == 0)
    {
        return;
    }

    offset = mb2_info_addr + 8; /* skip the 8-byte info header */

    while (1)
    {
        tag = (struct mb2_tag_header *)offset;

        if (tag->type == MB2_TAG_END)
        {
            break;
        }

        if (tag->type == MB2_TAG_MEMMAP)
        {
            memmap_tag = (struct mb2_memmap_tag *)tag;
        }

        /* Tags are padded to 8-byte alignment */
        offset += (tag->size + 7) & ~7u;
    }
}

static void print_hex64(unsigned long long v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++)
    {
        buf[17 - i] = digits[v & 0xF];
        v >>= 4;
    }
    buf[18] = '\0';
    terminal_write(buf);
}

static const char *mem_type_name(unsigned int type)
{
    switch (type)
    {
        case MB2_MEM_AVAILABLE:        return "Available";
        case MB2_MEM_RESERVED:         return "Reserved";
        case MB2_MEM_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case MB2_MEM_NVS:              return "ACPI NVS";
        case MB2_MEM_BADRAM:           return "Bad RAM";
        default:                       return "Unknown";
    }
}

void memmap_print(void)
{
    struct mb2_memmap_entry *entry;
    unsigned long offset;
    unsigned long end_offset;

    if (memmap_tag == (void *)0)
    {
        terminal_write_line("Memory map not available.");
        return;
    }

    terminal_write_line("Physical Memory Map:");
    terminal_write_line("  Base               Length             Type");
    terminal_write_line("  -----------------  -----------------  ----------------");

    offset     = (unsigned long)memmap_tag + sizeof(struct mb2_memmap_tag);
    end_offset = (unsigned long)memmap_tag + memmap_tag->size;

    while (offset + memmap_tag->entry_size <= end_offset)
    {
        entry = (struct mb2_memmap_entry *)offset;

        terminal_write("  ");
        print_hex64(entry->base_addr);
        terminal_write("  ");
        print_hex64(entry->length);
        terminal_write("  ");
        terminal_write_line(mem_type_name(entry->type));

        offset += memmap_tag->entry_size;
    }
}
