# Process manager and monitor

Two programs that talk over ZeroMQ:

| Directory | Program | Built with |
|-----------|---------|------------|
| [`manager/`](manager/) | `berayprocessmanager`: starts the services listed in a configuration file, restarts them by policy, measures CPU, memory, I/O and GPU use, and publishes it all. The same binary is the command-line client (`--status`, `--start`, `--stop`, `--restart`). | C++20, CMake |
| [`gui/`](gui/) | The health monitor: one window with every service, its graphs, cgroup processes and journal, and Start / Stop / Restart buttons. | Python 3, PyQt6 |

The contract between them is [`docs/protocol.md`](docs/protocol.md). The manager
keeps the GUI's original interface (a 128-byte health record per service on
port 6667, commands on 5557) and adds a detailed report on port 6668, replies to
commands, heartbeats and reload.

```text
 berayprocessmanager                          GUI (gui/process_monitor_gui.py)
 ┌──────────────────────────┐   health  6667  ┌──────────────────────────────┐
 │ services from            │ ──────────────▶ │ sidebar, tiles, graphs,      │
 │ services.conf            │   PUB → SUB     │ state band, journal          │
 │                          │                 │                              │
 │ ROUTER 5557              │ ◀────────────── │ Start / Stop / Restart       │
 │                          │   DEALER "PMC"  └──────────────────────────────┘
 │ PUB 6668 (detailed)      │ ──────────────▶  berayprocessmanager --status
 └──────────────────────────┘                  (same binary, client mode)
```

## Quick start (one Linux host)

```bash
# Build the manager (needs CMake 3.30+, g++ 10+, libzmq3-dev; see manager/README.md)
cmake -S manager -B manager/build -DPROCESS_MANAGER_BUILD_TESTS=OFF
cmake --build manager/build -j

# Run it with the example configuration (edit the binaries first)
manager/build/src/berayprocessmanager --config manager/config/services.conf

# In another terminal: the table, and a command
manager/build/src/berayprocessmanager --status --config manager/config/services.conf
manager/build/src/berayprocessmanager --restart sensor_fusion

# The GUI
cd gui && pip install -r requirements.txt && python process_monitor_gui.py
```

Both programs build for machines without network access: the manager from
distribution packages or sources placed under `manager/third_party/`, the GUI
as a standalone executable (`gui/scripts/build_executable.sh`).
