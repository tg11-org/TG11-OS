#ifndef TG11_SERIAL_H
#define TG11_SERIAL_H

int serial_init(void);
void serial_putchar(char c);
void serial_write(const char *str);
int serial_can_read(void);
int serial_try_read(char *out_char);

#endif
