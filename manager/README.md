# berayprocessmanager

A service supervisor in C++20. It starts the programs listed in a configuration
file, restarts them by policy, stops them in dependency order, measures CPU,
memory, I/O and GPU use, and publishes everything over ZeroMQ in the format the
GUI in `../gui` reads. The same binary is also the command-line client: it
prints the status table and starts, stops and restarts services of a running
manager.

A second program, `beraynetworkmanager`, is a ZeroMQ identity router (the
NetworkManager design brought into this tree): peers connect to it under a name
and address each other by name. With `router_endpoint` in its configuration the
manager serves commands through the router as well as directly, so clients
reach it by identity and several managers can share one router.

Linux is the primary platform; Windows is supported. The wire format is in
[`../docs/protocol.md`](../docs/protocol.md).

## Building

| Needs | Version | Ubuntu / Debian package |
|-------|---------|-------------------------|
| CMake | 3.30 or newer | `cmake` where the release has 3.30 (Ubuntu 24.04 has 3.28); otherwise Kitware's archive, below |
| C++ compiler | C++20: GCC 10, Clang 10, or Visual Studio 2019 16.10 and newer | `g++-10` on Ubuntu 20.04, `g++` from 22.04 on |
| libzmq | 4.1 or newer (built with 4.3.2) | `libzmq3-dev` |
| GoogleTest (tests only) | 1.8.1 or newer | `libgtest-dev` |

Nothing is downloaded during configure or build.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```

The build puts `berayprocessmanager` and `beraynetworkmanager` in `build/src`.
`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` turns compiler warnings into errors in the
project's own code. Without GoogleTest, configure with
`-DPROCESS_MANAGER_BUILD_TESTS=OFF`. For a
machine that has neither package, put the sources under `third_party/` on a
networked machine and copy the tree over; see
[`third_party/README.md`](third_party/README.md). A libzmq built by hand is
found with `-DZeroMQ_INCLUDE_DIR=… -DZeroMQ_LIBRARY=…` or `-DZeroMQ_DIR=…`, and
a GoogleTest install with `-DGTEST_ROOT=…`.

On Windows, from a Developer Command Prompt:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DZeroMQ_DIR=C:\libzmq\CMake
cmake --build build --config Release
```

**The toolchain on an offline machine.** Few long-term releases ship CMake 3.30.
On a networked machine, download Kitware's binary archive for the target from
cmake.org, `cmake-<version>-linux-x86_64.tar.gz` or
`cmake-<version>-windows-x86_64.zip`. It runs from wherever it is unpacked, so
put its `bin` directory first on `PATH`. Ubuntu 20.04 also needs GCC 10: on a
networked 20.04 machine, `sudo apt-get install --download-only g++-10` leaves the
packages in `/var/cache/apt/archives`. Install those `.deb` files with
`sudo dpkg -i` and configure with `-DCMAKE_CXX_COMPILER=g++-10`.

## Installing on Linux

```bash
sudo cmake --install build --prefix /usr/local
sudo mkdir -p /etc/berayprocessmanager
sudo cp config/services.conf /etc/berayprocessmanager/
sudo cp packaging/berayprocessmanager.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now berayprocessmanager
```

The unit is named `berayprocessmanager.service` because the GUI's manager page
shows that unit. It runs the manager as `Type=notify`, with `Delegate=yes` so it
can create a cgroup per service, and with `KillMode=mixed` so the manager stops
the services itself on `systemctl stop`.

The router has its own unit, for the host that should carry the commands:

```bash
sudo cp packaging/beraynetworkmanager.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now beraynetworkmanager
```

## Command line

```text
berayprocessmanager [--config FILE]              run the manager (default mode)
berayprocessmanager --status [--watch] [--wide]  table of every service
berayprocessmanager -s NAME | --start NAME       start (and what it depends on)
berayprocessmanager -k NAME | --stop NAME        stop
berayprocessmanager -r NAME | --restart NAME     restart
berayprocessmanager --reload                     re-read the configuration file
berayprocessmanager --heartbeat NAME             report that a service is alive
berayprocessmanager --check [--config FILE]      validate a configuration
```

`NAME` may be `'*'` for every service. The default configuration is
`/etc/berayprocessmanager/services.conf` (on Windows, `services.conf` next to
the executable). Client modes read the endpoints from it when it exists, so
`--status` finds a manager on custom ports; `--command` and `--report` override
them, and `--timeout MS` sets how long to wait (3000 by default).

`--router ENDPOINT` sends a command through the router instead of to the
command endpoint, to the manager named by `--manager NAME` (the configuration's
`identity`, else `berayprocessmanager`). `--status` always reads the report
socket directly.

```text
beraynetworkmanager [--bind ENDPOINT] [--log-level LEVEL]   run the router (default tcp://*:5558)
```

`--log-level debug` logs every forwarded message; the default logs where it
listens, which identities appear and go, and what it drops. It stops on
`SIGTERM` or `SIGINT` (Ctrl+C). Exit status: `0` stopped, `1` cannot listen,
`2` usage error.

