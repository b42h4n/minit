#ifndef MINIT_H
#define MINIT_H

#ifndef CONF_PATH
#define CONF_PATH "/etc/minit.conf"
#endif
#ifndef RUN_DIR
#define RUN_DIR "/run"
#endif
#define SOCK_PATH RUN_DIR "/minit.sock"
#ifndef CGROUP_ROOT
#define CGROUP_ROOT "/sys/fs/cgroup"
#endif
#ifndef LOG_DIR
#define LOG_DIR "/var/log/minit"
#endif

#endif