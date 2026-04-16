/* monitor_ioctl.h
 * Shared between user-space (engine.c) and kernel module (monitor.c)
 * Defines ioctl commands and the shared data structure.
 */
#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

/* Register a PID with soft/hard memory limits (in bytes) */
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_cmd)

/* Unregister a PID */
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_cmd)

/* Query current RSS for a PID */
#define MONITOR_QUERY      _IOWR(MONITOR_MAGIC, 3, struct monitor_cmd)

struct monitor_cmd {
    pid_t pid;
    long  soft_limit_bytes;  /* warn when RSS exceeds this */
    long  hard_limit_bytes;  /* kill when RSS exceeds this */
    long  rss_kb;            /* output: filled by MONITOR_QUERY */
    int   kill_reason;       /* output: 0=running, 1=soft warned, 2=hard killed */
};

#endif /* MONITOR_IOCTL_H */
