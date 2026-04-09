/**
 * Copyright (C) 2026 TG11
 *
 * PCI configuration space access and bus enumeration.
 */
#include "pci.h"
#include "arch.h"
#include "serial.h"

#define PCI_CONFIG_ADDR 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

struct pci_device pci_devices[PCI_MAX_DEVICES];
int               pci_device_count;

static const char hex[] = "0123456789ABCDEF";

static void serial_hex8(unsigned char v)
{
    char buf[3];
    buf[0] = hex[(v >> 4) & 0xF];
    buf[1] = hex[v & 0xF];
    buf[2] = '\0';
    serial_write(buf);
}

static void serial_hex16(unsigned short v)
{
    serial_hex8((unsigned char)(v >> 8));
    serial_hex8((unsigned char)(v & 0xFF));
}

static unsigned int pci_make_address(unsigned char bus, unsigned char slot,
                                     unsigned char func, unsigned char offset)
{
    return (1U << 31)
         | ((unsigned int)bus  << 16)
         | ((unsigned int)(slot & 0x1F) << 11)
         | ((unsigned int)(func & 0x07) << 8)
         | ((unsigned int)(offset & 0xFC));
}

unsigned int pci_config_read32(unsigned char bus, unsigned char slot,
                               unsigned char func, unsigned char offset)
{
    arch_outl(PCI_CONFIG_ADDR, pci_make_address(bus, slot, func, offset));
    return arch_inl(PCI_CONFIG_DATA);
}

void pci_config_write32(unsigned char bus, unsigned char slot,
                        unsigned char func, unsigned char offset,
                        unsigned int value)
{
    arch_outl(PCI_CONFIG_ADDR, pci_make_address(bus, slot, func, offset));
    arch_outl(PCI_CONFIG_DATA, value);
}

void pci_scan(void)
{
    unsigned int bus, slot, func;

    pci_device_count = 0;

    for (bus = 0; bus < 256; bus++)
    {
        for (slot = 0; slot < 32; slot++)
        {
            for (func = 0; func < 8; func++)
            {
                unsigned int id_reg, class_reg, irq_reg;
                unsigned short vendor, device;
                struct pci_device *d;
                int i;

                id_reg = pci_config_read32((unsigned char)bus,
                                           (unsigned char)slot,
                                           (unsigned char)func, 0x00);
                vendor = (unsigned short)(id_reg & 0xFFFF);
                device = (unsigned short)(id_reg >> 16);

                if (vendor == 0xFFFF) /* empty slot */
                {
                    if (func == 0) break; /* no device here, skip other funcs */
                    continue;
                }

                if (pci_device_count >= PCI_MAX_DEVICES) return;

                class_reg = pci_config_read32((unsigned char)bus,
                                              (unsigned char)slot,
                                              (unsigned char)func, 0x08);
                irq_reg   = pci_config_read32((unsigned char)bus,
                                              (unsigned char)slot,
                                              (unsigned char)func, 0x3C);

                d = &pci_devices[pci_device_count++];
                d->bus        = (unsigned char)bus;
                d->slot       = (unsigned char)slot;
                d->func       = (unsigned char)func;
                d->vendor_id  = vendor;
                d->device_id  = device;
                d->class_code = (unsigned char)(class_reg >> 24);
                d->subclass   = (unsigned char)((class_reg >> 16) & 0xFF);
                d->prog_if    = (unsigned char)((class_reg >> 8)  & 0xFF);
                d->irq_line   = (unsigned char)(irq_reg & 0xFF);

                for (i = 0; i < 6; i++)
                {
                    d->bar[i] = pci_config_read32((unsigned char)bus,
                                                  (unsigned char)slot,
                                                  (unsigned char)func,
                                                  (unsigned char)(0x10 + i * 4));
                }

                serial_write("[pci] ");
                serial_hex8((unsigned char)bus);
                serial_write(":");
                serial_hex8((unsigned char)slot);
                serial_write(".");
                serial_hex8((unsigned char)func);
                serial_write(" vendor=");
                serial_hex16(vendor);
                serial_write(" device=");
                serial_hex16(device);
                serial_write(" class=");
                serial_hex8(d->class_code);
                serial_write("/");
                serial_hex8(d->subclass);
                serial_write(" irq=");
                serial_hex8(d->irq_line);
                serial_write("\r\n");

                /* If func 0 is not multifunction, skip other funcs. */
                if (func == 0)
                {
                    unsigned int hdr = pci_config_read32((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0, 0x0C);
                    if (((hdr >> 16) & 0x80) == 0)
                        break;
                }
            }
        }
    }
}

int pci_find_device(unsigned short vendor, unsigned short device)
{
    int i;
    for (i = 0; i < pci_device_count; i++)
    {
        if (pci_devices[i].vendor_id == vendor &&
            pci_devices[i].device_id == device)
            return i;
    }
    return -1;
}
