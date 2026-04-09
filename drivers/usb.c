/*
 * USB UHCI Host Controller driver + HID mouse/tablet support
 * Minimal implementation for QEMU/Proxmox USB tablet device.
 */
#include "usb.h"
#include "pci.h"
#include "arch.h"
#include "serial.h"
#include "memory.h"
#include "timer.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

static const char hex_chars[] = "0123456789ABCDEF";

static void usb_log(const char *msg)
{
	serial_write("[usb] ");
	serial_write(msg);
	serial_write("\r\n");
}

static void usb_log_hex8(const char *prefix, unsigned char v)
{
	char buf[3];
	serial_write("[usb] ");
	serial_write(prefix);
	buf[0] = hex_chars[(v >> 4) & 0xF];
	buf[1] = hex_chars[v & 0xF];
	buf[2] = '\0';
	serial_write(buf);
	serial_write("\r\n");
}

static void usb_log_hex16(const char *prefix, unsigned short v)
{
	char buf[5];
	serial_write("[usb] ");
	serial_write(prefix);
	buf[0] = hex_chars[(v >> 12) & 0xF];
	buf[1] = hex_chars[(v >> 8) & 0xF];
	buf[2] = hex_chars[(v >> 4) & 0xF];
	buf[3] = hex_chars[v & 0xF];
	buf[4] = '\0';
	serial_write(buf);
	serial_write("\r\n");
}

static void usb_log_hex32(const char *prefix, unsigned int v)
{
	char buf[9];
	int i;
	serial_write("[usb] ");
	serial_write(prefix);
	for (i = 7; i >= 0; i--)
		buf[7 - i] = hex_chars[(v >> (i * 4)) & 0xF];
	buf[8] = '\0';
	serial_write(buf);
	serial_write("\r\n");
}

static void delay_ms(unsigned long ms)
{
	/* Use port-I/O busy-wait (~1 us per arch_io_wait) so this works
	   even before interrupts are enabled. */
	unsigned long i, loops = ms * 1000;
	for (i = 0; i < loops; i++)
		arch_io_wait();
}

static void delay_us(unsigned long us)
{
	unsigned long i;
	for (i = 0; i < us; i++)
		arch_io_wait();
}

/* ── UHCI Register Offsets ───────────────────────────────────────── */

#define UHCI_USBCMD     0x00
#define UHCI_USBSTS     0x02
#define UHCI_USBINTR    0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASEADD  0x08
#define UHCI_PORTSC1    0x10
#define UHCI_PORTSC2    0x12

/* USBCMD bits */
#define UHCI_CMD_RUN      0x0001
#define UHCI_CMD_HCRESET  0x0002
#define UHCI_CMD_GRESET   0x0004
#define UHCI_CMD_MAXP     0x0080   /* max packet = 64 */

/* PORTSC bits */
#define UHCI_PORT_CONNECT   0x0001
#define UHCI_PORT_CONNECT_CHG 0x0002
#define UHCI_PORT_ENABLE    0x0004
#define UHCI_PORT_ENABLE_CHG 0x0008
#define UHCI_PORT_RESET     0x0200

/* ── UHCI Transfer Descriptor (TD) ──────────────────────────────── */
/* Each TD is 32 bytes (4 dwords + 4 reserved for SW use). Must be 16-byte aligned. */

struct uhci_td {
	unsigned int link;         /* Link pointer (phys addr | flags) */
	unsigned int ctrl_status;  /* Control & status */
	unsigned int token;        /* Token (PID, device, endpoint, toggle, maxlen) */
	unsigned int buffer;       /* Buffer pointer (phys addr) */
	/* Software fields (not read by hardware) */
	unsigned int sw_phys;      /* Physical address of this TD */
	unsigned int sw_buf_phys;  /* Physical address of the data buffer */
	unsigned int sw_pad[2];
} __attribute__((aligned(16)));

/* ── Queue Head (QH) ────────────────────────────────────────────── */

struct uhci_qh {
	unsigned int head_link;    /* Horizontal link (next QH) */
	unsigned int element;      /* First TD in this queue */
	unsigned int sw_phys;
	unsigned int sw_pad[5];
} __attribute__((aligned(16)));

/* USB PIDs */
#define USB_PID_SETUP  0x2D
#define USB_PID_IN     0x69
#define USB_PID_OUT    0xE1

/* ── USB Setup Packet ────────────────────────────────────────────── */

struct usb_setup_pkt {
	unsigned char  bmRequestType;
	unsigned char  bRequest;
	unsigned short wValue;
	unsigned short wIndex;
	unsigned short wLength;
} __attribute__((packed));

