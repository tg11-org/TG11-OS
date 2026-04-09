/**
 * echo.c — Print command-line arguments, one per line.
 */
#include "tg11.h"

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
    {
        print(argv[i]);
        if (i + 1 < argc)
            write(1, " ", 1);
    }
    write(1, "\n", 1);
    return 0;
}