Exit status: `0` done (or nothing to do), `1` refused or failed, `2` usage or
configuration error, `3` no answer from the manager.

```text
$ berayprocessmanager --status
berayprocessmanager 1.0.0 on rig-01, pid 12246, up 11s
host: CPU 16.3% of 16 cores, memory 13.2 GiB used of 15.8 GiB, load 0.52 0.58 0.59

SERVICE          STATE      PID  UPTIME  RESTARTS  CPU%     MEMORY  GPU%  VRAM  LAST EXIT
sensor_fusion    running  12249     11s         0   0.0    1.5 MiB     -     -  -
path_planner     running  12258     10s         0   0.0  832.0 KiB     -     -  -
crasher          running  12308      1s         3   0.0    1.5 MiB     -     -  exit 3, 3s ago
vision_pipeline  running  12252     11s         0   4.0    1.5 MiB     -     -  -
cpu_burner       stopped      -       -         0     -          -     -     -  -
```

`--wide` adds restart mode, threads, open files, bytes read and written,
heartbeat age and binary. `--watch` redraws the table on every report.

The manager stops on `SIGTERM` or `SIGINT` (Ctrl+C) and reloads on `SIGHUP`.
A second stop request kills every service at once.

## Configuration

An INI file: `[manager]`, an optional `[defaults]` that every service starts
from, and one `[service NAME]` per service. Names are 1 to 31 characters of
`A-Z a-z 0-9 _ . -`, so they fit the command message. Unknown keys are errors,
reported with their line; see [`config/services.conf`](config/services.conf).

`[manager]`

