#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "minit.h"

#define MAX_SVC          64
#define RESTART_DELAY_MS 1000
#define DEFAULT_PATH     "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

typedef enum { ST_STOPPED, ST_RUNNING, ST_BACKOFF, ST_FAILED } state_t;
static const char *state_names[] = {"stopped", "running", "backoff", "failed"};

typedef struct {
    char name[64];
    char **argv;
    int max_restarts;
    int restarts;
    pid_t pid;
    state_t state;
    int wanted;
    int want_restart;
    int last_status;
    long long restart_at;
    long long started;
} svc_t;

static svc_t svcs[MAX_SVC];
static int nsvc;

static void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void panic(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "minit: PANIC: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    sync();
    exit(1);
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void mknod_safe(const char *path, mode_t mode, unsigned maj, unsigned min) {
    if (mknod(path, mode, makedev(maj, min)) < 0 && errno != EEXIST)
        fprintf(stderr, "minit: mknod %s: %s\n", path, strerror(errno));
}

static void symlink_safe(const char *target, const char *path) {
    if (symlink(target, path) < 0 && errno != EEXIST)
        fprintf(stderr, "minit: symlink %s: %s\n", path, strerror(errno));
}

static void mount_dev(void) {
    mkdir("/dev", 0755);

    if (mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755") < 0) {
        fprintf(stderr, "minit: devtmpfs unavailable (%s), falling back to tmpfs\n",
                strerror(errno));
        if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755,size=64k") < 0) {
            perror("minit: mount tmpfs on /dev");
            return;
        }
        // Without devtmpfs, kernel doesn't create device nodes, init MUST create a minimal set ourselves.
        mknod_safe("/dev/null",    S_IFCHR | 0666, 1, 3);
        mknod_safe("/dev/zero",    S_IFCHR | 0666, 1, 5);
        mknod_safe("/dev/full",    S_IFCHR | 0666, 1, 7);
        mknod_safe("/dev/random",  S_IFCHR | 0666, 1, 8);
        mknod_safe("/dev/urandom", S_IFCHR | 0666, 1, 9);
        mknod_safe("/dev/tty",     S_IFCHR | 0666, 5, 0);
        mknod_safe("/dev/console", S_IFCHR | 0600, 5, 1);
    }

    mkdir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
              "gid=5,mode=620,ptmxmode=666") < 0)
        perror("minit: mount devpts");

    mkdir("/dev/shm", 01777);
    chmod("/dev/shm", 01777);
    if (mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0)
        perror("minit: mount /dev/shm");

    symlink_safe("pts/ptmx",       "/dev/ptmx");
    symlink_safe("/proc/self/fd",  "/dev/fd");
    symlink_safe("/proc/self/fd/0", "/dev/stdin");
    symlink_safe("/proc/self/fd/1", "/dev/stdout");
    symlink_safe("/proc/self/fd/2", "/dev/stderr");

    // if the kernel hasnt provided stdio, bind it to the console
    if (fcntl(0, F_GETFD) < 0) {
        int c = open("/dev/console", O_RDWR);
        if (c >= 0) {
            dup2(c, 0); dup2(c, 1); dup2(c, 2);
            if (c > 2) close(c);
        }
    }
}

static void mount_fs(void) {
    mount_dev();

    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir(RUN_DIR, 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("tmpfs", RUN_DIR, "tmpfs", 0, "mode=0755");
}

static svc_t *find_by_name(const char *name) {
    for (int i = 0; i < nsvc; i++)
        if (strcmp(svcs[i].name, name) == 0) return &svcs[i];
    return NULL;
}

static svc_t *find_by_pid(pid_t pid) {
    for (int i = 0; i < nsvc; i++)
        if (svcs[i].pid == pid) return &svcs[i];
    return NULL;
}

static void log_event(svc_t *s, const char *fmt, ...) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, LOG_DIR "/%s.log", s->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char buf[512];
    int n = snprintf(buf, sizeof buf, "[minit] ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t r = write(fd, buf, n);
    (void)r;
    close(fd);
}

static void fmt_status(char *out, size_t sz, int st) {
    if (st < 0)              snprintf(out, sz, "-");
    else if (WIFEXITED(st))  snprintf(out, sz, "exit %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(out, sz, "signal %d", WTERMSIG(st));
    else                     snprintf(out, sz, "?");
}

//config
static char **tokenize(char *line, int *argc_out) {
    char **argv = NULL;
    int argc = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *start;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
        } else {
            start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
        }
        if (*p) *p++ = 0;
        argv = realloc(argv, (argc + 2) * sizeof *argv);
        if (!argv) panic("out of memory");
        argv[argc++] = strdup(start);
        argv[argc] = NULL;
    }
    *argc_out = argc;
    return argv;
}