/* USB Device Descriptor (first 18 bytes) */
struct usb_dev_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned short bcdUSB;
	unsigned char  bDeviceClass;
	unsigned char  bDeviceSubClass;
	unsigned char  bDeviceProtocol;
	unsigned char  bMaxPacketSize0;
	unsigned short idVendor;
	unsigned short idProduct;
	unsigned short bcdDevice;
	unsigned char  iManufacturer;
	unsigned char  iProduct;
	unsigned char  iSerialNumber;
	unsigned char  bNumConfigurations;
} __attribute__((packed));

/* USB Configuration Descriptor header */
struct usb_config_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned short wTotalLength;
	unsigned char  bNumInterfaces;
	unsigned char  bConfigurationValue;
	unsigned char  iConfiguration;
	unsigned char  bmAttributes;
	unsigned char  bMaxPower;
} __attribute__((packed));

/* USB Interface Descriptor */
struct usb_iface_desc {
	unsigned char bLength;
	unsigned char bDescriptorType;
	unsigned char bInterfaceNumber;
	unsigned char bAlternateSetting;
	unsigned char bNumEndpoints;
	unsigned char bInterfaceClass;
	unsigned char bInterfaceSubClass;
	unsigned char bInterfaceProtocol;
	unsigned char iInterface;
} __attribute__((packed));

/* USB Endpoint Descriptor */
struct usb_ep_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned char  bEndpointAddress;
	unsigned char  bmAttributes;
	unsigned short wMaxPacketSize;
	unsigned char  bInterval;
} __attribute__((packed));

/* ── Static DMA Buffers ──────────────────────────────────────────── */

/* We allocate all DMA structures from physically contiguous pages */
static unsigned long  dma_phys_base = 0;  /* physical address */
static unsigned char *dma_virt_base = 0;  /* virtual address */
#define DMA_PAGES 4   /* 16 KB should be plenty */

/* Layout within DMA region:
 *  0x0000 .. 0x0FFF  Frame List (1024 * 4 = 4096 bytes)
 *  0x1000 .. 0x10FF  Queue Heads (8 * 32 = 256 bytes)
 *  0x1100 .. 0x17FF  Transfer Descriptors (up to 28 * 32 = 896 bytes)
 *  0x1800 .. 0x1FFF  Setup packets + data buffers (2048 bytes)
 *  0x2000 .. 0x3FFF  Interrupt TD + HID report buffer
 */
#define DMA_FRAMELIST_OFF  0x0000
#define DMA_QH_OFF         0x1000
#define DMA_TD_OFF         0x1100
#define DMA_SETUP_OFF      0x1800
#define DMA_DATA_OFF       0x1900
#define DMA_INT_TD_OFF     0x2000
#define DMA_INT_QH_OFF     0x2040
#define DMA_INT_BUF_OFF    0x2080

#define MAX_TDS 16

/* ── Controller State ────────────────────────────────────────────── */

static unsigned short uhci_iobase = 0;
static int            uhci_found  = 0;

/* HID mouse/tablet state */
static volatile int usb_hid_valid    = 0;
static volatile int usb_hid_absolute = 0; /* 1 = tablet */
static volatile int usb_hid_abs_x    = 0;
static volatile int usb_hid_abs_y    = 0;
static volatile int usb_hid_rel_dx   = 0;
static volatile int usb_hid_rel_dy   = 0;
static volatile int usb_hid_btn      = 0;

static int hid_dev_addr    = 0;  /* USB address of HID device */
static int hid_ep_num      = 0;  /* Interrupt IN endpoint number */
static int hid_ep_maxpkt   = 0;  /* Max packet size for that EP */
static int hid_data_toggle = 0;  /* DATA0/DATA1 toggle */
static int hid_interval    = 10; /* polling interval in ms */

/* ── UHCI I/O helpers ────────────────────────────────────────────── */

static unsigned short uhci_inw(int offset)
{
	return arch_inw(uhci_iobase + (unsigned short)offset);
}

static void uhci_outw(int offset, unsigned short val)
{
	arch_outw(uhci_iobase + (unsigned short)offset, val);
}

static unsigned int uhci_inl(int offset)
{
	return arch_inl(uhci_iobase + (unsigned short)offset);
}

static void uhci_outl(int offset, unsigned int val)
{
	arch_outl(uhci_iobase + (unsigned short)offset, val);
}

/* ── DMA helpers ─────────────────────────────────────────────────── */

