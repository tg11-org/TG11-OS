#include "framebuffer.h"

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
