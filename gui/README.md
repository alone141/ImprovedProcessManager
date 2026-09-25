# PyQt6 + ZeroMQ Process Manager GUI

A GUI for monitoring and controlling services run by a central process manager over ZeroMQ. The manager is `berayprocessmanager` in [`../manager`](../manager); the protocol between the two is [`../docs/protocol.md`](../docs/protocol.md).

## Features

- **Real-time monitoring** via PUB-SUB (data broadcast from your process manager). The toolbar pill shows whether reports are actually arriving (`Live`, `Waiting for data…`, `No data for 12s`); when they stop, the last values are greyed out
- **Master-detail layout** — the sidebar lists `berayprocessmanager` and every reported process (state dot, live CPU % or a missed-beats badge; type in the filter box to narrow it). The pane on the right shows the selection
- **Process page** — state pill, **Start / Stop / Restart** (DEALER socket, talks to the manager's ROUTER; the manager's reply, such as `restart vision: ok (restarting)` or `start ghost: unknown service`, appears in the status bar), PID / uptime / heartbeat age, six metric tiles (CPU %, memory, **GPU %** and **VRAM** from local nvidia-smi joined by PID, missed beats, restarts) and a **state band** over the last 15 minutes
- **Graphs tab** — CPU %, memory, GPU % and VRAM for the selected process over the last 1/5/15 minutes; hover to read values, tick other processes under *Compare* to overlay them, *Pop out* opens the graphs in their own window. Recording starts with the GUI (one sample a second, 15 minutes kept), so the recent past is there when you look
- **cgroup PIDs tab** — the PIDs in `task_<name>/cgroup.procs` with comm, RSS and command line; double-click one for its `journalctl _PID=` window
- **Journal tab** — live `journalctl _SYSTEMD_CGROUP=…/task_<name>`, lines coloured by PID
- **Manager page** (select `berayprocessmanager`) — live `systemctl status` + journal for `berayprocessmanager.service` (Linux)
- Thread-safe ZMQ handling (never blocks the GUI)
- Dark Fusion theme; sizes follow the system font and display scaling

## Dependencies

```bash
pip install -r requirements.txt
```

Packages: PyQt6, pyzmq only. GPU tiles use **local nvidia-smi** (must be on PATH); no Python GPU package. The GUI works without a GPU.

## GPU tiles

The GUI samples **local** NVIDIA GPU usage via **nvidia-smi** and joins by **PID** from health reports. VRAM comes from compute apps (CUDA processes in v1). GPU % is shown when `pmon` provides it; otherwise the tile shows "—". Multi-GPU usage is summed per process. If nvidia-smi is unavailable, the GPU tiles show "—" and the toolbar pill reads `GPU unavailable`.

## Manager page

On Linux hosts with systemd, the manager page (first sidebar entry) auto-refreshes every 2s:

- `systemctl status … -n 0` — unit state only (no embedded journal tail)
- `journalctl -u … -n 50 -o json` — last 50 entries; **each line colored by `_PID`** (same PID keeps the same color). Lines without a PID are gray. New entries are appended; the view follows the newest line unless you scroll up (log windows work the same way).

Unit name is fixed in v1. The GUI must run on the **same machine** as the service (local systemctl/journalctl). On Windows or without systemd, the page shows an explanatory error and the process pages still work. The sidebar shows the unit's `Active:` state next to the manager entry.

**Per-process logs:** the **Journal** tab of a process page tails cgroup `task_<processName>` (`journalctl _SYSTEMD_CGROUP=…`) with the same PID colouring. Double-click a PID on the **cgroup PIDs** tab for a window with that PID's journal; a second double-click focuses the existing window.

## Quick Start

Run these from this `gui/` directory.

```bash
# 1. Install dependencies
pip install -r requirements.txt

# 2. Start the process manager (see ../manager/README.md) ...
../manager/build/src/berayprocessmanager --config ../manager/config/services.conf

#    ... or, to try the GUI without it, the mock process manager
python mock_publisher.py

# 3. Run the GUI (defaults match both: SUB 6667, DEALER 5557)
python process_monitor_gui.py

# Or override endpoints explicitly:
python process_monitor_gui.py \
    --sub tcp://127.0.0.1:6667 \
    --dealer tcp://127.0.0.1:5557
```

GUI flags: `--sub` (health SUB), `--dealer` (command DEALER). Mock binds with `--pub` / `--router` (see `mock_publisher.py --help`).

## Standalone executable (nothing to install on the target)

`ProcessMonitor.spec` bundles the GUI with its own Python, Qt and libzmq into a folder you copy to the air-gapped machine; it needs no Python and no packages there. `MockPublisher` is built alongside, so the GUI can be tried on the target without the real manager.

```bash
# On a Linux machine with network access (same distro/release as the target is safest):
scripts/build_executable.sh
# -> dist/ProcessMonitor-linux-x86_64-glibc<version>.tar.gz
```

```powershell
# On Windows:
powershell -ExecutionPolicy Bypass -File scripts\build_executable.ps1
# -> dist\ProcessMonitor-windows-x64.zip
```