static unsigned int dma_phys(int offset)
{
	return (unsigned int)(dma_phys_base + (unsigned long)offset);
}

static void *dma_virt(int offset)
{
	return (void *)(dma_virt_base + offset);
}

static void dma_zero(int offset, int size)
{
	unsigned char *p = (unsigned char *)dma_virt(offset);
	int i;
	for (i = 0; i < size; i++) p[i] = 0;
}

/* ── Build a TD ──────────────────────────────────────────────────── */

static void td_init(struct uhci_td *td, unsigned int link,
                     int pid, int addr, int ep, int toggle,
                     int maxlen, unsigned int buf_phys)
{
	td->link = link;
	/* Control/status: active=1, error_count=3, SPD=0 */
	td->ctrl_status = (1U << 23) | (3U << 27);  /* active + 3 error retries */
	/* Token: maxlen is (actual_length - 1), or 0x7FF for 0-length */
	{
		unsigned int mlen = (maxlen > 0) ? (unsigned int)(maxlen - 1) : 0x7FFU;
		td->token = (unsigned int)pid
		          | ((unsigned int)addr << 8)
		          | ((unsigned int)ep << 15)
		          | ((unsigned int)toggle << 19)
		          | (mlen << 21);
	}
	td->buffer = buf_phys;
}

/* Wait for a TD to complete (not active). Returns 0 on success, -1 timeout, -2 error */
static int td_wait(struct uhci_td *td, unsigned long timeout_ms)
{
	unsigned long deadline = timer_ticks() + (timeout_ms + 9) / 10;
	while (timer_ticks() < deadline)
	{
		unsigned int status = td->ctrl_status;
		if (!(status & (1U << 23)))  /* not active */
		{
			if (status & 0x007E0000U) /* any error bits (stall, data buffer, babble, NAK, CRC, bitstuff) */
				return -2;
			return 0;
		}
		delay_us(50);
	}
	return -1; /* timeout */
}

/* ── USB Control Transfer ────────────────────────────────────────── */

/*
 * Perform a USB control transfer (SETUP + optional DATA + STATUS).
 * Returns bytes transferred on success, or negative on error.
 */
static int usb_control_transfer(int addr, struct usb_setup_pkt *setup,
                                void *data, int data_len, int direction_in)
{
	struct uhci_td *td_setup, *td_data, *td_status;
	unsigned int td_phys_setup, td_phys_data, td_phys_status;
	unsigned int setup_buf_phys, data_buf_phys;
	unsigned int *framelist;
	int result;
	unsigned char *src, *dst;
	int i;

	/* Clean TD area */
	dma_zero(DMA_TD_OFF, 3 * 32);
	dma_zero(DMA_SETUP_OFF, 8);
	dma_zero(DMA_DATA_OFF, 256);

	td_setup  = (struct uhci_td *)dma_virt(DMA_TD_OFF);
	td_data   = (struct uhci_td *)dma_virt(DMA_TD_OFF + 32);
	td_status = (struct uhci_td *)dma_virt(DMA_TD_OFF + 64);

	td_phys_setup  = dma_phys(DMA_TD_OFF);
	td_phys_data   = dma_phys(DMA_TD_OFF + 32);
	td_phys_status = dma_phys(DMA_TD_OFF + 64);

	setup_buf_phys = dma_phys(DMA_SETUP_OFF);
	data_buf_phys  = dma_phys(DMA_DATA_OFF);

	/* Copy setup packet to DMA buffer */
	src = (unsigned char *)setup;
	dst = (unsigned char *)dma_virt(DMA_SETUP_OFF);
	for (i = 0; i < 8; i++) dst[i] = src[i];

	/* If direction_out and data_len > 0, copy data to DMA */
	if (!direction_in && data_len > 0 && data != (void *)0)
	{
		src = (unsigned char *)data;
		dst = (unsigned char *)dma_virt(DMA_DATA_OFF);
		for (i = 0; i < data_len && i < 256; i++) dst[i] = src[i];
	}

	if (data_len > 0)
	{
		/* SETUP -> DATA -> STATUS */
		int data_pid = direction_in ? USB_PID_IN : USB_PID_OUT;
		int status_pid = direction_in ? USB_PID_OUT : USB_PID_IN;

		td_init(td_setup, td_phys_data, USB_PID_SETUP, addr, 0, 0, 8, setup_buf_phys);
		td_init(td_data, td_phys_status, data_pid, addr, 0, 1, data_len, data_buf_phys);
		td_init(td_status, 0x01U /*terminate*/, status_pid, addr, 0, 1, 0, 0);
	}
	else
	{
		/* SETUP -> STATUS (no data phase) */
		td_init(td_setup, td_phys_status, USB_PID_SETUP, addr, 0, 0, 8, setup_buf_phys);
		td_init(td_status, 0x01U, USB_PID_IN, addr, 0, 1, 0, 0);
	}

	/* Stop the controller briefly */
	uhci_outw(UHCI_USBCMD, uhci_inw(UHCI_USBCMD) & (unsigned short)~UHCI_CMD_RUN);
	delay_us(100);

	/* Point frame 0 to the setup TD */
	framelist = (unsigned int *)dma_virt(DMA_FRAMELIST_OFF);
	framelist[0] = td_phys_setup; /* bit 0 clear = valid TD, bit 1 clear = TD not QH */

	/* Reset frame number and start */
	uhci_outw(UHCI_FRNUM, 0);
	uhci_outw(UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_MAXP);

	/* Wait for completion */
	result = td_wait(td_setup, 500);
	if (result < 0) {
		usb_log("SETUP TD failed");
		goto done;
	}

	if (data_len > 0)
	{
		result = td_wait(td_data, 500);
		if (result < 0) {
			usb_log("DATA TD failed");
			goto done;
		}
		/* Copy received data back if direction_in */
		if (direction_in && data != (void *)0)
		{
			int actual_len = (int)((td_data->ctrl_status + 1) & 0x7FF);
			if (actual_len > data_len) actual_len = data_len;
			src = (unsigned char *)dma_virt(DMA_DATA_OFF);
			dst = (unsigned char *)data;
			for (i = 0; i < actual_len; i++) dst[i] = src[i];
		}
	}

	result = td_wait(td_status, 500);
	if (result < 0) {
		usb_log("STATUS TD failed");
		goto done;
	}

	result = data_len; /* success: return requested length */

done:
	/* Stop controller, restore frame list entry to terminate */
	uhci_outw(UHCI_USBCMD, uhci_inw(UHCI_USBCMD) & (unsigned short)~UHCI_CMD_RUN);
	framelist[0] = 0x01U; /* terminate */
	return result;
}

