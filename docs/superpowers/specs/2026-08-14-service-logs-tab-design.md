# Service Logs Tab (berayprocessmanager.service)

**Date:** 2026-08-14  
**Status:** Approved for implementation planning  
**Scope:** PyQt6 GUI only; local Linux systemd

## Problem

Operators use the Process Manager GUI for process health but still leave the app to run `systemctl status` / journalctl for the process manager unit itself. They want a **live tab** for `berayprocessmanager.service` status and recent logs.

## Goals

1. Add a **second tab** next to the process table: **Processes** | **Service**.
2. Service tab shows:
   - **Status block:** full text of `systemctl status berayprocessmanager.service` (no pager).
   - **Journal block:** recent lines from `journalctl -u berayprocessmanager.service` (last **200** lines).
3. **Auto-refresh every 2 seconds** while the app runs (poll, not `journalctl -f`).
4. Work when GUI runs on the **same Linux host** as the service (local `systemctl` / `journalctl`).
5. Unit name **fixed:** `berayprocessmanager.service` (v1).

## Non-goals (v1)

- Windows service logs / Event Log
- SSH/remote systemctl
- Editable unit name in UI
- Streaming `journalctl -f` long-lived process
- Filtering, search, export
- sudo password prompts (run commands as the GUI user; show stderr on failure)

## Decisions

| Topic | Decision |
|-------|----------|
| Layout | `QTabWidget`: Processes (existing), Service (new) |
| Content | Status + journal (200 lines) |
| Unit | Fixed `berayprocessmanager.service` |
| Host | Same machine; subprocess |
| Refresh | Poll every **2 s** |
| Threading | Fetch off UI thread (QThread worker or QThreadPool); update text via signals |
| Commands | `systemctl status … --no-pager -l`; `journalctl -u … -n 200 --no-pager -o short-iso` |

## Architecture

```
┌─ ProcessMonitorWindow ─────────────────────────────────────┐
│  Toolbar (endpoints, GPU, connection)                       │
│  ┌─ QTabWidget ──────────────────────────────────────────┐ │
│  │ [Processes]  │  [Service]                             │ │
│  │   table…     │  QTextEdit status (read-only)          │ │
│  │              │  QTextEdit journal (read-only)         │ │
│  └──────────────┴────────────────────────────────────────┘ │
│         ▲ signals                                          │
│  SystemdLogWorker (QThread) ── subprocess systemctl/journalctl
└────────────────────────────────────────────────────────────┘
```

## Components

### 1. UI changes (`process_monitor_gui.py`)

- Wrap existing central process table (+ bottom bar if present) in tab **"Processes"**.
- Tab **"Service"**:
  - Header label: unit name `berayprocessmanager.service`
  - Read-only monospace `QPlainTextEdit` or `QTextEdit` for **status**
  - Read-only monospace view for **journal**
  - Optional small “Last refresh: HH:MM:SS” / error line
- Dark theme: match existing dark palette (dark base, light text).
- Auto-scroll journal to bottom on update **only if** user was already near bottom (optional polish; v1 may always snap to bottom).

**v1 scroll policy:** Always scroll journal to end on refresh (simplest).

### 2. `SystemdLogWorker` (in `process_monitor_gui.py` or `systemd_log_worker.py`)

**Responsibility:** Run systemctl/journalctl periodically; emit text.

**Surface:**

- Signals: `status_text(str)`, `journal_text(str)`, `error(str)`, optionally `refreshed()`
- `start()` / `stop()` on a `QThread`
- Loop every 2 s while running:
  1. `systemctl status berayprocessmanager.service --no-pager -l`
  2. `journalctl -u berayprocessmanager.service -n 200 --no-pager -o short-iso`
  3. Emit both (or emit error if both fail)
- Subprocess: `shell=False`, timeout ~5 s, capture text; Windows: if ever run, fail gracefully (feature is Linux-only).
- Do not require root; if permission denied, show command stderr in the error/status area.

**Constants:**

```text
SERVICE_UNIT = "berayprocessmanager.service"
REFRESH_MS = 2000
JOURNAL_LINES = 200
```

### 3. Lifecycle

- Start worker after UI build (or when Service tab first shown — **v1: start at window open** so status is warm).
- Stop worker in `closeEvent` with existing ZMQ/GPU cleanup.
- Processes tab behavior unchanged (ZMQ, GPU timer).

## Error handling

| Situation | Behavior |
|-----------|----------|
| Not Linux / systemctl missing | Show clear message in Service tab; keep polling or stop after first fail with sticky error |
| Unit not found / inactive | Still show `systemctl status` output (systemd includes useful text) |
| Timeout / non-zero exit | Display stderr/stdout in the relevant pane; do not crash |
| journal empty | Empty journal pane is OK |

## Testing

1. **Manual on Linux:** unit installed → status + journal update every ~2 s.
2. **Manual unit stopped:** status shows inactive; journal still has lines.
3. **Manual without systemctl:** tab shows error text; Processes tab still works.
4. No unit tests required for subprocess integration in v1 (optional later with mocked Popen).

## Success criteria

1. User can switch to **Service** tab and see live status + last 200 journal lines for `berayprocessmanager.service`.
2. Content refreshes ~every 2 s without freezing the Processes table (fetch off UI thread).
3. Processes / ZMQ / GPU features remain unchanged.
4. Graceful message when systemctl/journalctl unavailable.

## Implementation notes

- Prefer small new module `systemd_logs.py` for run helpers + constants so GUI stays thinner.
- `env={"SYSTEMD_COLORS": "0", "SYSTEMD_PAGER": ""}` or `LC_ALL=C` to avoid ANSI/pager issues.
- `systemctl` may need full path `/usr/bin/systemctl` via `shutil.which`.
