#ifndef TG11_SCREEN_H
#define TG11_SCREEN_H


#define SCREEN_STYLE_BOLD      0x01
#define SCREEN_STYLE_ITALIC    0x02
#define SCREEN_STYLE_UNDERLINE 0x04
#define SCREEN_STYLE_STRIKE    0x08
void screen_clear(void);
void screen_putchar(char c);
void screen_write(const char *str);
void screen_backspace(void);
void screen_set_color(unsigned char color);
unsigned char screen_get_color(void);
unsigned short screen_get_pos(void);
void screen_set_style(unsigned char style);
unsigned char screen_get_style(void);
void screen_set_pos(unsigned short offset);
void screen_set_hw_cursor(unsigned short offset);
void screen_write_char_at(unsigned short offset, char c);
void screen_read_char_at(unsigned short offset, char *c, unsigned char *color);
unsigned long screen_get_width(void);
void screen_write_cell_at(unsigned short offset, char c, unsigned char color, unsigned char style);
void screen_read_cell_at(unsigned short offset, char *c, unsigned char *color, unsigned char *style);
unsigned long screen_get_height(void);
void screen_set_text_mode_80x25(void);
void screen_set_text_mode_80x50(void);
int screen_set_framebuffer_text_mode(void);
int screen_get_cursor_style(void);
int screen_set_cursor_style(int style);
int screen_fbfont_get_style(void);
int screen_fbfont_set_style(int style);
unsigned int screen_fbfont_get_size(void);
int screen_fbfont_set_size(unsigned int size);
void screen_fbfont_reset_custom(void);
int screen_fbfont_set_custom_glyph(char ch, const unsigned char rows[7]);
int screen_fbfont_get_custom_glyph(char ch, unsigned char rows[7], int *is_custom);

/* Framebuffer pixel-drawing APIs for GUI */
int screen_fb_is_active(void);
unsigned int screen_fb_width(void);
unsigned int screen_fb_height(void);
void screen_fb_plot_pixel(unsigned int x, unsigned int y, unsigned int rgb);
unsigned int screen_fb_read_pixel(unsigned int x, unsigned int y);
void screen_fb_fill_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int rgb);
void screen_fb_draw_char(unsigned int px, unsigned int py, char ch, unsigned int fg, unsigned int bg);
void screen_fb_draw_string(unsigned int px, unsigned int py, const char *s, unsigned int fg, unsigned int bg);
unsigned int screen_fb_font_w(void);
unsigned int screen_fb_cell_h(void);

#endif
