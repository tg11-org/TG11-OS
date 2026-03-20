#ifndef TG11_FRAMEBUFFER_H
#define TG11_FRAMEBUFFER_H

void framebuffer_init(unsigned long mb2_info_addr);
int  framebuffer_available(void);
unsigned long long framebuffer_addr(void);
unsigned int framebuffer_pitch(void);
unsigned int framebuffer_width(void);
unsigned int framebuffer_height(void);
unsigned int framebuffer_bpp(void);
unsigned int framebuffer_type(void);

#endif
