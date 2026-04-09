/**
 * Copyright (C) 2026 TG11
 *
 * Intel 82540EM (E1000) Ethernet driver for QEMU.
 *
 * MMIO-based register access.  TX/RX descriptor rings use physical
 * addresses for DMA; each ring has 32 entries of 16 bytes each.
 */
#include "e1000.h"
#include "pci.h"
#include "arch.h"
#include "memory.h"
#include "serial.h"

/* ── E1000 register offsets ────────────────────────────────────── */
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EERD      0x0014
#define E1000_ICR       0x00C0
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404
#define E1000_MTA       0x5200

/* ── CTRL bits ─────────────────────────────────────────────────── */
#define CTRL_SLU        (1U << 6)   /* Set Link Up */
#define CTRL_RST        (1U << 26)  /* Device Reset */

/* ── STATUS bits ───────────────────────────────────────────────── */
#define STATUS_LU       (1U << 1)   /* Link Up */

/* ── RCTL bits ─────────────────────────────────────────────────── */
#define RCTL_EN         (1U << 1)
#define RCTL_SBP        (1U << 2)
#define RCTL_UPE        (1U << 3)
#define RCTL_MPE        (1U << 4)
#define RCTL_BAM        (1U << 15)
#define RCTL_BSIZE_2048 (0U << 16)
#define RCTL_SECRC      (1U << 26)

/* ── TCTL bits ─────────────────────────────────────────────────── */
#define TCTL_EN         (1U << 1)
#define TCTL_PSP        (1U << 3)

/* ── Interrupt mask bits ───────────────────────────────────────── */
#define ICR_TXDW        (1U << 0)
#define ICR_TXQE        (1U << 1)
#define ICR_LSC         (1U << 2)
#define ICR_RXDMT0      (1U << 4)
#define ICR_RXO         (1U << 6)
#define ICR_RXT0        (1U << 7)

/* ── Descriptor ring sizes ─────────────────────────────────────── */
#define NUM_RX_DESC     32
#define NUM_TX_DESC     32
#define RX_BUF_SIZE     2048

/* ── RX descriptor (legacy) ────────────────────────────────────── */
struct e1000_rx_desc
{
    unsigned long  addr;
    unsigned short length;
    unsigned short checksum;
    unsigned char  status;
    unsigned char  errors;
    unsigned short special;
} __attribute__((packed));

#define RXD_STAT_DD     0x01
#define RXD_STAT_EOP    0x02

/* ── TX descriptor (legacy) ────────────────────────────────────── */
struct e1000_tx_desc
{
    unsigned long  addr;
    unsigned short length;
    unsigned char  cso;
    unsigned char  cmd;
    unsigned char  status;
    unsigned char  css;
    unsigned short special;
} __attribute__((packed));

#define TXD_CMD_EOP     0x01
#define TXD_CMD_IFCS    0x02
#define TXD_CMD_RS      0x08
#define TXD_STAT_DD     0x01

/* ── Driver state ──────────────────────────────────────────────── */
static volatile unsigned char *mmio_base;
static unsigned long           mmio_phys_base;
static unsigned char           mac_addr[6];

/* Descriptor rings — must be 16-byte aligned, contiguous physical pages */
static struct e1000_rx_desc *rx_descs;
static struct e1000_tx_desc *tx_descs;
static unsigned long         rx_descs_phys;
static unsigned long         tx_descs_phys;

/* RX packet buffers — one 2048-byte buffer per descriptor */
static unsigned long rx_buf_phys[NUM_RX_DESC];
static unsigned char *rx_buf_virt[NUM_RX_DESC];

/* TX packet buffers */
static unsigned long tx_buf_phys[NUM_TX_DESC];
static unsigned char *tx_buf_virt[NUM_TX_DESC];

static unsigned int rx_cur;
static unsigned int tx_cur;

static volatile int rx_pending; /* set in IRQ, cleared after poll */

static int e1000_up;

/* ── MMIO read/write ───────────────────────────────────────────── */

static unsigned int e1000_read(unsigned int reg)
{
    return *(volatile unsigned int *)(mmio_base + reg);
}

static void e1000_write(unsigned int reg, unsigned int val)
{
    *(volatile unsigned int *)(mmio_base + reg) = val;
}

/* ── Hex print helper for serial diagnostics ───────────────────── */

static void serial_write_hex(unsigned long val)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--)
    {
        buf[i] = hx[val & 0xF];
        val >>= 4;
    }
    buf[16] = '\0';
    serial_write("0x");
    serial_write(buf);
}

