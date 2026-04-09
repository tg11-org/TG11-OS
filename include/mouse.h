#ifndef TG11_MOUSE_H
#define TG11_MOUSE_H

void mouse_init(void);
void mouse_handle_byte(unsigned char byte);
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_get_buttons(void);

#endif
