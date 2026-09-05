# minit

<p>
  <img src="https://img.shields.io/badge/C-99-A8B9CC?style=for-the-badge&logo=c&logoColor=white" alt="C99"/>
  <img src="https://img.shields.io/badge/LINUX-4.4%20%2B-FCC624?style=for-the-badge&logo=linux&logoColor=black" alt="Linux"/>
  <img src="https://img.shields.io/badge/PID-1-0A66C2?style=for-the-badge&logo=linuxcontainers&logoColor=white" alt="PID 1"/>
  <img src="https://img.shields.io/badge/SIZE-MINIMAL-CC3333?style=for-the-badge&logo=speedtest&logoColor=white" alt="Minimal"/>
  <img src="https://img.shields.io/badge/LICENSE-GPL3.0-8A2BE2?style=for-the-badge&logo=opensourceinitiative&logoColor=white" alt="License GPL"/>
</p>

**minit** is an extremely minimal init system (PID 1) for Linux. It mounts the essential virtual filesystems, launches a list of programs from a plain-text config file, and reaps zombie processes. That's it — no service supervision, no dependency graph, no control socket. Just enough to get a system booted.

## What it actually does

On startup, minit:

1. Warns (but doesn't refuse) if it isn't running as PID 1
2. Installs a `SIGCHLD` handler that reaps zombie processes via `waitpid(-1, NULL, WNOHANG)`
3. Creates and mounts `/proc` (`procfs`) and `/sys` (`sysfs`)
4. Opens `/etc/minit.conf` and runs every line as a program (`fork()` + `execv()`, no arguments)
5. Idles forever in a `sleep(3600)` loop

If `/etc/minit.conf` can't be opened, minit doesn't panic or exit — it just idles forever, doing nothing.

## Building

No build system required — a single translation unit, so a direct compiler invocation is enough.

### Dynamic build

```bash
gcc -O2 -Wall -o minit minit.c
```

### Static build

```bash
gcc -static -O2 -Wall -o minit minit.c
```

For a truly static binary with no runtime NSS/dlopen surprises, build against musl instead of glibc:

```bash
musl-gcc -static -O2 -o minit minit.c
```

Strip it down for size if needed:

```bash
strip --strip-all minit
```

## Configuration

The config is a **single plain-text file**:

```
/etc/minit.conf
```

### Format

- One absolute path to an executable per line
- Programs are run **without arguments**
- Blank lines are ignored
- Lines starting with `#` are treated as comments and ignored
- No sections, no key/value pairs, no INI syntax

Example `/etc/minit.conf`:

```Config
# Bring up networking
/bin/network

# Console
/bin/sh
```

## Installing as PID 1

Point the kernel at the compiled binary via a kernel command-line parameter:

```Config
init=/sbin/minit
```

Make sure the binary and everything it `execv()`s are reachable at boot — minit mounts `/proc` and `/sys` itself, but does **not** mount `/`, `/dev`, or anything else. Your root filesystem and any other mounts need to already be in place (e.g. handled by an initramfs) before minit runs.

## Known limitations

This is intentionally bare-bones. As of the current implementation:

- **No process supervision** — if a launched program exits or crashes, it is not restarted
- **No arguments or environment control** — every program listed in the config is exec'd with `argv = {path, NULL}` and the inherited environment
- **No logging** — child process output goes wherever their inherited file descriptors point
- **No control interface** — there is no `minitctl` or equivalent; reconfiguration requires editing `/etc/minit.conf` and rebooting or re-executing minit
- **No error recovery for a missing config** — if `/etc/minit.conf` is absent or unreadable, minit sleeps forever instead of falling back to anything
- **Only mounts `/proc` and `/sys`** — no `/dev`, `tmpfs`, or `devtmpfs` handling

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
