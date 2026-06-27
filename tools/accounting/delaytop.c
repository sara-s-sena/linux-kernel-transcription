// SPDX-License-Identifier: GPL-2.0
/*
 * delaytop.c - system-wide delay monitoring tool.
 *
 * This tool provides real-time monitoring and statistics of
 * system, container, and task-level delays, including CPU,
 * memory, IO, and IRQ. It supports both interactive (top-like),
 * and can output delay information for the whole system, specific
 * containers (cgroups), or individual tasks (PIDs).
 *
 * Key features:
 *      - Collects per-task delay accounting statistics via taskstats.
 *      - Collects system-wide PSI information.
 *      - Supports sorting, filtering.
 *      - Supports both interactive (screen refresh).
 *
 * Copyright (C) Fan Yu, ZTE Corp. 2025
 * Copyright (C) Wang Yaxin, ZTE Corp. 2025
 * 
 * Compile with
 *      gcc -I/usr/src/linux/include delaytop.c -o delaytop
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <termios.h>
#include <limits.h>
#include <linux/genetlink.h>
#include <linux/taskstats.h>
#include <linux/cgroupstats.h>
#include <stddef.h>

#define PSI_PATH        "/proc/pressure"
#define PSI_CPU_PATH    "/proc/pressure/cpu"
#define PSI_MEMORY_PATH "/proc/pressure/memory"
#define PSI_IO_PATH     "/proc/pressure/io"
#define PSI_IRQ_PATH    "/proc/pressure/irq"

#define NLA_NEXT(na)                    ((struct nlattr *)((char *)(na) + NLA_ALIGN((na)->nla_len)))
#define NLA_DATA(na)                    ((void *)((char *)(na) + NLA_HDRLEN))
#define NLA_PAYLOAD(len)                (len - NLA_HDRLEN)

#define GENLMSG_DATA(glh)               ((void *)(NLMSG_DATA(glh) + GENL_HDRLEN))
#define GENLMSG_PAYLOAD(glh)    (NLMSG_PAYLOAD(glh, 0) - GENL_HDRLEN)

#define TASK_COMM_LEN   16
#define MAX_MSG_SIZE    1024    
#define MAX_TASKS               1000
#define MAX_BUF_LEN             256
#define SET_TASK_STAT(task_count, field) tasks[task_count].field = stats.field 
#define BOOL_FPRINT(stream, fmt, ...) \
({ \
        int ret = fprint(stream, fmt, ##__VA_ARGS__); \
        ret >= 0; \
})
#define TASK_AVG(task, field) average_ms((task).field##_delay_total, (task).field##_count)
#define PSI_LINE_FORMAT "%-12s %6.1f%%/%6.1f%%/%6.1f%%/%8llu(ms)\n"
#define DELAY_FMT_DEFAULT "%8.2f %8.2f %8.2f %8.2f\n"
#define DELAY_FMT_MEMVERBOSE "%8.2f %8.2f %8.2f %8.2f %8.2f %8.2f\n"
#define SORT_FIELD(name, cmd, modes) \
        {#name, #cmd, \
        offsetof(struct task_info, name##_delay_total), \
        offsetof(struct task_info, name##_count), \
        modes}
#define END_FIELD {NULL, 0, 0}

/* Display mode types */
#define MODE_TYPE_ALL   (0xFFFFFFFF)
#define MODE_DEFAULT    (1 << 0)
#define MODE_MEMVERBOSE (1 << 1)

/* PSI statistics structure */
struct psi_stats {
        double cpu_some_avg10, cpu_some_avg60, cpu_some_avg300;
        unsigned long long cpu_some_total;
        double cpu_full_avg10, cpu_full_avg60, cpu_full_avg300;
        unsigned long long cpu_full_total;
        double memory_some_avg10, memory_some_avg60, memory_some_avg300;
        unsigned long long memory_some_total;
        double memory_full_avg10, memory_full_avg60, memory_full_avg300;
        unsigned long long memory_full_total;
        double io_some_avg10, io_some_avg60, io_some_avg300;
        unsigned long long io_some_total;
        double io_full_avg10, io_full_avg60, io_full_avg300;
        unsigned long long io_full_total;
        double irq_full_avg10, irq_full_avg60, irq_full_avg300;
        unsigned long long irq_full_total;
};

