#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include "minit.h"

static void usage(void) {
    fprintf(stderr,
        "usage: minitctl status [name]\n"
        "       minitctl start|stop|restart <name>\n"
        "       minitctl log <name> [lines]   (default 50)\n");
    exit(2);
}

static int send_cmd(const char *line) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "minitctl: cannot connect to %s: %s\n", SOCK_PATH, strerror(errno));
        return 1;
    }
    char msg[300];
    int n = snprintf(msg, sizeof msg, "%s\n", line);
    if (write(fd, msg, n) != n) { perror("write"); return 1; }

    char buf[4096];
    ssize_t r;
    int first = 1, rc = 0;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        if (first && r >= 5 && memcmp(buf, "error", 5) == 0) rc = 1;
        first = 0;
        if (fwrite(buf, 1, r, rc ? stderr : stdout) != (size_t)r) break;
    }
    close(fd);
    return rc;
}

static int show_log(const char *name, int lines) {
    if (strchr(name, '/') || name[0] == '.') {
        fprintf(stderr, "minitctl: bad service name\n");
        return 1;
    }
    char path[512];
    snprintf(path, sizeof path, LOG_DIR "/%s.log", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "minitctl: %s: %s\n", path, strerror(errno)); return 1; }

    off_t size = lseek(fd, 0, SEEK_END);
    off_t from = size > 65536 ? size - 65536 : 0;
    size_t len = (size_t)(size - from);
    char *buf = malloc(len + 1);
    if (!buf) return 1;
    ssize_t got = pread(fd, buf, len, from);
    close(fd);
    if (got < 0) { perror("read"); free(buf); return 1; }
    len = (size_t)got;

    size_t start = len;
    int count = 0;
    if (len > 0 && buf[len - 1] == '\n') start--;
    while (start > 0) {
        if (buf[start - 1] == '\n' && ++count == lines) break;
        start--;
    }
    fwrite(buf + start, 1, len - start, stdout);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "log")) {
        if (argc < 3) usage();
        int lines = argc > 3 ? atoi(argv[3]) : 50;
        if (lines <= 0) lines = 50;
        return show_log(argv[2], lines);
    }
    if (!strcmp(cmd, "status")) {
        char line[300];
        snprintf(line, sizeof line, argc > 2 ? "status %s" : "status", argc > 2 ? argv[2] : "");
        return send_cmd(line);
    }
    if (!strcmp(cmd, "start") || !strcmp(cmd, "stop") || !strcmp(cmd, "restart")) {
        if (argc < 3) usage();
        char line[300];
        snprintf(line, sizeof line, "%s %s", cmd, argv[2]);
        return send_cmd(line);
    }
    usage();
    return 2;
}
