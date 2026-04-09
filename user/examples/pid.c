/**
 * pid.c — Print the current process ID and exit.
 */
#include "tg11.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    print("My PID is ");
    print_ulong((unsigned long)getpid());
    write(1, "\n", 1);
    return 0;
}
