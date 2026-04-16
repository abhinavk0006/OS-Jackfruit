/*
 * engine.c  --  OS-Jackfruit Container Runtime
 *
 * FIXES over previous versions:
 *  1. SIGCHLD handler uses self-pipe trick (async-signal-safe, no mutex in handler)
 *  2. Each client connection handled in its own thread (supervisor never blocks)
 *  3. ContainerArgs stores strings as arrays (no dangling pointers after free)
 *  4. rootfs resolved to absolute path with realpath() before clone
 *  5. "run" uses polling loop instead of waitpid (avoids double-reap race)
 *
 * Usage:
 *   sudo ./engine supervisor ./rootfs        # start the long-running supervisor
 *   sudo ./engine start <name> <rootfs> <cmd> [args...]
 *   sudo ./engine run   <name> <rootfs> <cmd> [args...]   # foreground, waits
 *   sudo ./engine ps
 *   sudo ./engine logs  <name>
 *   sudo ./engine stop  <name>
 *
 * Build:   gcc -Wall -O2 -D_GNU_SOURCE -o engine engine.c -lpthread
 * Requires: root privileges
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sched.h>
#include <limits.h>

#include "monitor_ioctl.h"

/* pivot_root is not always in glibc headers */
static int my_pivot_root(const char *new_root, const char *put_old)
{
    return syscall(SYS_pivot_root, new_root, put_old);
}

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define SOCKET_PATH     "/tmp/jackfruit.sock"
#define MAX_CONTAINERS  64
#define MAX_NAME        64
#define MAX_PATH        256
#define MAX_ARGS        16
#define LOG_DIR         "/tmp/jackfruit_logs"
#define MONITOR_DEV     "/dev/container_monitor"
#define STACK_SIZE      (1024 * 1024)
#define LOG_BUF_SLOTS   512
#define LOG_MSG_SIZE    512
#define DEFAULT_SOFT_MB 64
#define DEFAULT_HARD_MB 128
#define IPC_LINE_MAX    1024

/* ------------------------------------------------------------------ */
/*  Container state                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    CS_STARTING = 0,
    CS_RUNNING,
    CS_STOPPED,
    CS_KILLED,
    CS_HARD_LIMIT_KILLED
} ContainerState;

static const char *state_str(ContainerState s)
{
    switch (s) {
    case CS_STARTING:          return "starting";
    case CS_RUNNING:           return "running";
    case CS_STOPPED:           return "stopped";
    case CS_KILLED:            return "killed";
    case CS_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                   return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Container metadata record                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    char           name[MAX_NAME];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    long           soft_bytes;
    long           hard_bytes;
    char           log_path[MAX_PATH];
    int            exit_status;
} Container;

/* ------------------------------------------------------------------ */
/*  Args passed through clone() to container child                      */
/*  All strings stored as arrays (not pointers) -- avoids pointer       */
/*  issues after parent frees this struct.                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char rootfs[MAX_PATH];
    char cmd[MAX_PATH];
    char args[MAX_ARGS][MAX_PATH];
    int  num_args;
    int  pipe_wr;          /* write-end of log pipe, handed to child */
    char hostname[MAX_NAME];
} ContainerArgs;

/* ------------------------------------------------------------------ */
/*  Bounded log buffer (ring buffer, producer-consumer)                 */
/* ------------------------------------------------------------------ */
typedef struct {
    char cname[MAX_NAME];
    char msg[LOG_MSG_SIZE];
} LogMsg;

typedef struct {
    LogMsg          slots[LOG_BUF_SLOTS];
    int             head;
    int             tail;
    int             count;
    int             shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} LogBuf;

/* ------------------------------------------------------------------ */
/*  Globals                                                             */
/* ------------------------------------------------------------------ */
static Container       g_cont[MAX_CONTAINERS];
static int             g_ncont = 0;
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;

static LogBuf          g_logbuf;
static pthread_t       g_consumer;

/* Self-pipe: SIGCHLD handler writes a byte here so main loop can reap */
static int g_sigpipe[2] = {-1, -1};

static volatile sig_atomic_t g_shutdown = 0;

/* Kernel monitor device fd */
static int g_monfd = -1;

/* ================================================================== */
/*  Log buffer operations                                               */
/* ================================================================== */