/* ── USB Enumeration ─────────────────────────────────────────────── */

static int usb_set_address(int addr)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x00;
	setup.bRequest      = 5; /* SET_ADDRESS */
	setup.wValue        = (unsigned short)addr;
	setup.wIndex        = 0;
	setup.wLength       = 0;
	return usb_control_transfer(0, &setup, (void *)0, 0, 0);
}

static int usb_get_device_descriptor(int addr, struct usb_dev_desc *desc)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x80;
	setup.bRequest      = 6; /* GET_DESCRIPTOR */
	setup.wValue        = 0x0100; /* Device descriptor, index 0 */
	setup.wIndex        = 0;
	setup.wLength       = 18;
	return usb_control_transfer(addr, &setup, desc, 18, 1);
}

static int usb_get_config_descriptor(int addr, unsigned char *buf, int len)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x80;
	setup.bRequest      = 6; /* GET_DESCRIPTOR */
	setup.wValue        = 0x0200; /* Config descriptor, index 0 */
	setup.wIndex        = 0;
	setup.wLength       = (unsigned short)len;
	return usb_control_transfer(addr, &setup, buf, len, 1);
}

static int usb_set_configuration(int addr, int config)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x00;
	setup.bRequest      = 9; /* SET_CONFIGURATION */
	setup.wValue        = (unsigned short)config;
	setup.wIndex        = 0;
	setup.wLength       = 0;
	return usb_control_transfer(addr, &setup, (void *)0, 0, 0);
}

static int usb_set_idle(int addr, int iface)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x21; /* class, interface */
	setup.bRequest      = 0x0A; /* SET_IDLE */
	setup.wValue        = 0;    /* duration=0, report_id=0 → indefinite idle */
	setup.wIndex        = (unsigned short)iface;
	setup.wLength       = 0;
	return usb_control_transfer(addr, &setup, (void *)0, 0, 0);
}

static int usb_set_protocol(int addr, int iface, int protocol)
{
	struct usb_setup_pkt setup;
	setup.bmRequestType = 0x21; /* class, interface */
	setup.bRequest      = 0x0B; /* SET_PROTOCOL */
	setup.wValue        = (unsigned short)protocol; /* 0=boot, 1=report */
	setup.wIndex        = (unsigned short)iface;
	setup.wLength       = 0;
	return usb_control_transfer(addr, &setup, (void *)0, 0, 0);
}

