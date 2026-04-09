/**
 * Copyright (C) 2026 TG11
 *
 * PCI configuration space access and bus enumeration.
 */
#ifndef TG11_PCI_H
#define TG11_PCI_H

#define PCI_MAX_DEVICES 32

struct pci_device
{
    unsigned char  bus;
    unsigned char  slot;
    unsigned char  func;
    unsigned short vendor_id;
    unsigned short device_id;
    unsigned char  class_code;
    unsigned char  subclass;
    unsigned char  prog_if;
    unsigned char  irq_line;
    unsigned int   bar[6];
};

extern struct pci_device pci_devices[PCI_MAX_DEVICES];
extern int               pci_device_count;

unsigned int pci_config_read32(unsigned char bus, unsigned char slot,
                               unsigned char func, unsigned char offset);
void         pci_config_write32(unsigned char bus, unsigned char slot,
                                unsigned char func, unsigned char offset,
                                unsigned int value);
void         pci_scan(void);
int          pci_find_device(unsigned short vendor, unsigned short device);

#endif /* TG11_PCI_H */