static void load_config(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) panic("cannot open %s: %s", CONF_PATH, strerror(errno));

    char line[512];
    int lineno = 0;
    int default_restarts = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;

        if (strncmp(p, "restarts", 8) == 0) {
            char *q = p + 8;
            while (isspace((unsigned char)*q)) q++;
            if (*q == '=') {
                q++;
                char *end;
                long v = strtol(q, &end, 10);
                while (isspace((unsigned char)*end)) end++;
                if (end == q || *end || v < 0 || v > 1000000)
                    panic("%s:%d: bad restarts value", CONF_PATH, lineno);
                default_restarts = (int)v;
                continue;
            }
        }

        char name[64] = "";
        size_t k = 0;
        while (isalnum((unsigned char)p[k]) || p[k] == '_' || p[k] == '-' || p[k] == '.') k++;
        if (k > 0 && k < sizeof name && p[k] == ':' &&
            (p[k + 1] == 0 || isspace((unsigned char)p[k + 1]))) {
            memcpy(name, p, k);
            name[k] = 0;
            p += k + 1;
        }

        int argc;
        char **argv = tokenize(p, &argc);
        if (argc == 0) panic("%s:%d: missing command", CONF_PATH, lineno);
        if (nsvc >= MAX_SVC) panic("too many services (max %d)", MAX_SVC);

        if (!name[0]) {
            const char *b = strrchr(argv[0], '/');
            snprintf(name, sizeof name, "%.58s", b ? b + 1 : argv[0]);
        }
        char uniq[64];
        snprintf(uniq, sizeof uniq, "%s", name);
        for (int n = 2; find_by_name(uniq); n++)
            snprintf(uniq, sizeof uniq, "%.50s-%d", name, n);

        svc_t *s = &svcs[nsvc++];
        memset(s, 0, sizeof *s);
        snprintf(s->name, sizeof s->name, "%s", uniq);
        s->argv = argv;
        s->max_restarts = default_restarts;
        s->last_status = -1;
        s->state = ST_STOPPED;
    }
    fclose(f);
}

// services handling
static void schedule_restart(svc_t *s) {
    if (s->restarts < s->max_restarts) {
        s->restarts++;
        s->state = ST_BACKOFF;
        s->restart_at = now_ms() + RESTART_DELAY_MS;
        log_event(s, "restart %d/%d in %d ms", s->restarts, s->max_restarts, RESTART_DELAY_MS);
    } else {
        s->state = ST_FAILED;
        log_event(s, "restart limit reached (%d), giving up", s->max_restarts);
    }
}

static void start_service(svc_t *s) {
    char logpath[PATH_MAX];
    snprintf(logpath, sizeof logpath, LOG_DIR "/%s.log", s->name);

    pid_t pid = fork();
    if (pid < 0) {
        perror("minit: fork");
        schedule_restart(s);
        return;
    }
    if (pid == 0) {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);
        signal(SIGPIPE, SIG_DFL);
        setsid();

        int in = open("/dev/null", O_RDONLY);
        int out = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }

        execvp(s->argv[0], s->argv);
        fprintf(stderr, "minit: exec %s failed: %s\n", s->argv[0], strerror(errno));
        _exit(127);
    }
    s->pid = pid;
    s->state = ST_RUNNING;
    s->started = now_ms();
    log_event(s, "started pid %d", pid);
}

static void handle_exit(pid_t pid, int status) {
    svc_t *s = find_by_pid(pid);
    if (!s) return;

    char st[32];
    fmt_status(st, sizeof st, status);
    log_event(s, "pid %d exited (%s)", pid, st);

    s->pid = 0;
    s->last_status = status;

    if (s->want_restart) {
        s->want_restart = 0;
        s->restarts = 0;
        start_service(s);
    } else if (!s->wanted) {
        s->state = ST_STOPPED;
    } else {
        schedule_restart(s);
    }
}

static void reap(int sigfd) {
    struct signalfd_siginfo si;
    while (read(sigfd, &si, sizeof si) == (ssize_t)sizeof si) {}
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        handle_exit(pid, status);
}

