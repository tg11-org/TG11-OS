/**
 * Copyright (C) 2026 TG11
 *
 * Intel 82540EM (E1000) Ethernet controller driver.
 */
#ifndef TG11_E1000_H
#define TG11_E1000_H

#define E1000_VENDOR_ID  0x8086
#define E1000_DEVICE_ID  0x100E  /* 82540EM-A (QEMU default) */

int  e1000_init(void);
int  e1000_send(const void *data, unsigned long length);
int  e1000_poll_rx(void *buf, unsigned long buf_size, unsigned long *out_len);
void e1000_get_mac(unsigned char mac[6]);
int  e1000_is_link_up(void);

/* Called from IRQ stub in interrupts.s */
void e1000_interrupt_handler(void);

#endif /* TG11_E1000_H */