static void lb_init(LogBuf *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

/* Producer calls this: blocks if buffer full */
static void lb_push(LogBuf *b, const char *cname, const char *msg, size_t n)
{
    pthread_mutex_lock(&b->lock);
    while (b->count == LOG_BUF_SLOTS && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->lock);
    if (!b->shutdown) {
        LogMsg *s = &b->slots[b->tail];
        strncpy(s->cname, cname, MAX_NAME - 1);
        size_t cp = (n < LOG_MSG_SIZE - 1) ? n : (LOG_MSG_SIZE - 1);
        memcpy(s->msg, msg, cp);
        s->msg[cp] = '\0';
        b->tail = (b->tail + 1) % LOG_BUF_SLOTS;
        b->count++;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->lock);
}

/* Consumer calls this: blocks if buffer empty. Returns -1 when done. */
static int lb_pop(LogBuf *b, LogMsg *out)
{
    pthread_mutex_lock(&b->lock);
    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->lock);
    if (b->count == 0) {        /* shutdown with empty buffer */
        pthread_mutex_unlock(&b->lock);
        return -1;
    }
    *out = b->slots[b->head];
    b->head = (b->head + 1) % LOG_BUF_SLOTS;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 0;
}

/* Signal shutdown and wake everyone up */
static void lb_shutdown(LogBuf *b)
{
    pthread_mutex_lock(&b->lock);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->lock);
}

/* ================================================================== */
/*  Write a log message to a container's log file                       */
/* ================================================================== */

