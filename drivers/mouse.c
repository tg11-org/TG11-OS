#include "mouse.h"
#include "arch.h"

static unsigned char mouse_packet[3];
static int mouse_packet_index = 0;
static volatile int mouse_x = 0;
static volatile int mouse_y = 0;
static volatile int mouse_buttons = 0;

int mouse_get_x(void)       { return mouse_x; }
int mouse_get_y(void)       { return mouse_y; }
int mouse_get_buttons(void) { return mouse_buttons; }

static void mouse_wait_write(void)
{
    unsigned int timeout = 100000;
    while (timeout-- && (arch_inb(0x64) & 0x02));
}

static void mouse_wait_read(void)
{
    unsigned int timeout = 100000;
    while (timeout-- && !(arch_inb(0x64) & 0x01));
}

static void mouse_write(unsigned char byte)
{
    mouse_wait_write();
    arch_outb(0x64, 0xD4); /* tell controller next byte goes to aux port */
    mouse_wait_write();
    arch_outb(0x60, byte);
}

static unsigned char mouse_read(void)
{
    mouse_wait_read();
    return arch_inb(0x60);
}

void mouse_init(void)
{
    unsigned char status;

    /* Enable auxiliary (mouse) PS/2 port */
    mouse_wait_write();
    arch_outb(0x64, 0xA8);

    /* Read and update the compaq status byte to enable IRQ12 + aux clock */
    mouse_wait_write();
    arch_outb(0x64, 0x20);
    status = mouse_read();
    status |=  0x02; /* enable IRQ12 */
    status &= ~0x20; /* enable aux clock */
    mouse_wait_write();
    arch_outb(0x64, 0x60);
    mouse_wait_write();
    arch_outb(0x60, status);

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read(); /* ACK */
}

void mouse_handle_byte(unsigned char byte)
{
    /* First byte must have bit 3 set; drop garbage bytes */
    if (mouse_packet_index == 0 && !(byte & 0x08))
    {
        return;
    }

    mouse_packet[mouse_packet_index++] = byte;

    if (mouse_packet_index == 3)
    {
        unsigned char s = mouse_packet[0];
        int dx = (int)mouse_packet[1] - ((s & 0x10) ? 256 : 0);
        int dy = (int)mouse_packet[2] - ((s & 0x20) ? 256 : 0);

        mouse_buttons  = s & 0x07;
        mouse_x       += dx;
        mouse_y       -= dy; /* PS/2 dy is inverted vs. screen coords */

        mouse_packet_index = 0;
    }
}