/* ── Map MMIO region ───────────────────────────────────────────── */

static int map_mmio(unsigned long phys, unsigned long num_pages)
{
    unsigned long virt;
    unsigned long i;
    unsigned long flags;

    serial_write("[e1000] map_mmio: phys=");
    serial_write_hex(phys);
    serial_write(" pages=");
    {
        char nb[8];
        int d = 0;
        unsigned long v = num_pages;
        if (v == 0) { nb[d++] = '0'; }
        else { char t[8]; int td = 0; while (v) { t[td++] = '0' + (char)(v % 10); v /= 10; } while (td--) nb[d++] = t[td]; }
        nb[d] = '\0';
        serial_write(nb);
    }
    serial_write("\r\n");

    virt = (unsigned long)virt_reserve_pages(num_pages);
    if (virt == 0)
    {
        serial_write("[e1000] map_mmio: virt_reserve_pages FAILED\r\n");
        return -1;
    }
    serial_write("[e1000] map_mmio: virt=");
    serial_write_hex(virt);
    serial_write("\r\n");

    flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE
          | PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_WRITE_THROUGH
          | PAGE_FLAG_NO_EXECUTE;

    for (i = 0; i < num_pages; i++)
    {
        int rc = paging_map_page(virt + i * MEMORY_PAGE_SIZE,
                            phys + i * MEMORY_PAGE_SIZE, flags);
        if (rc != 0)
        {
            serial_write("[e1000] map_mmio: paging_map_page FAILED at page ");
            {
                char nb[8];
                int d = 0;
                unsigned long v = i;
                if (v == 0) { nb[d++] = '0'; }
                else { char t[8]; int td = 0; while (v) { t[td++] = '0' + (char)(v % 10); v /= 10; } while (td--) nb[d++] = t[td]; }
                nb[d] = '\0';
                serial_write(nb);
            }
            serial_write(" virt=");
            serial_write_hex(virt + i * MEMORY_PAGE_SIZE);
            serial_write(" phys=");
            serial_write_hex(phys + i * MEMORY_PAGE_SIZE);
            serial_write(" rc=");
            {
                char nb[8];
                int d = 0;
                int neg = rc < 0;
                unsigned long v = (unsigned long)(neg ? -rc : rc);
                if (v == 0) { nb[d++] = '0'; }
                else { char t[8]; int td = 0; while (v) { t[td++] = '0' + (char)(v % 10); v /= 10; } while (td--) nb[d++] = t[td]; }
                nb[d] = '\0';
                if (neg) serial_write("-");
                serial_write(nb);
            }
            serial_write("\r\n");
            return -1;
        }
    }

    mmio_base      = (volatile unsigned char *)virt;
    mmio_phys_base = phys;
    serial_write("[e1000] map_mmio: OK\r\n");
    return 0;
}

/* ── Read MAC address from EEPROM ──────────────────────────────── */

static unsigned short eeprom_read(unsigned char addr)
{
    unsigned int val;
    e1000_write(E1000_EERD, (1U) | ((unsigned int)addr << 8));
    while (1)
    {
        val = e1000_read(E1000_EERD);
        if (val & (1U << 4)) break;
    }
    return (unsigned short)(val >> 16);
}

static void read_mac(void)
{
    unsigned short w;
    w = eeprom_read(0);
    mac_addr[0] = (unsigned char)(w & 0xFF);
    mac_addr[1] = (unsigned char)(w >> 8);
    w = eeprom_read(1);
    mac_addr[2] = (unsigned char)(w & 0xFF);
    mac_addr[3] = (unsigned char)(w >> 8);
    w = eeprom_read(2);
    mac_addr[4] = (unsigned char)(w & 0xFF);
    mac_addr[5] = (unsigned char)(w >> 8);
}

/* ── Allocate descriptor rings and buffers ─────────────────────── */

