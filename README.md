# minit

<p>
  <img src="https://img.shields.io/badge/C-99-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C99"/>
  <img src="https://img.shields.io/badge/LINUX-4.4%20%2B-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="Linux"/>
  <img src="https://img.shields.io/badge/PID-1-0A66C2?style=for-the-badge&logo=linuxcontainers&logoColor=white" alt="PID 1"/>
  <img src="https://img.shields.io/badge/SIZE-MINIMAL-CC3333?style=for-the-badge&logo=speedtest&logoColor=white" alt="Minimal"/>
  <img src="https://img.shields.io/badge/LICENSE-GPL3.0-8A2BE2?style=for-the-badge&logo=opensourceinitiative&logoColor=white" alt="License GPL"/>
</p>

**minit** is a minimal init system (PID 1) for Linux. It mounts the essential virtual filesystems, launches a list of programs from a plain-text config file, restarts them if they die, captures their output into log files, and reaps zombie processes. A small companion tool, `minitctl`, lets you inspect and control services at runtime. No dependency graph, no scripting language, no D-Bus: just enough to get a system booted and keep it running.

## What it actually does

On startup, minit:

1. Warns (but doesn't refuse) if it isn't running as PID 1
2. Sets a default `PATH` if the environment doesn't provide one
3. Mounts `/dev` (`devtmpfs`, falling back to `tmpfs` with a minimal set of device nodes), `/dev/pts`, `/dev/shm`, `/proc`, `/sys` and a `tmpfs` on `/run`
4. Creates `/var/log/minit`
5. Reads `/etc/minit.conf`. **If the file can't be opened, minit panics** (prints the reason and exits; as PID 1 this triggers a kernel panic)
6. Starts every service from the config (`fork()` + `execvp()`, arguments and `PATH` lookup supported), each in its own session with stdout/stderr redirected to its log file
7. Enters an event loop (`signalfd` + `poll`) that:
   - reaps every child, including orphans re-parented to PID 1
   - restarts services that exited, up to their configured limit
   - serves `minitctl` requests on a UNIX socket

## Building

No build system required: two translation units, so a direct compiler invocation is enough.

### Dynamic build

```bash
gcc -O2 -Wall -o src/minit minit.c
gcc -O2 -Wall -o src/minitctl minitctl.c
```

### Static build

```bash
gcc -static -O2 -Wall -o src/minit minit.c
gcc -static -O2 -Wall -o src/minitctl minitctl.c
```

For a truly static binary with no runtime NSS/dlopen surprises, build against musl instead of glibc:

```bash
musl-gcc -static -O2 -o src/minit minit.c
musl-gcc -static -O2 -o src/minitctl minitctl.c
```

Strip them down for size if needed:

```bash
strip --strip-all minit minitctl
```

Paths (config file, socket, log directory) are defined in `minit.h` and can be overridden at compile time, e.g. `-DCONF_PATH='"/etc/my.conf"'`.

## Configuration

The config is a **single plain-text file**:

```
/etc/minit.conf
```

### Format

- One service per line: a command followed by its arguments
- The command is looked up via `PATH`, so an absolute path is not required
- Arguments are split on whitespace; use double quotes to keep spaces inside one argument
- Blank lines are ignored
- Lines starting with `#` are comments
- `restarts = N` sets how many times a service is restarted after it exits. It applies to every service **below** that line and can be changed again further down (default is `0`, i.e. never restart)
- An optional `name:` prefix gives the service a name for `minitctl` and for its log file. Without it, the name is the basename of the command; duplicates get a `-2`, `-3`, ... suffix

Example `/etc/minit.conf`:

```
# Restart these up to 3 times if they die
restarts = 3
syslogd
getty1: getty -L tty1 115200 vt100

# Run once, never restart
restarts = 0
network: ip link set lo up
shell: sh -c "echo hello"
```

### Restart behaviour

- A service is restarted after **any** exit (success or failure), with a 1 second delay between attempts
- When the limit is reached, the service is marked `failed` and left alone
- `minitctl restart` / `start` resets the restart counter

## Logs

Each service's stdout and stderr are appended to:

```
/var/log/minit/<name>.log
```

minit also writes its own events there, prefixed with `[minit]` (start, exit status, restart attempts, giving up). stdin is `/dev/null`.

Logs are never rotated or truncated. Point `/var/log` at a `tmpfs` if the root filesystem is small or read-only.

## minitctl

```
minitctl status [name]
minitctl start   <name>
minitctl stop    <name>
minitctl restart <name>
minitctl log     <name> [lines]     # default: 50
```

Example output:

```
$ minitctl status
syslogd          running  pid=41     restarts=0/3 last=- up=312s
getty1           running  pid=42     restarts=1/3 last=signal 9 up=17s
network          stopped  pid=0      restarts=0/0 last=exit 0
```

States: `running`, `backoff` (waiting to be restarted), `stopped`, `failed` (restart limit reached).

`minitctl` talks to minit over `/run/minit.sock` (mode `0600`, root only). `log` reads the log file directly.

## Installing as PID 1

Point the kernel at the compiled binary via a kernel command-line parameter:

```
init=/sbin/minit
```

Install `minitctl` anywhere in `PATH` (e.g. `/sbin/minitctl`). Make sure the binaries and everything minit launches are reachable at boot. minit mounts `/dev`, `/proc`, `/sys` and `/run` itself, but does **not** mount `/` or anything else, so your root filesystem and any other mounts need to already be in place (e.g. handled by an initramfs) before minit runs.

`/dev` requires `CONFIG_DEVTMPFS` in the kernel for a full device tree. Without it, minit falls back to a `tmpfs` with only `null`, `zero`, `full`, `random`, `urandom`, `tty` and `console`; you would need `mdev`/`udev` or extra `mknod` calls for anything else.

## Known limitations

- **No shutdown/reboot handling**: minit does not react to `SIGTERM`/`SIGINT`/`SIGUSR1`, and there is no `poweroff`/`reboot` command
- **Stopping uses SIGTERM only**: a service that ignores it will not be killed (no SIGKILL escalation)

## Contributing

Pull requests are welcome! Before submitting a PR:

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the GPL v3.0 License. See the [LICENSE](./LICENSE) file for details.

---

If you run into any issues or have questions, please open an issue in this repository.