static void write_log_file(const char *cname, const char *msg)
{
    pthread_mutex_lock(&g_lock);
    char path[MAX_PATH] = {0};
    for (int i = 0; i < g_ncont; i++) {
        if (strcmp(g_cont[i].name, cname) == 0) {
            strncpy(path, g_cont[i].log_path, MAX_PATH - 1);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (!path[0]) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        close(fd);
    }
}

/* ================================================================== */
/*  Consumer thread: reads from ring buffer, writes to log files        */
/* ================================================================== */

static void *consumer_thread(void *arg)
{
    (void)arg;
    LogMsg m;

    while (lb_pop(&g_logbuf, &m) == 0)
        write_log_file(m.cname, m.msg);

    /* Drain anything remaining after shutdown signal */
    pthread_mutex_lock(&g_logbuf.lock);
    while (g_logbuf.count > 0) {
        LogMsg dm = g_logbuf.slots[g_logbuf.head];
        g_logbuf.head = (g_logbuf.head + 1) % LOG_BUF_SLOTS;
        g_logbuf.count--;
        pthread_mutex_unlock(&g_logbuf.lock);
        write_log_file(dm.cname, dm.msg);
        pthread_mutex_lock(&g_logbuf.lock);
    }
    pthread_mutex_unlock(&g_logbuf.lock);

    return NULL;
}

/* ================================================================== */
/*  Producer thread: one per container, reads from pipe, pushes to buf  */
/* ================================================================== */

typedef struct {
    int  fd;
    char cname[MAX_NAME];
} ProdArg;

static void *producer_thread(void *arg)
{
    ProdArg *pa = (ProdArg *)arg;
    char buf[LOG_MSG_SIZE];
    ssize_t n;

    while ((n = read(pa->fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        lb_push(&g_logbuf, pa->cname, buf, (size_t)n);
    }
    close(pa->fd);
    free(pa);
    return NULL;
}

/* ================================================================== */
/*  Kernel monitor helpers                                              */
/* ================================================================== */

static void mon_register(pid_t pid, long soft_bytes, long hard_bytes)
{
    if (g_monfd < 0) return;
    struct monitor_cmd c;
    memset(&c, 0, sizeof(c));
    c.pid              = pid;
    c.soft_limit_bytes = soft_bytes;
    c.hard_limit_bytes = hard_bytes;
    if (ioctl(g_monfd, MONITOR_REGISTER, &c) < 0)
        perror("engine: ioctl REGISTER");
}

static void mon_unregister(pid_t pid)
{
    if (g_monfd < 0) return;
    struct monitor_cmd c;
    memset(&c, 0, sizeof(c));
    c.pid = pid;
    ioctl(g_monfd, MONITOR_UNREGISTER, &c);
}

/* ================================================================== */
/*  Container child entrypoint (runs inside clone'd namespaces)         */
/* ================================================================== */

static int container_main(void *arg)
{
    ContainerArgs *ca = (ContainerArgs *)arg;

    /* Redirect stdout and stderr into the log pipe */
    if (ca->pipe_wr >= 0) {
        dup2(ca->pipe_wr, STDOUT_FILENO);
        dup2(ca->pipe_wr, STDERR_FILENO);
        close(ca->pipe_wr);
    }

    /* Set hostname in UTS namespace */
    if (sethostname(ca->hostname, strlen(ca->hostname)) < 0)
        perror("container: sethostname");

    /* Make root private so our mounts don't propagate to host */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("container: mount private /");

    /* --- Try pivot_root: bind-mount rootfs onto itself first --- */
    if (mount(ca->rootfs, ca->rootfs, NULL, MS_BIND | MS_REC, NULL) == 0) {
        char put_old[MAX_PATH + 16];
        snprintf(put_old, sizeof(put_old), "%s/.old_root", ca->rootfs);
        mkdir(put_old, 0700);

        if (my_pivot_root(ca->rootfs, put_old) == 0) {
            /* Success: unmount the old root */
            if (umount2("/.old_root", MNT_DETACH) < 0)
                perror("container: umount old root");
            rmdir("/.old_root");
            goto setup_fs;
        }
        /* pivot_root failed: undo bind mount and fall through to chroot */
        umount2(ca->rootfs, MNT_DETACH);
    }

    /* --- Fallback: chroot --- */
    if (chroot(ca->rootfs) < 0) {
        perror("container: chroot");
        exit(EXIT_FAILURE);
    }

setup_fs:
    chdir("/");

    /* Mount /proc */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("container: mount /proc");

    /* Minimal /dev/pts */
    mkdir("/dev", 0755);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, "newinstance");

    /* Build argv and exec the requested command */
    char *argv_exec[MAX_ARGS + 2];
    argv_exec[0] = ca->cmd;
    for (int i = 0; i < ca->num_args; i++)
        argv_exec[i + 1] = ca->args[i];
    argv_exec[ca->num_args + 1] = NULL;

    execvp(ca->cmd, argv_exec);
    fprintf(stderr, "container: execvp '%s' failed: %s\n", ca->cmd, strerror(errno));
    exit(EXIT_FAILURE);
}

/* ================================================================== */
/*  Helper: find container by name (call with g_lock held)             */
/* ================================================================== */

static Container *find_cont(const char *name)
{
    for (int i = 0; i < g_ncont; i++)
        if (strcmp(g_cont[i].name, name) == 0)
            return &g_cont[i];
    return NULL;
}

/* ================================================================== */
/*  Launch a container (MUST be called with g_lock held)               */
/*  Returns host PID on success, -1 on error.                          */
/* ================================================================== */

static pid_t launch_container(const char *name, const char *rootfs,
                               const char *cmd,
                               char args[MAX_ARGS][MAX_PATH], int nargs,
                               long soft_mb, long hard_mb)
{
    if (g_ncont >= MAX_CONTAINERS) {
        fprintf(stderr, "engine: too many containers\n");
        return -1;
    }

    /* Resolve rootfs to absolute path */
    char abs_rootfs[PATH_MAX];
    if (!realpath(rootfs, abs_rootfs)) {
        fprintf(stderr, "engine: rootfs '%s' not found: %s\n", rootfs, strerror(errno));
        return -1;
    }

    mkdir(LOG_DIR, 0755);

    Container *c = &g_cont[g_ncont];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, MAX_NAME - 1);
    c->start_time = time(NULL);
    c->state      = CS_STARTING;
    c->soft_bytes = soft_mb * 1024 * 1024;
    c->hard_bytes = hard_mb * 1024 * 1024;
    snprintf(c->log_path, MAX_PATH, "%s/%s.log", LOG_DIR, name);

    /* Create pipe for container stdout/stderr -> supervisor log */
    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("engine: pipe");
        return -1;
    }

    /* Build ContainerArgs on heap -- child gets COW copy via clone */
    ContainerArgs *ca = calloc(1, sizeof(ContainerArgs));
    if (!ca) {
        close(pfd[0]); close(pfd[1]);
        return -1;
    }
    strncpy(ca->rootfs,   abs_rootfs, MAX_PATH - 1);
    strncpy(ca->cmd,      cmd,        MAX_PATH - 1);
    strncpy(ca->hostname, name,       MAX_NAME - 1);
    ca->pipe_wr  = pfd[1];
    ca->num_args = nargs;
    for (int i = 0; i < nargs && i < MAX_ARGS; i++)
        strncpy(ca->args[i], args[i], MAX_PATH - 1);

    /* Allocate stack for the cloned process */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(ca);
        close(pfd[0]); close(pfd[1]);
        return -1;
    }

    /* Clone with new PID, UTS, and mount namespaces */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_main, stack + STACK_SIZE, clone_flags, ca);

    /* Parent: stack and ca are no longer needed (child has COW copies) */
    free(stack);
    free(ca);

    if (pid < 0) {
        perror("engine: clone");
        close(pfd[0]); close(pfd[1]);
        return -1;
    }

    /* Close write end in parent */
    close(pfd[1]);

    c->host_pid = pid;
    c->state    = CS_RUNNING;
    g_ncont++;

    /* Register with kernel monitor */
    mon_register(pid, c->soft_bytes, c->hard_bytes);

    /* Start producer thread for this container's log pipe */
    ProdArg *pa = malloc(sizeof(ProdArg));
    if (pa) {
        pa->fd = pfd[0];
        strncpy(pa->cname, name, MAX_NAME - 1);
        pthread_t t;
        if (pthread_create(&t, NULL, producer_thread, pa) == 0)
            pthread_detach(t);
        else {
            free(pa);
            close(pfd[0]);
        }
    } else {
        close(pfd[0]);
    }

    printf("engine: started container '%s' pid=%d\n", name, pid);
    fflush(stdout);
    return pid;
}