On the target: unpack and run `ProcessMonitor/ProcessMonitor --sub tcp://HOST:6667 --dealer tcp://HOST:5557` (or `ProcessMonitor.exe` on Windows).

Rules of thumb:

- **No cross-compiling.** PyInstaller bundles the interpreter and Qt of the machine that builds, so build Linux binaries on Linux and Windows binaries on Windows.
- **glibc.** A Linux build runs on any x86_64 host whose glibc is **equal or newer** than the build machine's; the script prints the version and puts it in the archive name. Build on an older release when in doubt.
- **Desktop libraries.** Qt's bundled `xcb` platform plugin still uses the host's X11/Wayland, `libxkbcommon`, `libGL`, fontconfig and freetype; any host that runs a desktop (or runs the pip-installed GUI today) has them.
- **Folder vs single file.** The default is a folder (`dist/ProcessMonitor/`). `PM_ONEFILE=1` builds single files, which unpack to a temporary directory on every start and fail on a `noexec` `/tmp`.
- **Offline build machine.** Download wheels once on a networked machine with `pip download -r requirements.txt pyinstaller -d wheels --platform manylinux2014_x86_64 --python-version 3.10 --only-binary=:all:`, then build with `WHEELS=wheels scripts/build_executable.sh`.
- **Bare python3.** A build machine whose python3 has no pip or venv (Debian/Ubuntu without `python3-pip` / `python3-venv`) works too: the script bootstraps pip with `get-pip.py` and uses `virtualenv`, no root needed. Behind a TLS-inspecting proxy it points pip at the system certificate store.
- **WSL is enough.** A WSL Ubuntu on a Windows PC produces the Linux build (verified on Ubuntu 20.04 / glibc 2.31 under WSL1). On WSL1 the script also strips Qt's "needs Linux 4.11" kernel note from `libQt6Core.so.6`, which WSL1's 4.4 kernel would otherwise refuse; that changes nothing on a real kernel. Note that the bundled PyQt6 is whatever pip resolves for the build machine's Python (Python 3.8 gets PyQt6 6.7); use Python 3.9+ there to ship the same PyQt6 you develop with.

## Architecture

```
┌──────────────────────────┐   health, PUB → SUB, 6667   ┌──────────────────────┐
│   berayprocessmanager    │ ──────────────────────────▶ │   PyQt6 GUI (this)   │
│   (../manager, C++)      │                             │                      │
│                          │   commands, DEALER "PMC"    │  • sidebar, tiles,   │
│  • runs the services     │ ◀────────────────────────── │    graphs, journal   │
│  • restarts, measures    │   replies, ROUTER → DEALER  │  • Start/Stop/Restart│
│  • publishes reports     │ ──────────────────────────▶ └──────────────────────┘
└──────────────────────────┘
```

The GUI reads the simplified health report (one 128-byte record per service)
and sends `CommandMessage`s. The manager also publishes a detailed report on
port 6668 (per-service GPU, threads, open files, I/O, exit codes, host figures),
which `berayprocessmanager --status` shows; the GUI does not read it yet.

## Protocol

`health_structs.py` holds the binary structs: `DetailedHealthReport` (the
128-byte health record; the name is historical), `CommandMessage` (65 bytes,
sent after the frame `BPM` with identity `PMC`) and `CommandReply` (128 bytes,
the manager's answer). The complete description, including the detailed report
and heartbeats, is [`../docs/protocol.md`](../docs/protocol.md).
`mock_publisher.py` speaks the same protocol for testing without the manager.

## Ports

Health `6667` (SUB), commands `5557` (DEALER); the manager's detailed report is
on `6668`. The GUI takes `--sub` and `--dealer`; the mock takes `--pub` and
`--router`.

## Next Steps

- Read the detailed report (port 6668) for GPU figures measured on the manager's
  host, so the GPU tiles work when the GUI runs elsewhere
- Offer the manager's `*` target ("start all" / "stop all") and `--reload`
- Add authentication/encryption if needed (ZMQ CURVE)
- Persist last used addresses in QSettings

## File Structure

- `process_monitor_gui.py` – Main window: ZMQ / systemd / GPU workers and their wiring to the views
- `process_views.py` – Sidebar, process page (tiles, state band, Graphs / cgroup PIDs / Journal tabs) and manager page
- `usage_graphs.py` – Usage history, charts and the pop-out graphs window (QPainter, no extra dependency)
- `journal_view.py` – PID-coloured journal text view
- `ui_scale.py` – Font- and display-relative sizing
- `systemd_logs.py` – systemctl / journalctl / cgroup helpers
- `gpu_sampler.py` – Local nvidia-smi sampling by PID
- `health_structs.py` – Binary health, command and reply structs
- `mock_publisher.py` – Mock process manager for testing
- `ProcessMonitor.spec`, `scripts/` – Standalone executable build
- `tests/` – pytest suite (`python -m pytest` from this directory)
- `requirements.txt` – Python dependencies
- `README.md` – This file
