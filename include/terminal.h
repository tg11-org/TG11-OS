#ifndef TG11_TERMINAL_H
#define TG11_TERMINAL_H

void terminal_init(unsigned long mb2_info_addr);
void terminal_poll(void);
void terminal_enqueue_scancode(unsigned char scancode);
void terminal_write(const char *str);
void terminal_write_line(const char *str);
void terminal_write_hex64(unsigned long v);
void terminal_write_hex8(unsigned char v);
int terminal_read_line(char *out, unsigned long out_size);
int terminal_input_available(void);

#endif
