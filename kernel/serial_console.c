/**
 * Copyright (C) 2026 TG11
 *
 * Serial rescue console — a standalone kernel task that owns COM1
 * for input and exposes a minimal rescue command set.
 *
 * This task runs independently from the VGA shell (task0).  With the
 * PIT timer driving preemption, this console can respond to commands
 * even when the VGA shell is blocked in a tight loop.
 *
 * Supported commands: help tasks shellspawn taskprotect shellwatch tasklog
 *                     taskkill taskstop taskcont ticks reboot
 */
#include "serial_console.h"
#include "serial.h"
#include "task.h"
#include "timer.h"
#include "arch.h"
#include "kernel.h"

#define SC_LINE_MAX 80

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static int sc_str_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int sc_str_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
    return 1;
}

static unsigned int sc_parse_uint(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned int)(*s - '0'); s++; }
    return v;
}

static const char *sc_read_token(const char *s, char *out, unsigned int out_sz)
{
    unsigned int i = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s != '\0' && *s != ' ' && *s != '\t' && i + 1 < out_sz) out[i++] = *s++;
    out[i] = '\0';
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void sc_dispatch(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;

    if (*line == '\0') { serial_write("serial> "); return; }

    if (sc_str_eq(line, "help"))
    {
        serial_write("Serial rescue console commands:\r\n"
                     "  tasks               list all tasks\r\n"
                     "  shellspawn          ensure shell task exists\r\n"
                     "  shellwatch <on|off|show>  shell respawn watchdog\r\n"
                     "  taskprotect <t> <on|off>  set protection by id/name\r\n"
                     "  tasklog <on|off|show>     task event serial log\r\n"
                     "  taskkill <id>       kill task by id\r\n"
                     "  taskstop <id>       suspend task\r\n"
                     "  taskcont <id>       resume suspended task\r\n"
                     "  ticks               show timer tick count\r\n"
                     "  reboot              reboot system\r\n");
    }
    else if (sc_str_eq(line, "tasks"))
    {
        task_print_list_serial();
    }
    else if (sc_str_eq(line, "shellspawn"))
    {
        int id = kernel_ensure_shell_task();
        if (id < 0) serial_write("shellspawn: failed\r\n");
        else        serial_write("shellspawn: shell ready\r\n");
    }
    else if (sc_str_starts(line, "shellwatch"))
    {
        char mode[8];
        sc_read_token(line + 10, mode, sizeof(mode));
        if (mode[0] == '\0' || sc_str_eq(mode, "show"))
        {
            serial_write("shellwatch: ");
            serial_write(kernel_shell_watchdog_enabled() ? "on\r\n" : "off\r\n");
        }
        else if (sc_str_eq(mode, "on"))
        {
            kernel_set_shell_watchdog(1);
            serial_write("shellwatch: enabled\r\n");
        }
        else if (sc_str_eq(mode, "off"))
        {
            kernel_set_shell_watchdog(0);
            serial_write("shellwatch: disabled\r\n");
        }
        else serial_write("shellwatch: use on/off/show\r\n");
    }
    else if (sc_str_starts(line, "taskprotect "))
    {
        char target[24];
        char mode[8];
        const char *rest = sc_read_token(line + 12, target, sizeof(target));
        sc_read_token(rest, mode, sizeof(mode));
        if (target[0] == '\0' || mode[0] == '\0')
        {
            serial_write("taskprotect: usage taskprotect <id|name> <on|off>\r\n");
        }
        else if (!(sc_str_eq(mode, "on") || sc_str_eq(mode, "off")))
        {
            serial_write("taskprotect: use on/off\r\n");
        }
        else
        {
            unsigned int id = sc_parse_uint(target);
            int rc = (id > 0)
                ? task_set_protection_by_id(id, sc_str_eq(mode, "on"))
                : task_set_protection_by_name(target, sc_str_eq(mode, "on"));
            if (rc == 0) serial_write("taskprotect: updated\r\n");
            else serial_write("taskprotect: failed\r\n");
        }
    }
    else if (sc_str_starts(line, "tasklog"))
    {
        char mode[8];
        sc_read_token(line + 7, mode, sizeof(mode));
        if (mode[0] == '\0' || sc_str_eq(mode, "show"))
        {
            serial_write("tasklog: ");
            serial_write(task_event_log_enabled() ? "on\r\n" : "off\r\n");
        }
        else if (sc_str_eq(mode, "on"))
        {
            task_set_event_log(1);
            serial_write("tasklog: enabled\r\n");
        }
        else if (sc_str_eq(mode, "off"))
        {
            task_set_event_log(0);
            serial_write("tasklog: disabled\r\n");
        }
        else serial_write("tasklog: use on/off/show\r\n");
    }
    else if (sc_str_starts(line, "taskkill "))
    {
        unsigned int id = sc_parse_uint(line + 9);
        if (id == 0)             serial_write("taskkill: bad id\r\n");
        else if (task_kill(id) == 0)  serial_write("taskkill: terminated\r\n");
        else                     serial_write("taskkill: failed (no such task or protected)\r\n");
    }
    else if (sc_str_starts(line, "taskstop "))
    {
        unsigned int id = sc_parse_uint(line + 9);
        if (id == 0)              serial_write("taskstop: bad id\r\n");
        else if (task_stop(id) == 0)   serial_write("taskstop: stopped\r\n");
        else                      serial_write("taskstop: failed\r\n");
    }
    else if (sc_str_starts(line, "taskcont "))
    {
        unsigned int id = sc_parse_uint(line + 9);
        if (id == 0)               serial_write("taskcont: bad id\r\n");
        else if (task_continue(id) == 0) serial_write("taskcont: resumed\r\n");
        else                       serial_write("taskcont: failed\r\n");
    }
    else if (sc_str_eq(line, "ticks"))
    {
        serial_write("ticks: ");
        /* write decimal tick count to serial */
        {
            char buf[20];
            unsigned int i = 0;
            unsigned long v = timer_ticks();
            if (v == 0) {
                buf[i++] = '0';
            } else {
                while (v > 0) { buf[i++] = (char)('0' + v % 10); v /= 10; }
                { unsigned int lo = 0, hi = i - 1;
                  while (lo < hi) { char t = buf[lo]; buf[lo] = buf[hi]; buf[hi] = t; lo++; hi--; } }
            }
            buf[i] = '\0';
            serial_write(buf);
        }
        serial_write("\r\n");
    }
    else if (sc_str_eq(line, "reboot"))
    {
        serial_write("Rebooting...\r\n");
        arch_disable_interrupts();
        arch_outb(0xCF9, 0x06);
        arch_outb(0x64, 0xFE);
        for (;;) arch_halt();
    }
    else
    {
        serial_write("unknown: '");
        serial_write(line);
        serial_write("' (type 'help')\r\n");
    }

    serial_write("serial> ");
}