static int alloc_rings(void)
{
    unsigned long phys;
    unsigned char *virt;
    int i;

    /* RX descriptors — 32 * 16 = 512 bytes, fits in one page */
    phys = phys_alloc_page();
    if (!phys) return -1;
    memory_zero_phys_page(phys);
    virt = (unsigned char *)virt_reserve_pages(1);
    if (!virt) return -1;
    if (paging_map_page((unsigned long)virt, phys,
                        PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXECUTE) != 0)
        return -1;
    rx_descs      = (struct e1000_rx_desc *)virt;
    rx_descs_phys = phys;

    /* TX descriptors */
    phys = phys_alloc_page();
    if (!phys) return -1;
    memory_zero_phys_page(phys);
    virt = (unsigned char *)virt_reserve_pages(1);
    if (!virt) return -1;
    if (paging_map_page((unsigned long)virt, phys,
                        PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXECUTE) != 0)
        return -1;
    tx_descs      = (struct e1000_tx_desc *)virt;
    tx_descs_phys = phys;

    /* RX buffers — one page per descriptor (2048 fit in 4096) */
    for (i = 0; i < NUM_RX_DESC; i++)
    {
        phys = phys_alloc_page();
        if (!phys) return -1;
        memory_zero_phys_page(phys);
        virt = (unsigned char *)virt_reserve_pages(1);
        if (!virt) return -1;
        if (paging_map_page((unsigned long)virt, phys,
                            PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXECUTE) != 0)
            return -1;
        rx_buf_phys[i] = phys;
        rx_buf_virt[i] = virt;
        rx_descs[i].addr   = phys;
        rx_descs[i].status = 0;
    }

    /* TX buffers */
    for (i = 0; i < NUM_TX_DESC; i++)
    {
        phys = phys_alloc_page();
        if (!phys) return -1;
        memory_zero_phys_page(phys);
        virt = (unsigned char *)virt_reserve_pages(1);
        if (!virt) return -1;
        if (paging_map_page((unsigned long)virt, phys,
                            PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXECUTE) != 0)
            return -1;
        tx_buf_phys[i] = phys;
        tx_buf_virt[i] = virt;
    }

    return 0;
}

/* ── Initialise the E1000 ──────────────────────────────────────── */

int e1000_init(void)
{
    int pci_idx;
    struct pci_device *dev;
    unsigned long bar0;
    unsigned int cmd, i;

    pci_idx = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (pci_idx < 0)
    {
        serial_write("[e1000] device not found\r\n");
        return -1;
    }
    dev = &pci_devices[pci_idx];

    /* BAR0 is memory-mapped (bit 0 == 0) */
    bar0 = dev->bar[0] & ~0xFUL;
    if (bar0 == 0)
    {
        serial_write("[e1000] BAR0 is zero\r\n");
        return -1;
    }

    /* Enable PCI bus mastering + memory access */
    cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1U << 1) | (1U << 2); /* Memory Space + Bus Master */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

    serial_write("[e1000] BAR0=");
    serial_write_hex(bar0);
    serial_write(" raw=");
    serial_write_hex((unsigned long)dev->bar[0]);
    serial_write("\r\n");

    /* Map MMIO — E1000 uses ~128 KB of register space */
    if (map_mmio(bar0, 32) != 0)  /* 32 pages = 128 KB */
    {
        serial_write("[e1000] MMIO map failed\r\n");
        return -1;
    }

    /* Reset the device */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_RST);
    /* Small busy-wait for reset to complete */
    for (i = 0; i < 100000; i++)
        arch_io_wait();

    /* Disable interrupts during setup */
    e1000_write(E1000_IMC, 0xFFFFFFFFU);
    (void)e1000_read(E1000_ICR); /* clear pending */

    /* Read MAC address */
    read_mac();
    serial_write("[e1000] MAC: ");
    {
        static const char hx[] = "0123456789ABCDEF";
        char buf[18];
        int j;
        for (j = 0; j < 6; j++)
        {
            buf[j*3]   = hx[mac_addr[j] >> 4];
            buf[j*3+1] = hx[mac_addr[j] & 0xF];
            buf[j*3+2] = (j < 5) ? ':' : '\0';
        }
        buf[17] = '\0';
        serial_write(buf);
    }
    serial_write("\r\n");

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_SLU);

    /* Allocate DMA rings and buffers */
    if (alloc_rings() != 0)
    {
        serial_write("[e1000] ring alloc failed\r\n");
        return -1;
    }

    /* ── Configure RX ─────────────────────────────────────────── */
    e1000_write(E1000_RDBAL, (unsigned int)(rx_descs_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (unsigned int)(rx_descs_phys >> 32));
    e1000_write(E1000_RDLEN, (unsigned int)(NUM_RX_DESC * sizeof(struct e1000_rx_desc)));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, NUM_RX_DESC - 1);

    /* Program receive address (RAL/RAH) from MAC */
    {
        unsigned int ral = (unsigned int)mac_addr[0]
                         | ((unsigned int)mac_addr[1] << 8)
                         | ((unsigned int)mac_addr[2] << 16)
                         | ((unsigned int)mac_addr[3] << 24);
        unsigned int rah = (unsigned int)mac_addr[4]
                         | ((unsigned int)mac_addr[5] << 8)
                         | (1U << 31); /* Address Valid */
        e1000_write(E1000_RAL0, ral);
        e1000_write(E1000_RAH0, rah);
    }

    /* Clear multicast table */
    for (i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* Enable receiver: unicast, broadcast, strip CRC */
    e1000_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);

    /* ── Configure TX ─────────────────────────────────────────── */
    e1000_write(E1000_TDBAL, (unsigned int)(tx_descs_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (unsigned int)(tx_descs_phys >> 32));
    e1000_write(E1000_TDLEN, (unsigned int)(NUM_TX_DESC * sizeof(struct e1000_tx_desc)));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    /* Inter-Packet Gap — standard values for IEEE 802.3 */
    e1000_write(E1000_TIPG, 10 | (10 << 10) | (10 << 20));

    /* Enable transmitter */
    e1000_write(E1000_TCTL, TCTL_EN | TCTL_PSP
                            | (15U << 4)     /* CT = 15 */
                            | (64U << 12));   /* COLD = 64 */

    /* Enable RX interrupts */
    e1000_write(E1000_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);

    rx_cur = 0;
    tx_cur = 0;
    rx_pending = 0;
    e1000_up = 1;

    serial_write("[e1000] initialised OK\r\n");
    return 0;
}