/* ================================================================== */
/*  Reap zombie children and update container state                     */
/*  Called from the main supervisor loop (NOT from signal handler).     */
/* ================================================================== */

static void reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_ncont; i++) {
            if (g_cont[i].host_pid == pid) {
                g_cont[i].exit_status = status;
                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                        && g_cont[i].state != CS_STOPPED)
                    g_cont[i].state = CS_KILLED;
                else if (g_cont[i].state == CS_RUNNING || g_cont[i].state == CS_STARTING)
                    g_cont[i].state = CS_STOPPED;
                mon_unregister(pid);
                break;
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
}

/* ================================================================== */
/*  Signal handlers                                                     */
/* ================================================================== */

/* SIGCHLD: write a byte to the self-pipe -- async-signal-safe */
static void sig_chld(int sig)
{
    (void)sig;
    char b = 1;
    /* write() is async-signal-safe; ignore errors */
    if (write(g_sigpipe[1], &b, 1) < 0) {}
}

static void sig_term(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* ================================================================== */
/*  IPC: handle one client connection                                   */
/*                                                                      */
/*  Protocol: client sends "CMD arg1 arg2 ...\n"                        */
/*            server replies with lines, terminated by ".\n"           */
/* ================================================================== */

static void handle_client(int cfd)
{
    char line[IPC_LINE_MAX];
    ssize_t n = recv(cfd, line, sizeof(line) - 1, 0);
    if (n <= 0) return;
    line[n] = '\0';

    /* strip trailing newline */
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    /* tokenise */
    char *tok[MAX_ARGS + 8];
    int   tc = 0;
    for (char *p = strtok(line, " ");
         p && tc < (int)(sizeof(tok)/sizeof(tok[0]) - 1);
         p = strtok(NULL, " "))
        tok[tc++] = p;
    tok[tc] = NULL;

    if (tc == 0) { dprintf(cfd, "error: empty command\n.\n"); return; }

    /* ---- ps ---- */
    if (strcmp(tok[0], "ps") == 0) {
        pthread_mutex_lock(&g_lock);
        dprintf(cfd, "%-16s %-8s %-18s %-20s %s\n",
                "NAME", "PID", "STATE", "STARTED", "LOG");
        for (int i = 0; i < g_ncont; i++) {
            Container *c = &g_cont[i];
            char tb[32];
            struct tm *tm = localtime(&c->start_time);
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm);
            dprintf(cfd, "%-16s %-8d %-18s %-20s %s\n",
                    c->name, c->host_pid, state_str(c->state), tb, c->log_path);
        }
        pthread_mutex_unlock(&g_lock);
        dprintf(cfd, ".\n");
        return;
    }

    /* ---- logs <name> ---- */
    if (strcmp(tok[0], "logs") == 0) {
        if (tc < 2) { dprintf(cfd, "error: logs <name>\n.\n"); return; }
        pthread_mutex_lock(&g_lock);
        Container *c = find_cont(tok[1]);
        char lp[MAX_PATH] = {0};
        if (c) strncpy(lp, c->log_path, MAX_PATH - 1);
        pthread_mutex_unlock(&g_lock);
        if (!lp[0]) { dprintf(cfd, "error: container '%s' not found\n.\n", tok[1]); return; }
        int fd = open(lp, O_RDONLY);
        if (fd < 0) { dprintf(cfd, "(log file not yet created)\n.\n"); return; }
        char buf[4096]; ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0)
            write(cfd, buf, (size_t)r);
        close(fd);
        dprintf(cfd, "\n.\n");
        return;
    }

    /* ---- stop <name> ---- */
    if (strcmp(tok[0], "stop") == 0) {
        if (tc < 2) { dprintf(cfd, "error: stop <name>\n.\n"); return; }
        pthread_mutex_lock(&g_lock);
        Container *c = find_cont(tok[1]);
        if (!c) {
            pthread_mutex_unlock(&g_lock);
            dprintf(cfd, "error: container '%s' not found\n.\n", tok[1]);
            return;
        }
        if (c->state != CS_RUNNING) {
            const char *st = state_str(c->state);
            pthread_mutex_unlock(&g_lock);
            dprintf(cfd, "container '%s' is not running (state: %s)\n.\n", tok[1], st);
            return;
        }
        pid_t pid = c->host_pid;
        c->state = CS_STOPPED;
        pthread_mutex_unlock(&g_lock);

        kill(pid, SIGTERM);
        usleep(200000);   /* give 200ms for graceful exit */
        kill(pid, SIGKILL);
        dprintf(cfd, "stopped container '%s'\n.\n", tok[1]);
        return;
    }

    /* ---- start / run ---- */
    if (strcmp(tok[0], "start") == 0 || strcmp(tok[0], "run") == 0) {
        int fg = (strcmp(tok[0], "run") == 0);
        if (tc < 4) {
            dprintf(cfd, "error: %s <name> <rootfs> <cmd> [args...]\n.\n", tok[0]);
            return;
        }
        const char *cname  = tok[1];
        const char *rootfs = tok[2];
        const char *cmd    = tok[3];

        char args[MAX_ARGS][MAX_PATH];
        int nargs = 0;
        for (int i = 4; i < tc && nargs < MAX_ARGS; i++)
            strncpy(args[nargs++], tok[i], MAX_PATH - 1);

        pthread_mutex_lock(&g_lock);
        if (find_cont(cname)) {
            pthread_mutex_unlock(&g_lock);
            dprintf(cfd, "error: container '%s' already exists. Use a different name.\n.\n", cname);
            return;
        }
        pid_t pid = launch_container(cname, rootfs, cmd, args, nargs,
                                     DEFAULT_SOFT_MB, DEFAULT_HARD_MB);
        pthread_mutex_unlock(&g_lock);

        if (pid < 0) {
            dprintf(cfd, "error: failed to launch container\n.\n");
            return;
        }

        if (!fg) {
            dprintf(cfd, "started container '%s' pid=%d\n.\n", cname, (int)pid);
            return;
        }

        /* Foreground mode: poll until container changes state */
        dprintf(cfd, "container '%s' pid=%d running (foreground)...\n", cname, (int)pid);
        while (1) {
            usleep(200000);   /* 200ms poll */
            pthread_mutex_lock(&g_lock);
            Container *c = find_cont(cname);
            int done = c && (c->state != CS_RUNNING && c->state != CS_STARTING);
            pthread_mutex_unlock(&g_lock);
            if (done) break;
        }
        dprintf(cfd, "container '%s' exited\n.\n", cname);
        return;
    }

    dprintf(cfd, "error: unknown command '%s'\n.\n", tok[0]);
}