/* ------------------------------------------------------------------ */
/* Task entry point                                                   */
/* ------------------------------------------------------------------ */

void serial_console_task(void *arg)
{
    char line[SC_LINE_MAX];
    unsigned int len = 0;
    int last_was_cr = 0;
    char c;
    (void)arg;

    serial_write("\r\n[serial] rescue console ready -- type 'help'\r\n");
    serial_write("serial> ");

    for (;;)
    {
        int got_any = 0;

        while (serial_try_read(&c))
        {
            got_any = 1;

            if (last_was_cr && c == '\n') { last_was_cr = 0; continue; }
            last_was_cr = 0;

            if (c == '\r' || c == '\n')
            {
                last_was_cr = (c == '\r');
                serial_write("\r\n");
                line[len] = '\0';
                sc_dispatch(line);
                len = 0;
                continue;
            }

            if (c == '\b' || c == 0x7F)
            {
                if (len > 0) { len--; serial_write("\b \b"); }
                continue;
            }

            if (c == 0x03) /* Ctrl+C */
            {
                serial_write("^C\r\nserial> ");
                len = 0;
                continue;
            }

            if (len + 1 < SC_LINE_MAX)
            {
                serial_putchar(c); /* echo */
                line[len++] = c;
            }
        }

        if (!got_any)
            task_yield();
    }
}
