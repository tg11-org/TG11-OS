/**
 * cat.c — Read and display file contents.
 *
 * Usage: exec /cat.elf /path/to/file
 */
#include "tg11.h"

int main(int argc, char **argv)
{
    int fd;
    char buf[512];
    ssize_t n;

    if (argc < 2)
    {
        puts("usage: cat <file>");
        return 1;
    }

    fd = (int)open(argv[1]);
    if (fd < 0)
    {
        print("cat: cannot open ");
        puts(argv[1]);
        return 1;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);

    close(fd);
    return 0;
}
