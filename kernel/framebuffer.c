#include "framebuffer.h"
#include "arch.h"

#define MB2_TAG_END         0
#define MB2_TAG_FRAMEBUFFER 8

#pragma pack(push, 1)
struct mb2_tag_header
{
	unsigned int type;
	unsigned int size;
};

struct mb2_tag_framebuffer
{
	unsigned int type;
	unsigned int size;
	unsigned long long framebuffer_addr;
	unsigned int framebuffer_pitch;
	unsigned int framebuffer_width;
	unsigned int framebuffer_height;
	unsigned char framebuffer_bpp;
	unsigned char framebuffer_type;
	unsigned short reserved;
};
#pragma pack(pop)

static int fb_available = 0;
static unsigned long long fb_addr = 0;
static unsigned int fb_pitch = 0;
static unsigned int fb_w = 0;
static unsigned int fb_h = 0;
static unsigned int fb_bpp = 0;
static unsigned int fb_type = 0;
static unsigned int fb_boot_w = 0;
static unsigned int fb_boot_h = 0;
static unsigned int fb_boot_bpp = 0;
static int fb_mode_source = 0; /* 0=bootloader, 1=kernel-vbe */

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID     0x0
#define VBE_DISPI_INDEX_XRES   0x1
#define VBE_DISPI_INDEX_YRES   0x2
#define VBE_DISPI_INDEX_BPP    0x3
#define VBE_DISPI_INDEX_ENABLE 0x4

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40

#define VBE_DISPI_ID0          0xB0C0
#define VBE_DISPI_ID5          0xB0C5

static unsigned short vbe_dispi_read(unsigned short index)
{
	arch_outw(VBE_DISPI_IOPORT_INDEX, index);
	return arch_inw(VBE_DISPI_IOPORT_DATA);
}

static void vbe_dispi_write(unsigned short index, unsigned short value)
{
	arch_outw(VBE_DISPI_IOPORT_INDEX, index);
	arch_outw(VBE_DISPI_IOPORT_DATA, value);
}

static int vbe_dispi_present(void)
{
	unsigned short id = vbe_dispi_read(VBE_DISPI_INDEX_ID);
	return (id >= VBE_DISPI_ID0 && id <= VBE_DISPI_ID5) ? 1 : 0;
}

void framebuffer_init(unsigned long mb2_info_addr)
{
	unsigned long offset;
	struct mb2_tag_header *tag;

	fb_available = 0;
	fb_addr = 0;
	fb_pitch = 0;
	fb_w = 0;
	fb_h = 0;
	fb_bpp = 0;
	fb_type = 0;
	fb_boot_w = 0;
	fb_boot_h = 0;
	fb_boot_bpp = 0;
	fb_mode_source = 0;

	if (mb2_info_addr == 0) return;

	offset = mb2_info_addr + 8;
	for (;;)
	{
		tag = (struct mb2_tag_header *)offset;
		if (tag->type == MB2_TAG_END) break;
		if (tag->type == MB2_TAG_FRAMEBUFFER)
		{
			struct mb2_tag_framebuffer *fb = (struct mb2_tag_framebuffer *)tag;
			fb_available = 1;
			fb_addr = fb->framebuffer_addr;
			fb_pitch = fb->framebuffer_pitch;
			fb_w = fb->framebuffer_width;
			fb_h = fb->framebuffer_height;
			fb_bpp = (unsigned int)fb->framebuffer_bpp;
			fb_type = (unsigned int)fb->framebuffer_type;
			fb_boot_w = fb_w;
			fb_boot_h = fb_h;
			fb_boot_bpp = fb_bpp;
			fb_mode_source = 0;
		}
		offset += (tag->size + 7) & ~7u;
	}
}

int framebuffer_available(void)
{
	return fb_available;
}

unsigned long long framebuffer_addr(void)
{
	return fb_addr;
}

unsigned int framebuffer_pitch(void)
{
	return fb_pitch;
}

unsigned int framebuffer_width(void)
{
	return fb_w;
}

unsigned int framebuffer_height(void)
{
	return fb_h;
}

unsigned int framebuffer_bpp(void)
{
	return fb_bpp;
}

unsigned int framebuffer_type(void)
{
	return fb_type;
}

int framebuffer_try_set_mode(unsigned int width, unsigned int height, unsigned int bpp)
{
	unsigned short rw, rh, rb;
	if (!fb_available || fb_type != 1) return 0;
	if (!(bpp == 24 || bpp == 32)) return 0;
	if (!vbe_dispi_present()) return 0;

	vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
	vbe_dispi_write(VBE_DISPI_INDEX_XRES, (unsigned short)width);
	vbe_dispi_write(VBE_DISPI_INDEX_YRES, (unsigned short)height);
	vbe_dispi_write(VBE_DISPI_INDEX_BPP, (unsigned short)bpp);
	vbe_dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

	rw = vbe_dispi_read(VBE_DISPI_INDEX_XRES);
	rh = vbe_dispi_read(VBE_DISPI_INDEX_YRES);
	rb = vbe_dispi_read(VBE_DISPI_INDEX_BPP);
	if ((unsigned int)rw != width || (unsigned int)rh != height || (unsigned int)rb != bpp) return 0;

	fb_w = width;
	fb_h = height;
	fb_bpp = bpp;
	fb_pitch = width * (bpp / 8u);
	fb_mode_source = 1;
	return 1;
}

void framebuffer_try_auto_hires(void)
{
	/* Only try to upscale if we booted in a low VBE mode. */
	if (!fb_available || fb_type != 1) return;
	if (fb_w >= 1600 && fb_h >= 900) return;
	if (fb_bpp != 24 && fb_bpp != 32) return;

	if (framebuffer_try_set_mode(1920, 1080, 32)) return;
	if (framebuffer_try_set_mode(1600, 900, 32)) return;
	if (framebuffer_try_set_mode(1360, 768, 32)) return;
	if (framebuffer_try_set_mode(1280, 720, 32)) return;
}

unsigned int framebuffer_boot_width(void)
{
	return fb_boot_w;
}

unsigned int framebuffer_boot_height(void)
{
	return fb_boot_h;
}

unsigned int framebuffer_boot_bpp(void)
{
	return fb_boot_bpp;
}

int framebuffer_mode_source(void)
{
	return fb_mode_source;
}
