#ifndef TG11_USB_H
#define TG11_USB_H

void usb_init(void);

/* Poll USB HID mouse/tablet — call from GUI event loop */
void usb_hid_poll(void);

/* Get latest USB mouse/tablet state */
int usb_mouse_valid(void);      /* 1 if USB mouse/tablet found */
int usb_mouse_is_absolute(void);/* 1 if tablet (absolute), 0 if relative mouse */
int usb_mouse_abs_x(void);      /* absolute X (0–32767) for tablet */
int usb_mouse_abs_y(void);      /* absolute Y (0–32767) for tablet */
int usb_mouse_rel_dx(void);     /* relative delta X since last poll */
int usb_mouse_rel_dy(void);     /* relative delta Y since last poll */
int usb_mouse_buttons(void);    /* button bitmask (bit0=left, bit1=right, bit2=middle) */

#endif
