#ifndef TG11_SERIAL_CONSOLE_H
#define TG11_SERIAL_CONSOLE_H

/*
 * Entry point for the serial rescue console kernel task.
 * Create with task_create("serial", serial_console_task, 0).
 */
void serial_console_task(void *arg);

#endif /* TG11_SERIAL_CONSOLE_H */