// CLI

static void print_status(int fd, svc_t *s) {
    char last[32], up[32] = "";
    fmt_status(last, sizeof last, s->last_status);
    if (s->state == ST_RUNNING)
        snprintf(up, sizeof up, " up=%llds", (now_ms() - s->started) / 1000);
    dprintf(fd, "%-16s %-8s pid=%-6d restarts=%d/%d last=%s%s\n",
            s->name, state_names[s->state], (int)s->pid,
            s->restarts, s->max_restarts, last, up);
}

static void cmd_start(svc_t *s) {
    s->wanted = 1;
    s->restarts = 0;
    s->want_restart = 0;
    start_service(s);
}

static void handle_client(int fd) {
    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    buf[strcspn(buf, "\r\n")] = 0;

    char *save;
    char *cmd = strtok_r(buf, " ", &save);
    char *arg = strtok_r(NULL, " ", &save);
    if (!cmd) return;

    if (strcmp(cmd, "status") == 0) {
        if (arg) {
            svc_t *s = find_by_name(arg);
            if (!s) { dprintf(fd, "error: no such service: %s\n", arg); return; }
            print_status(fd, s);
        } else {
            for (int i = 0; i < nsvc; i++) print_status(fd, &svcs[i]);
        }
        return;
    }

    if (!arg) { dprintf(fd, "error: %s needs a service name\n", cmd); return; }
    svc_t *s = find_by_name(arg);
    if (!s) { dprintf(fd, "error: no such service: %s\n", arg); return; }

    if (strcmp(cmd, "start") == 0) {
        if (s->pid > 0) { dprintf(fd, "error: %s already running\n", s->name); return; }
        cmd_start(s);
        dprintf(fd, "started %s\n", s->name);
    } else if (strcmp(cmd, "stop") == 0) {
        s->wanted = 0;
        s->want_restart = 0;
        if (s->pid > 0) kill(s->pid, SIGTERM);
        else s->state = ST_STOPPED;
        dprintf(fd, "stopping %s\n", s->name);
    } else if (strcmp(cmd, "restart") == 0) {
        if (s->pid > 0) {
            s->wanted = 1;
            s->want_restart = 1;
            kill(s->pid, SIGTERM);
        } else {
            cmd_start(s);
        }
        dprintf(fd, "restarting %s\n", s->name);
    } else {
        dprintf(fd, "error: unknown command: %s\n", cmd);
    }
}

static int make_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) panic("socket: %s", strerror(errno));
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    unlink(SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        panic("bind %s: %s", SOCK_PATH, strerror(errno));
    chmod(SOCK_PATH, 0600);
    if (listen(fd, 8) < 0) panic("listen: %s", strerror(errno));
    return fd;
}

int main(void) {
    int is_init = (getpid() == 1);
    if (!is_init) fprintf(stderr, "Warning: minit is intended to run as PID 1\n");

    setenv("PATH", DEFAULT_PATH, 0);
    signal(SIGPIPE, SIG_IGN);

    if (is_init) mount_fs();
    mkdir("/var", 0755);
    mkdir("/var/log", 0755);
    mkdir(LOG_DIR, 0755);

    load_config();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sigfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sigfd < 0) panic("signalfd: %s", strerror(errno));

    int lsock = make_socket();

    for (int i = 0; i < nsvc; i++) {
        svcs[i].wanted = 1;
        start_service(&svcs[i]);
    }

    for (;;) {
        long long now = now_ms();
        int timeout = -1;
        for (int i = 0; i < nsvc; i++) {
            if (svcs[i].state != ST_BACKOFF) continue;
            long long d = svcs[i].restart_at - now;
            if (d < 0) d = 0;
            if (timeout < 0 || d < timeout) timeout = (int)d;
        }

        struct pollfd fds[2] = {{sigfd, POLLIN, 0}, {lsock, POLLIN, 0}};
        int r = poll(fds, 2, timeout);
        if (r < 0 && errno != EINTR) { perror("minit: poll"); sleep(1); }

        if (fds[0].revents & POLLIN) reap(sigfd);
        if (fds[1].revents & POLLIN) {
            int c = accept4(lsock, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) { handle_client(c); close(c); }
        }

        now = now_ms();
        for (int i = 0; i < nsvc; i++)
            if (svcs[i].state == ST_BACKOFF && svcs[i].restart_at <= now)
                start_service(&svcs[i]);
    }
}