/* Each client gets its own thread so the supervisor never blocks */
static void *client_thread(void *arg)
{
    int cfd = (int)(intptr_t)arg;
    handle_client(cfd);
    close(cfd);
    return NULL;
}

/* ================================================================== */
/*  Supervisor main loop                                                */
/* ================================================================== */

static void supervisor_run(const char *rootfs_hint)
{
    (void)rootfs_hint;

    /* Init log buffer and start consumer */
    lb_init(&g_logbuf);
    if (pthread_create(&g_consumer, NULL, consumer_thread, NULL) != 0) {
        perror("engine: pthread_create consumer");
        exit(EXIT_FAILURE);
    }

    /* Self-pipe for SIGCHLD */
    if (pipe(g_sigpipe) < 0) {
        perror("engine: pipe sigpipe");
        exit(EXIT_FAILURE);
    }
    fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);

    /* Open kernel monitor device (optional) */
    g_monfd = open(MONITOR_DEV, O_RDWR);
    if (g_monfd < 0)
        fprintf(stderr, "engine: warning: %s not available (module not loaded?)\n",
                MONITOR_DEV);

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_chld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sig_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Create UNIX domain socket */
    unlink(SOCKET_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("engine: socket"); exit(EXIT_FAILURE); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("engine: bind"); exit(EXIT_FAILURE);
    }
    chmod(SOCKET_PATH, 0666);
    if (listen(srv, 16) < 0) { perror("engine: listen"); exit(EXIT_FAILURE); }

    printf("engine: supervisor ready, socket=%s\n", SOCKET_PATH);
    fflush(stdout);

    /* Main select loop */
    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        FD_SET(g_sigpipe[0], &rfds);
        int maxfd = (srv > g_sigpipe[0]) ? srv : g_sigpipe[0];

        struct timeval tv = {1, 0};
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("engine: select");
            break;
        }

        /* Drain self-pipe and reap children (safe: no mutex in signal handler) */
        if (FD_ISSET(g_sigpipe[0], &rfds)) {
            char drain[64];
            read(g_sigpipe[0], drain, sizeof(drain));
            reap_children();
        }

        /* Accept new client connections */
        if (FD_ISSET(srv, &rfds)) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd >= 0) {
                pthread_t t;
                if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)cfd) == 0)
                    pthread_detach(t);
                else
                    close(cfd);
            }
        }
    }

    printf("engine: shutting down...\n");
    fflush(stdout);

    /* Stop all running containers */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ncont; i++)
        if (g_cont[i].state == CS_RUNNING)
            kill(g_cont[i].host_pid, SIGTERM);
    pthread_mutex_unlock(&g_lock);

    sleep(2);
    reap_children();

    /* Shutdown log pipeline */
    lb_shutdown(&g_logbuf);
    pthread_join(g_consumer, NULL);

    /* Cleanup monitor */
    if (g_monfd >= 0) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_ncont; i++)
            mon_unregister(g_cont[i].host_pid);
        pthread_mutex_unlock(&g_lock);
        close(g_monfd);
    }

    close(srv);
    unlink(SOCKET_PATH);
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);

    printf("engine: clean shutdown complete\n");
    fflush(stdout);
}