/* ── Send a raw Ethernet frame ─────────────────────────────────── */

int e1000_send(const void *data, unsigned long length)
{
    struct e1000_tx_desc *desc;
    const unsigned char *src = (const unsigned char *)data;
    unsigned long i;

    if (!e1000_up || length > 1518 || length == 0) return -1;

    desc = &tx_descs[tx_cur];

    /* Wait for previous TX at this slot to complete */
    while (!(desc->status & TXD_STAT_DD) && desc->cmd != 0)
        arch_io_wait();

    /* Copy frame into TX buffer */
    for (i = 0; i < length; i++)
        tx_buf_virt[tx_cur][i] = src[i];

    desc->addr   = tx_buf_phys[tx_cur];
    desc->length = (unsigned short)length;
    desc->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;
    desc->cso    = 0;
    desc->css    = 0;
    desc->special = 0;

    tx_cur = (tx_cur + 1) % NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_cur);

    return 0;
}

/* ── Poll for a received packet ────────────────────────────────── */

int e1000_poll_rx(void *buf, unsigned long buf_size, unsigned long *out_len)
{
    struct e1000_rx_desc *desc;
    unsigned long pkt_len, copy_len, i;
    unsigned char *dst = (unsigned char *)buf;

    if (!e1000_up) return -1;

    desc = &rx_descs[rx_cur];
    if (!(desc->status & RXD_STAT_DD)) return 0; /* no packet */

    pkt_len  = desc->length;
    copy_len = pkt_len < buf_size ? pkt_len : buf_size;

    for (i = 0; i < copy_len; i++)
        dst[i] = rx_buf_virt[rx_cur][i];

    if (out_len) *out_len = copy_len;

    /* Reset descriptor and advance tail */
    desc->status = 0;
    {
        unsigned int old_rx = rx_cur;
        rx_cur = (rx_cur + 1) % NUM_RX_DESC;
        e1000_write(E1000_RDT, old_rx);
    }

    return 1; /* packet received */
}

/* ── MAC address accessor ──────────────────────────────────────── */

void e1000_get_mac(unsigned char out[6])
{
    int i;
    for (i = 0; i < 6; i++) out[i] = mac_addr[i];
}

int e1000_is_link_up(void)
{
    if (!e1000_up) return 0;
    return (e1000_read(E1000_STATUS) & STATUS_LU) ? 1 : 0;
}

/* ── Interrupt handler (called from irq11_stub) ────────────────── */

void e1000_interrupt_handler(void)
{
    unsigned int icr;

    if (!e1000_up)
    {
        /* Spurious — send EOI and exit */
        arch_outb(0xA0, 0x20);
        arch_outb(0x20, 0x20);
        return;
    }

    icr = e1000_read(E1000_ICR); /* read clears interrupt cause */

    if (icr & (ICR_RXT0 | ICR_RXO | ICR_RXDMT0))
        rx_pending = 1;

    /* Send EOI to both PICs (IRQ 11 is on slave PIC) */
    arch_outb(0xA0, 0x20);
    arch_outb(0x20, 0x20);
}
