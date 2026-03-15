static volatile unsigned short *const VGA = (unsigned short *)0xB8000;
static const unsigned long VGA_WIDTH = 80;
static const unsigned long VGA_HEIGHT = 25;

static unsigned long term_row = 0;
static unsigned long term_col = 0;
static unsigned char term_color = 0x0F; /* white on black */

static void terminal_clear(void)
{
    for (unsigned long y = 0; y < VGA_HEIGHT; y++)
    {
        for (unsigned long x = 0; x < VGA_WIDTH; x++)
        {
            VGA[y * VGA_WIDTH + x] = ((unsigned short)term_color << 8) | ' ';
        }
    }
    term_row = 0;
    term_col = 0;
}

static void terminal_putchar(char c)
{
    if (c == '\n')
    {
        term_col = 0;
        term_row++;
        return;
    }

    VGA[term_row * VGA_WIDTH + term_col] =
        ((unsigned short)term_color << 8) | (unsigned char)c;

    term_col++;
    if (term_col >= VGA_WIDTH)
    {
        term_col = 0;
        term_row++;
    }

    if (term_row >= VGA_HEIGHT)
    {
        term_row = 0;
    }
}

static void terminal_write(const char *str)
{
    while (*str)
    {
        terminal_putchar(*str++);
    }
}

void kernel_main(void)
{
    terminal_clear();
    terminal_write("TG11 OS (64-bit)\n");
    terminal_write("v0.0.1\n");

    for (;;)
    {
        __asm__ volatile("hlt");
    }
}