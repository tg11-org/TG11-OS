#ifndef TG11_SCREEN_H
#define TG11_SCREEN_H

void screen_clear(void);
void screen_putchar(char c);
void screen_write(const char *str);
void screen_backspace(void);
void screen_set_color(unsigned char color);
unsigned short screen_get_pos(void);
void screen_set_pos(unsigned short offset);
void screen_set_hw_cursor(unsigned short offset);
void screen_write_char_at(unsigned short offset, char c);

#endif