/* ── Port Reset ──────────────────────────────────────────────────── */

static int uhci_port_reset(int port_offset)
{
	unsigned short sc;

	/* Assert reset */
	sc = uhci_inw(port_offset);
	uhci_outw(port_offset, sc | UHCI_PORT_RESET);
	delay_ms(60);

	/* De-assert reset */
	sc = uhci_inw(port_offset);
	uhci_outw(port_offset, sc & (unsigned short)~UHCI_PORT_RESET);
	delay_ms(20);

	/* Clear change bits, enable port */
	sc = uhci_inw(port_offset);
	uhci_outw(port_offset, sc | UHCI_PORT_CONNECT_CHG | UHCI_PORT_ENABLE_CHG | UHCI_PORT_ENABLE);
	delay_ms(20);

	sc = uhci_inw(port_offset);
	if (!(sc & UHCI_PORT_CONNECT))
		return -1; /* no device */
	if (!(sc & UHCI_PORT_ENABLE))
	{
		/* Retry enable */
		uhci_outw(port_offset, sc | UHCI_PORT_ENABLE);
		delay_ms(20);
		sc = uhci_inw(port_offset);
	}
	usb_log_hex16("port status after reset: ", sc);
	return (sc & UHCI_PORT_ENABLE) ? 0 : -1;
}

/* ── Parse Config Descriptor for HID endpoints ───────────────────── */

static int parse_config_for_hid(unsigned char *buf, int total_len,
                                int *out_ep, int *out_maxpkt, int *out_iface,
                                int *out_protocol)
{
	int pos = 0;
	int found_hid_iface = 0;
	int iface_num = 0;
	int iface_protocol = 0;

	while (pos + 2 <= total_len)
	{
		unsigned char bLen  = buf[pos];
		unsigned char bType = buf[pos + 1];
		if (bLen == 0) break;

		if (bType == 4 && bLen >= 9) /* Interface descriptor */
		{
			unsigned char bClass    = buf[pos + 5];
			unsigned char bSubClass = buf[pos + 6];
			unsigned char bProto    = buf[pos + 7];
			iface_num      = buf[pos + 2];
			iface_protocol = bProto;
			/* HID class = 3, subclass 1 = boot, protocol 2 = mouse */
			if (bClass == 3) /* HID class */
			{
				found_hid_iface = 1;
				(void)bSubClass; /* could check ==1 for boot interface */
			}
			else
			{
				found_hid_iface = 0;
			}
		}
		else if (bType == 5 && bLen >= 7 && found_hid_iface) /* Endpoint descriptor */
		{
			unsigned char bAddr = buf[pos + 2];
			unsigned char bAttr = buf[pos + 3];
			unsigned short wMaxPkt = (unsigned short)(buf[pos + 4] | ((unsigned short)buf[pos + 5] << 8));
			if ((bAttr & 0x03) == 3 && (bAddr & 0x80)) /* interrupt IN */
			{
				*out_ep      = bAddr & 0x0F;
				*out_maxpkt  = wMaxPkt;
				*out_iface   = iface_num;
				*out_protocol = iface_protocol;
				return 0; /* found it */
			}
		}
		pos += bLen;
	}
	return -1;
}

/* ── Interrupt IN Transfer (single polling read) ─────────────────── */

/*
 * Issue a single interrupt IN transfer on the given endpoint.
 * Returns bytes read (>=0) or negative on error/NAK.
 */
static int usb_interrupt_in(int addr, int ep, void *buf, int maxlen, int *toggle)
{
	struct uhci_td *td;
	unsigned int td_phys, buf_phys;
	unsigned int *framelist;
	int result, actual_len, i;
	unsigned char *src, *dst;

	dma_zero(DMA_INT_TD_OFF, 32);
	dma_zero(DMA_INT_BUF_OFF, 64);

	td = (struct uhci_td *)dma_virt(DMA_INT_TD_OFF);
	td_phys = dma_phys(DMA_INT_TD_OFF);
	buf_phys = dma_phys(DMA_INT_BUF_OFF);

	td_init(td, 0x01U /* terminate */, USB_PID_IN, addr, ep, *toggle, maxlen, buf_phys);

	/* Stop controller, set up frame 0, start */
	uhci_outw(UHCI_USBCMD, uhci_inw(UHCI_USBCMD) & (unsigned short)~UHCI_CMD_RUN);
	delay_us(50);

	framelist = (unsigned int *)dma_virt(DMA_FRAMELIST_OFF);
	framelist[0] = td_phys;

	uhci_outw(UHCI_FRNUM, 0);
	uhci_outw(UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_MAXP);

	result = td_wait(td, 50);

	uhci_outw(UHCI_USBCMD, uhci_inw(UHCI_USBCMD) & (unsigned short)~UHCI_CMD_RUN);
	framelist[0] = 0x01U;

	if (result < 0)
		return result;

	/* Toggle for next transfer */
	*toggle ^= 1;

	actual_len = (int)((td->ctrl_status + 1) & 0x7FF);
	if (actual_len > maxlen) actual_len = maxlen;

	src = (unsigned char *)dma_virt(DMA_INT_BUF_OFF);
	dst = (unsigned char *)buf;
	for (i = 0; i < actual_len; i++) dst[i] = src[i];

	return actual_len;
}

