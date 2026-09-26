# Process manager and monitor

[![CI](https://github.com/alone141/ImprovedProcessManager/actions/workflows/ci.yml/badge.svg)](https://github.com/alone141/ImprovedProcessManager/actions/workflows/ci.yml)

Programs that talk over ZeroMQ:

| Directory | Program | Built with |
|-----------|---------|------------|
| [`manager/berayprocessmanager/`](manager/berayprocessmanager/) | `berayprocessmanager`: starts the services listed in a configuration file, restarts them by policy, measures CPU, memory, I/O and GPU use, and publishes it all. The same binary is the command-line client (`--status`, `--start`, `--stop`, `--restart`). | C++20, CMake |
| [`manager/beraynetworkmanager/`](manager/beraynetworkmanager/) | `beraynetworkmanager`: a ZeroMQ identity router. Peers connect under a name and address each other by name; with `router_endpoint` configured, the manager serves commands through it as well, so several managers can share one router and clients reach them by identity. | C++20, CMake |
| [`gui/`](gui/) | The health monitor: one window with every service, its figures and details from the manager, graphs, cgroup processes and journal, a host overview, and Start / Stop / Restart buttons. | Python 3, PyQt6 |

The contract between them is [`docs/protocol.md`](docs/protocol.md). The manager
keeps the GUI's original interface (a 128-byte health record per service on
port 6667, commands on 5557) and adds a detailed report on port 6668, replies to
commands, heartbeats and reload. The GUI reads both reports. Commands can also
travel through the router; the reports stay on the manager's own sockets.

```text
 berayprocessmanager                          GUI (gui/process_monitor_gui.py)
 ┌──────────────────────────┐   health  6667  ┌──────────────────────────────┐
 │ services from            │ ──────────────▶ │ sidebar, tiles, graphs,      │
 │ services.conf            │   PUB → SUB     │ state band, journal          │
 │                          │                 │                              │
 │ ROUTER 5557              │ ◀────────────── │ Start / Stop / Restart       │
 │                          │   DEALER "PMC"  └──────────────────────────────┘
 │ PUB 6668 (detailed)      │ ──────────────▶  the GUI's details and host overview,
 │                          │                  berayprocessmanager --status (client mode)
 │ DEALER "berayprocess-    │   commands by   ┌──────────────────────────────┐
 │         manager"         │ ◀─────────────▶ │ beraynetworkmanager          │ ◀── berayprocessmanager --stop NAME
 └──────────────────────────┘   identity      │ ROUTER 5558                  │     --router tcp://HOST:5558
                                              └──────────────────────────────┘
```

## Quick start (one Linux host)

```bash
# Build both programs (needs CMake 3.30+, g++ 10+, libzmq3-dev; see manager/README.md)
cmake -S manager -B manager/build -DPROCESS_MANAGER_BUILD_TESTS=OFF
cmake --build manager/build -j

# Run it with the example configuration (edit the binaries first)
manager/build/bin/berayprocessmanager --config manager/config/services.conf

# In another terminal: the table, and a command
manager/build/bin/berayprocessmanager --status --config manager/config/services.conf
manager/build/bin/berayprocessmanager --restart sensor_fusion

# Optional: the router, and a command through it (needs router_endpoint in services.conf)
manager/build/bin/beraynetworkmanager
manager/build/bin/berayprocessmanager --restart sensor_fusion --router tcp://127.0.0.1:5558

# The GUI
cd gui && pip install -r requirements.txt && python process_monitor_gui.py
```

Both programs build for machines without network access: the manager from
distribution packages or sources placed under `manager/third_party/`, the GUI
as a standalone executable (`gui/scripts/build_executable.sh`).

## Continuous integration

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push to
`main` and every pull request. The build still downloads nothing; each job
installs packages or unpacks sources first, the way a networked machine
prepares an offline one.

| Job | Checks |
|-----|--------|
| `manager-linux` | Ubuntu 24.04 with `libzmq3-dev` and `libgtest-dev`, warnings as errors, every test, and `--version` / `--check` on the example configuration |
| `manager-gcc10` | the compiler floor: GCC 10 with Kitware's CMake 3.30 archive |
| `manager-vendored` | the offline path: libzmq and GoogleTest sources under `manager/third_party/`, built with CMake 4 |
| `manager-windows` | MSVC with the vendored libzmq and GoogleTest, every test |
| `gui-tests` | `pytest` for `gui/` on Ubuntu and Windows with Python 3.10, the version the standalone executable is built with |
