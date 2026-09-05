#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>

void mount_fs() {
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
}

void reap_zombies(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

void run_program(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = {(char *)path, NULL};
        execv(path, args);
        perror("Exec failed");
        exit(1);
    }
}

int main() {
    if (getpid() != 1) {
        printf("Warning: minit is intended to run as PID 1\n");
    }

    signal(SIGCHLD, reap_zombies);

    mount_fs();

    FILE *conf = fopen("/etc/minit.conf", "r");
    if (!conf) {
        perror("Failed to open /etc/minit.conf");
        while(1) sleep(3600);
    }

    char line[256];
    while (fgets(line, sizeof(line), conf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '\0' || line[0] == '#') continue;

        run_program(line);
    }

    fclose(conf);
    while (1) {
        sleep(3600);
    }

    return 0;
}