| Key | Default | Meaning |
|-----|---------|---------|
| `health_endpoint` | `tcp://*:6667` | simplified health report (the GUI's `--sub`) |
| `report_endpoint` | `tcp://*:6668` | detailed report (the CLI's `--status`) |
| `command_endpoint` | `tcp://*:5557` | commands (the GUI's `--dealer`) |
| `router_endpoint` | none | also serve commands through the router at this endpoint, for example `tcp://127.0.0.1:5558` |
| `identity` | `berayprocessmanager` | the manager's name on the router: 1 to 255 printable characters without spaces, unique per router |
| `publish_interval_ms` | `1000` | 100 to 60000 |
| `log_level` | `info` | `error`, `warning`, `info`, `debug` |
| `cgroups` | `auto` | `auto`, `off`, `required` |
| `gpu` | `auto` | `auto`, `off` |

`[service NAME]` (all but the first four may also go in `[defaults]`)

| Key | Default | Meaning |
|-----|---------|---------|
| `binary` | required | absolute path, a path relative to `working_dir`, or a name looked up on `PATH` |
| `args` | none | split like a shell: `'single'`, `"double \" quote"`; a backslash elsewhere is literal |
| `description` | none | shown in the detailed report |
| `depends_on` | none | services that must be running first, comma or space separated |
| `working_dir` | the manager's | an absolute path |
| `env` | none | `NAME=value`, repeatable |
| `autostart` | `true` | start with the manager |
| `restart` | `on-failure` | `never`, `on-failure` (non-zero exit or signal), `always` |
| `restart_delay_ms` | `1000` | first delay; doubled after each restart ... |
| `restart_delay_max_ms` | `30000` | ... up to this |
| `max_restarts` | `5` | automatic restarts allowed within `restart_window_s`; then `failed`; `0` = unlimited |
| `restart_window_s` | `60` | |
| `start_grace_ms` | `1000` | counts as running once up this long (without heartbeats) |
| `stop_signal` | `TERM` | `TERM`, `INT`, `HUP`, `QUIT`, `KILL`, `USR1`, `USR2` |
| `stop_timeout_ms` | `5000` | then killed |
| `heartbeat_interval_ms` | `0` (off) | the service promises a heartbeat this often |
| `heartbeat_tolerance` | `3` | missed intervals before it is unhealthy |
| `unhealthy_action` | `none` | `none` or `restart` |
| `output` | `auto` | `journal`, `inherit`, `null`, or an absolute log file (appended); `auto` = journal when the manager itself logs to journald |
| `memory_max` | none | for example `512M`; cgroup `memory.max`, job memory limit on Windows |
| `cpu_max` | none | percent of one core, for example `150%`; cgroup `cpu.max`, job CPU rate on Windows |

## Behaviour

**States.** `stopped` → `waiting` (for dependencies) → `starting` → `running`,
and `unhealthy` (heartbeats missed), `stopping`, `backoff` (a restart is
scheduled) and `failed` (it will not be restarted). The health report folds
these into the GUI's five states; the table on `docs/protocol.md` lists how.

**Dependencies.** A service waits until everything in `depends_on` runs, and
starting it starts those first. It fails instead of waiting forever when a
dependency is failed, stopped or no longer configured. Automatic restarts wait
for dependencies as well, so a crash of one service never starts the services
that need it before it is back.

**Starting.** Each service runs in a new session with standard input on
`/dev/null`, default signal handling and no blocked signals. On Linux it gets
`SIGTERM` if the manager dies, so it never outlives it, and with cgroups it
lives in `task_<name>` below the manager's own cgroup, the layout the GUI's
journal and cgroup PID views read. Output goes to its own journald stream, so
`journalctl` attributes each line to the service.

**Stopping.** The stop signal goes to the whole process group, followed by
`SIGCONT` so a stopped process acts on it; after `stop_timeout_ms` the service
is killed (the whole cgroup where there is one). When the main process exits,
whatever it left behind is killed too, and the exit only counts once those
processes are gone, so a restart never finds an old copy still holding a port.
Processes that survive `SIGKILL` for 5 s are logged and no longer waited for.
Without cgroups the leftovers are found through the service's session, which
also catches jobs that moved to a process group of their own. At shutdown,
services stop in reverse dependency order.

**Heartbeats.** Optional, per service; see the protocol document. Without them
a service counts as alive while its process runs. With `unhealthy_action =
restart`, an unhealthy service is restarted through the same backoff as a crash
and the restart counts toward `max_restarts`.

**Cgroups.** cgroup v2 only enables controllers for the children of a cgroup
without processes of its own, so the manager moves itself to a `supervisor`
leaf first. It does that only when it is alone in its cgroup, as under the
systemd unit. Started from a login shell, it leaves that session's processes
where they are: services still get their `task_<name>` groups, but
`memory_max` and `cpu_max` are not applied. In the root cgroup nothing needs to
move.

**Reload.** `--reload` or `SIGHUP` re-reads the file: new services are added
(and started when `autostart`), removed ones are stopped and dropped, changed
ones take their new settings at their next start. Endpoint, router, identity,
cgroup and GPU settings need a manager restart. A file with an error is
rejected and the running configuration stays.

**Through the router.** With `router_endpoint` set, the manager connects a
DEALER to `beraynetworkmanager` under its `identity` and serves the commands
that arrive there in the same loop as its own command socket; the reply goes
back through the router to whoever asked. The link reconnects on its own while
the router is away, and the router hands the identity over to the newest
connection when the manager restarts. The router queues nothing: a command to
a manager that is not connected is dropped with a warning in the router's log,
and the client reports no answer after its timeout. The router's default port
is 5558, so it can share a host with the manager's own command socket on 5557;
the NetworkManager project's original binary binds 5557 and needs another host.
Services get `BPM_ROUTER_ENDPOINT` and `BPM_MANAGER_IDENTITY` for heartbeats
that travel the same way. The framing is in the protocol document.

**Measuring.** Once per publish interval. CPU time, memory (resident), threads,
open files and I/O are summed over every process of the service: the cgroup
members where cgroups are used, otherwise the service's session and
descendants. CPU time includes the children each process has waited for. With
cgroups, CPU and I/O counters come from `cpu.stat` and `io.stat`, so they
include processes that already exited. I/O through a stacked device such as
LVM, dm-crypt or md is counted once, on the devices below it. GPU use comes from
NVML, loaded at run time from the NVIDIA driver; hosts without one report no
GPU figures.

**Windows.** Each service runs in a job object that is killed when the manager
exits. A stop sends Ctrl+Break to console programs that share the manager's
console and `WM_CLOSE` to windowed ones, then terminates the job after
`stop_timeout_ms`. An exit counts once the job is empty. `journal` output falls
back to the manager's own output.
There is no reload signal; use `--reload`.

## Layout

| Module | Role |
|--------|------|
| `Daemon` | the run mode: sockets, the poll loop, reload, shutdown |
| `ServiceManager`, `Service` | the services and their state machine |
| `ConfigParser`, `ServiceConfig` | the configuration file |
| `ProcessLauncher`, `NativeLauncher` | starting, stopping and measuring processes (`src/posix`, `src/windows`) |
| `CgroupTree`, `ProcFs`, `ProcessTable`, `SystemMonitor`, `GpuMonitor` | measurements |
| `HealthRecord`, `DetailedReport`, `CommandMessage`, `WireReader`, `WireWriter` | the wire format |
| `ZmqSocket`, `ReportPublisher`, `CommandServer`, `ManagerClient` | ZeroMQ |
| `MessageRouter`, `Envelope`, `RouterLink`, `RouterOptions`, `PeerAddress` | the router, its framing, the manager's link to it, its command line, and the peer address in its connection log (`src/posix`, `src/windows`) |
| `CommandLine`, `StatusTable`, `Console` | the command line |
| `Logger`, `Instant`, `SignalWatcher`, `SystemdNotifier` | support |

`process_manager_core` holds everything but ZeroMQ; `process_manager_net` adds
the sockets, the router and the daemon; `tests/` has one GoogleTest file per
module. `main.cpp` is the manager and `RouterMain.cpp` the router.

## Style notes

The code follows [`../cpp-style.md`](../cpp-style.md): C++20, with `std::span`
for every sequence a function only reads. The build asks for CMake 3.30, newer
than the guide's sample. The departures:

- Win32 sources include `<windows.h>` before the other system headers, which
  depend on it.
- Platform code sits in `src/posix` and `src/windows`, one file per module and
  platform; the few `#ifdef` blocks elsewhere say why.
