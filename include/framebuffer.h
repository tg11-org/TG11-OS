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
int framebuffer_try_set_mode(unsigned int width, unsigned int height, unsigned int bpp);
void framebuffer_try_auto_hires(void);
unsigned int framebuffer_boot_width(void);
unsigned int framebuffer_boot_height(void);
unsigned int framebuffer_boot_bpp(void);
int framebuffer_mode_source(void);

#endif