/* Task delay information structure */
struct task_info {
        int pid;
        int tgid;
        char command[TASK_COMM_LEN];
        unsigned long long cpu_count;
        unsigned long long cpu_delay_total;
        unsigned long long blkio_count;
        unsigned long long blkio_delay_total;
        unsigned long long swapin_count;
        unsigned long long swapin_delay_total;
        unsigned long long freepages_count;
        unsigned long long freepages_delay_total;
        unsigned long long thrashing_count;
        unsigned long long thrashing_delay_total;
        unsigned long long compact_count;
        unsigned long long compact_delay_total;
        unsigned long long wpcopy_count;
        unsigned long long wpcopy_delay_total;
        unsigned long long irq_count;
        unsigned long long irq_delay_total;
        unsigned long long mem_count;
        unsigned long long mem_delay_total;
};

/* Container statistics structure */
struct container_stats {
        int nr_sleeping;                /* Number of sleeping processes */
        int nr_running;                 /* Number of running processes */
        int nr_stopped;                 /* Number of stopped processes */
        int nr_uninterruptible; /* Number of uninterruptible processes */
        int nr_io_wait;                 /* Number of processes in IO wait */
};

/* Delay field structure */
struct field_desc {
        const char *name;       /* Field name for cmdline argument */
        const char *cmd_char;   /* Interactive command */
        unsigned long total_offset; /* Offset of total delay in task_info */
        unsigned long count_offset; /* Offset of count in task_info */
        size_t supported_modes; /* Supported display modes */
};

/* Program settings structure */
struct config {
        int delay;                              /* Update interval in seconds */
        int interations;                /* Number of interations, 0 == infinite */
        int max_processes;              /* Maximum number of processes to show */
        int output_one_time;    /* Output once and exit */
        int monitor_pid;                /* Monitor specific PID */
        char *container_patch;  /* Path to container cgroup */
        const struct field_desc *sort_field;    /* Current sort field */
        size_t display_mode;    /* Current display mode */
};

/* Global variables */
static struct config cfg;
static struct psi_stats psi;
static struct task_info tasks[MAX_TASKS];
static int task_count;
static int running = 1;
static struct container_stats container_stats;
static const struct field_desc sort_fields[] = {
        SORT_FIELD(cpu,         c,      MODE_DEFAULT),
        SORT_FIELD(blkio,       i,      MODE_DEFAULT),
        SORT_FIELD(irq,         q,      MODE_DEFAULT),
        SORT_FIELD(mem,         m,      MODE_DEFAULT | MODE_MEMVERBOSE),
        SORT_FIELD(swapin,      s,      MODE_MEMVERBOSE),
        SORT_FIELD(freepages,   r,      MODE_MEMVERBOSE),
        SORT_FIELD(thrashing,   t,      MODE_MEMVERBOSE),
        SORT_FIELD(compact,     p,      MODE_MEMVERBOSE),
        SORT_FIELD(wpcopy,      w       MODE_MEMVERBOSE),
        END_FIELD 
};
static int sort_selected;

/* Netlink socket variables */
static int nl_sd = -1;
static int family_id;

/* Set terminal to non-canonical mode for q-to-quit */
static struct termios orig_termios;
static void enable_raw_mode(void)
{
        struct termios raw;

        tcgetattr(STDIN_FILENO, &orig_termios);
        raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}
static void disable_raw_mode(void)
{
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* Find field descriptor by command line */
static const struct field_desc *get_field_by_cmd_char(char ch)
{
        const struct field_desc *field;

        for (field = sort_field; field->name != NULL; field++) {
                if (field->cmd_char[0] == ch)
                        return field;
        }

        return NULL;
}