/* ── HID Report Parsing ──────────────────────────────────────────── */

/* QEMU USB tablet boot protocol report (6 bytes):
 *   byte 0:      buttons (bit0=left, bit1=right, bit2=middle)
 *   byte 1-2:    X absolute (little-endian, 0–32767)
 *   byte 3-4:    Y absolute (little-endian, 0–32767)
 *   byte 5:      wheel (signed)
 *
 * QEMU USB mouse boot protocol report (3-4 bytes):
 *   byte 0:      buttons
 *   byte 1:      X relative (signed byte)
 *   byte 2:      Y relative (signed byte)
 *   byte 3:      wheel (optional)
 */
static void parse_hid_report(unsigned char *buf, int len)
{
	if (len < 3) return;

	usb_hid_btn = buf[0] & 0x07;

	if (usb_hid_absolute && len >= 5)
	{
		usb_hid_abs_x = (int)(buf[1] | ((unsigned short)buf[2] << 8));
		usb_hid_abs_y = (int)(buf[3] | ((unsigned short)buf[4] << 8));
	}
	else
	{
		/* Relative mouse */
		signed char dx = (signed char)buf[1];
		signed char dy = (signed char)buf[2];
		usb_hid_rel_dx += dx;
		usb_hid_rel_dy += dy;
	}
}

/* ── Enumerate one port ──────────────────────────────────────────── */

static int enumerate_port(int port_offset, int usb_addr)
{
	struct usb_dev_desc dev_desc;
	unsigned char config_buf[128];
	int ep_num = 0, ep_maxpkt = 8, iface_num = 0, protocol = 0;
	int cfg_len;
	int i;

	if (uhci_port_reset(port_offset) < 0)
		return -1;

	delay_ms(30);

	/* Get device descriptor at address 0 (just first 8 bytes) */
	{
		unsigned char tmp8[8];
		struct usb_setup_pkt setup;
		setup.bmRequestType = 0x80;
		setup.bRequest      = 6;
		setup.wValue        = 0x0100;
		setup.wIndex        = 0;
		setup.wLength       = 8;
		if (usb_control_transfer(0, &setup, tmp8, 8, 1) < 0)
		{
			usb_log("failed to get 8-byte dev desc at addr 0");
			return -1;
		}
	}

	delay_ms(10);

	/* Set address */
	if (usb_set_address(usb_addr) < 0)
	{
		usb_log("SET_ADDRESS failed");
		return -1;
	}
	delay_ms(10);

	/* Get full device descriptor */
	for (i = 0; i < 128; i++) ((unsigned char *)&dev_desc)[i] = 0;
	if (usb_get_device_descriptor(usb_addr, &dev_desc) < 0)
	{
		usb_log("GET_DESCRIPTOR(device) failed");
		return -1;
	}

	usb_log_hex16("  vendor=", dev_desc.idVendor);
	usb_log_hex16("  product=", dev_desc.idProduct);
	usb_log_hex8("  class=", dev_desc.bDeviceClass);
	usb_log_hex8("  maxpkt0=", dev_desc.bMaxPacketSize0);

	/* Get config descriptor */
	for (i = 0; i < 128; i++) config_buf[i] = 0;
	if (usb_get_config_descriptor(usb_addr, config_buf, 9) < 0)
	{
		usb_log("GET_DESCRIPTOR(config header) failed");
		return -1;
	}
	cfg_len = (int)(config_buf[2] | ((unsigned short)config_buf[3] << 8));
	if (cfg_len > 128) cfg_len = 128;
	if (cfg_len > 9)
	{
		if (usb_get_config_descriptor(usb_addr, config_buf, cfg_len) < 0)
		{
			usb_log("GET_DESCRIPTOR(config full) failed");
			return -1;
		}
	}

	/* Parse for HID interface and interrupt endpoint */
	if (parse_config_for_hid(config_buf, cfg_len, &ep_num, &ep_maxpkt, &iface_num, &protocol) < 0)
	{
		usb_log("no HID interrupt IN endpoint found");
		return -1;
	}

	usb_log_hex8("  HID iface=", (unsigned char)iface_num);
	usb_log_hex8("  HID protocol=", (unsigned char)protocol);
	usb_log_hex8("  HID ep=", (unsigned char)ep_num);
	usb_log_hex8("  HID maxpkt=", (unsigned char)ep_maxpkt);

	/* Set configuration */
	{
		int config_val = config_buf[5]; /* bConfigurationValue */
		if (usb_set_configuration(usb_addr, config_val) < 0)
		{
			usb_log("SET_CONFIGURATION failed");
			return -1;
		}
	}

	/* Set boot protocol (protocol 0) for HID */
	usb_set_protocol(usb_addr, iface_num, 0);
	delay_ms(10);

	/* Set idle (suppress duplicate reports) */
	usb_set_idle(usb_addr, iface_num);
	delay_ms(10);

	/* Determine device type:
	 * protocol 1 = keyboard, 2 = mouse.
	 * QEMU tablet uses protocol 0 (non-boot) or sometimes 2.
	 * If vendor is 0x0627 (QEMU), it's a tablet.
	 * Otherwise fall back to relative mouse.
	 */
	if (dev_desc.idVendor == 0x0627 || protocol == 0)
		usb_hid_absolute = 1; /* QEMU tablet → absolute coordinates */
	else if (protocol == 2)
		usb_hid_absolute = 0; /* standard boot mouse → relative */
	else
		usb_hid_absolute = 0; /* default to relative */

	usb_log_hex8("  absolute=", (unsigned char)usb_hid_absolute);

	hid_dev_addr    = usb_addr;
	hid_ep_num      = ep_num;
	hid_ep_maxpkt   = ep_maxpkt;
	hid_data_toggle = 0;
	usb_hid_valid   = 1;

	usb_log("HID device configured OK!");
	return 0;
}