/* ================================================================== */
/*  CLI client: connect to supervisor and send command                  */
/* ================================================================== */

static void cli_send(const char *cmdline)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("cli: socket"); exit(EXIT_FAILURE); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "cli: cannot connect to supervisor at %s: %s\n"
            "     Is the supervisor running?\n"
            "     Start it with: sudo ./engine supervisor ./rootfs\n",
            SOCKET_PATH, strerror(errno));
        close(fd);
        exit(EXIT_FAILURE);
    }

    /* Send command with newline terminator */
    dprintf(fd, "%s\n", cmdline);
    shutdown(fd, SHUT_WR);

    /* Read and print response until end-of-reply marker ".\n" */
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        char *dot = strstr(buf, "\n.\n");
        if (dot) {
            *dot = '\0';
            printf("%s\n", buf);
            break;
        }
        printf("%s", buf);
    }
    close(fd);
}

/* ================================================================== */
/*  main                                                                */
/* ================================================================== */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "OS-Jackfruit Container Engine\n\n"
            "Usage:\n"
            "  sudo %s supervisor <rootfs>               -- start supervisor\n"
            "  sudo %s start  <name> <rootfs> <cmd> ...  -- start background container\n"
            "  sudo %s run    <name> <rootfs> <cmd> ...  -- run foreground container\n"
            "  sudo %s ps                                -- list containers\n"
            "  sudo %s logs   <name>                     -- show container log\n"
            "  sudo %s stop   <name>                     -- stop container\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        const char *rootfs = (argc >= 3) ? argv[2] : "./rootfs";
        supervisor_run(rootfs);
        return 0;
    }

    /* All other subcommands: forward to supervisor via socket */
    char cmdline[IPC_LINE_MAX];
    int off = 0;
    for (int i = 1; i < argc && off < (int)sizeof(cmdline) - 2; i++) {
        if (i > 1) cmdline[off++] = ' ';
        int r = snprintf(cmdline + off, sizeof(cmdline) - off - 1, "%s", argv[i]);
        if (r > 0) off += r;
    }
    cmdline[off] = '\0';
    cli_send(cmdline);
    return 0;
}