/* ── Initialization ──────────────────────────────────────────────── */

void usb_init(void)
{
	int i;
	unsigned int *framelist;
	unsigned int cmd;
	struct pci_device *dev = (void *)0;

	usb_log("scanning for USB controllers...");
	usb_log_hex8("pci_device_count=", (unsigned char)pci_device_count);

	/* Dump all USB host controllers (class 0x0C subclass 0x03) */
	for (i = 0; i < pci_device_count; i++)
	{
		if (pci_devices[i].class_code == 0x0C &&
		    pci_devices[i].subclass   == 0x03)
		{
			usb_log_hex16("  USB ctrl vendor=", pci_devices[i].vendor_id);
			usb_log_hex16("  USB ctrl device=", pci_devices[i].device_id);
			usb_log_hex8("  prog_if=", pci_devices[i].prog_if);
			/* prog_if: 0x00=UHCI, 0x10=OHCI, 0x20=EHCI, 0x30=xHCI */
			if (pci_devices[i].prog_if == 0x00)
				usb_log("  -> UHCI (USB 1.x)");
			else if (pci_devices[i].prog_if == 0x10)
				usb_log("  -> OHCI (USB 1.x)");
			else if (pci_devices[i].prog_if == 0x20)
				usb_log("  -> EHCI (USB 2.0)");
			else if (pci_devices[i].prog_if == 0x30)
				usb_log("  -> xHCI (USB 3.x)");

			/* Pick first UHCI */
			if (pci_devices[i].prog_if == 0x00 && !dev)
				dev = &pci_devices[i];
		}
	}

	if (!dev)
	{
		usb_log("no UHCI controller found (need prog_if=0x00)");
		usb_log("hint: if using QEMU, add: -device piix3-usb-uhci -device usb-tablet");
		return;
	}

	usb_log_hex16("found UHCI at vendor=", dev->vendor_id);
	usb_log_hex16("  device=", dev->device_id);

	/* UHCI I/O base is in BAR4 (offset 0x20), I/O space */
	uhci_iobase = (unsigned short)(dev->bar[4] & ~0x1FU);
	if (uhci_iobase == 0)
	{
		/* Some implementations use BAR0 */
		uhci_iobase = (unsigned short)(dev->bar[0] & ~0x1FU);
	}
	if (uhci_iobase == 0)
	{
		usb_log("UHCI I/O base is 0, aborting");
		return;
	}
	usb_log_hex16("  I/O base=", uhci_iobase);
	usb_log_hex8("  IRQ=", dev->irq_line);

	/* Enable bus mastering + I/O space */
	cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
	cmd |= (1U << 0) | (1U << 2); /* I/O Space + Bus Master */
	pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

	/* Allocate DMA pages */
	{
		unsigned long phys;
		unsigned char *virt;

		phys = phys_alloc_pages(DMA_PAGES);
		if (phys == 0) { usb_log("DMA alloc failed"); return; }
		virt = (unsigned char *)virt_reserve_pages(DMA_PAGES);
		if (!virt) { usb_log("virt reserve failed"); return; }

		for (i = 0; i < DMA_PAGES; i++)
		{
			paging_map_page((unsigned long)virt + (unsigned long)i * 4096,
			                phys + (unsigned long)i * 4096,
			                PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXECUTE | PAGE_FLAG_CACHE_DISABLE);
		}

		dma_phys_base = phys;
		dma_virt_base = virt;

		/* Zero entire DMA region */
		for (i = 0; i < DMA_PAGES * 4096; i++)
			virt[i] = 0;
	}

	/* Reset controller */
	uhci_outw(UHCI_USBCMD, UHCI_CMD_GRESET);
	delay_ms(50);
	uhci_outw(UHCI_USBCMD, 0);
	delay_ms(10);
	uhci_outw(UHCI_USBCMD, UHCI_CMD_HCRESET);
	delay_ms(20);
	uhci_outw(UHCI_USBCMD, 0);

	/* Initialize frame list — all entries terminate */
	framelist = (unsigned int *)dma_virt(DMA_FRAMELIST_OFF);
	for (i = 0; i < 1024; i++)
		framelist[i] = 0x01U; /* terminate */

	/* Set frame list base address */
	uhci_outl(UHCI_FLBASEADD, dma_phys(DMA_FRAMELIST_OFF));
	uhci_outw(UHCI_FRNUM, 0);

	/* Disable interrupts (we'll poll) */
	uhci_outw(UHCI_USBINTR, 0);

	/* Clear status */
	uhci_outw(UHCI_USBSTS, 0xFFFF);

	uhci_found = 1;
	usb_log("UHCI controller initialized");

	/* Enumerate devices on port 1 and port 2 */
	{
		unsigned short p1 = uhci_inw(UHCI_PORTSC1);
		unsigned short p2 = uhci_inw(UHCI_PORTSC2);
		usb_log_hex16("port1 status=", p1);
		usb_log_hex16("port2 status=", p2);

		if (p1 & UHCI_PORT_CONNECT)
		{
			usb_log("device on port 1, enumerating...");
			if (enumerate_port(UHCI_PORTSC1, 1) == 0)
			{
				usb_log("port 1: HID device ready");
			}
		}

		if (!usb_hid_valid && (p2 & UHCI_PORT_CONNECT))
		{
			usb_log("device on port 2, enumerating...");
			if (enumerate_port(UHCI_PORTSC2, 2) == 0)
			{
				usb_log("port 2: HID device ready");
			}
		}
	}

	if (!usb_hid_valid)
		usb_log("no HID mouse/tablet found on USB ports");
	else
		usb_log("USB HID mouse/tablet ready for polling");
}

/* ── Polling ─────────────────────────────────────────────────────── */

void usb_hid_poll(void)
{
	unsigned char report[8];
	int len;

	if (!uhci_found || !usb_hid_valid) return;

	len = usb_interrupt_in(hid_dev_addr, hid_ep_num, report, hid_ep_maxpkt, &hid_data_toggle);
	if (len > 0)
		parse_hid_report(report, len);
}

/* ── Public Getters ──────────────────────────────────────────────── */

int usb_mouse_valid(void)      { return usb_hid_valid; }
int usb_mouse_is_absolute(void){ return usb_hid_absolute; }
int usb_mouse_abs_x(void)      { return usb_hid_abs_x; }
int usb_mouse_abs_y(void)      { return usb_hid_abs_y; }
int usb_mouse_buttons(void)    { return usb_hid_btn; }

int usb_mouse_rel_dx(void)
{
	int v = usb_hid_rel_dx;
	usb_hid_rel_dx = 0;
	return v;
}

int usb_mouse_rel_dy(void)
{
	int v = usb_hid_rel_dy;
	usb_hid_rel_dy = 0;
	return v;
}
