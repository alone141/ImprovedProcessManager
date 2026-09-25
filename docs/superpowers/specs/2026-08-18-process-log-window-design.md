# Per-process log window (double-click)

**Date:** 2026-08-18  
**Status:** Approved for implementation planning

## Problem

Operators can see the manager unit journal on the Service tab, but not logs for a **single** managed process. Each process runs in a cgroup named `task_<processName>`. They want a dedicated log view for that process.

## Goals

1. **Double-click** a row on the Processes table.
2. Open a **new window** titled with that process name.
3. Show recent journal lines for the cgroup **`task_<processName>`**.
4. Find the cgroup as a path under `/sys/fs/cgroup` whose last component is `task_<processName>`.
5. Fetch with `journalctl _SYSTEMD_CGROUP=<full-path> -n 50 --no-pager -o json`.
6. Same **PID coloring** and short-iso-style lines as the Service tab.
7. Auto-refresh every **2 s**.
8. If a window for that process is already open, **raise it** (do not spawn a second).

## Non-goals

- Editing the cgroup naming scheme
- PID-only fallback (this spec is cgroup-only)
- Multiple windows for the same process
- Changing ZMQ / Start-Stop / GPU
- Changing the Service tab (manager unit)

## Decisions

| Topic | Decision |
|-------|----------|
| Trigger | Double-click process table row |
| Window | Separate `QDialog` or small `QMainWindow` |
| Cgroup | Last path component == `task_<processName>` under `/sys/fs/cgroup` |
| Journal | `_SYSTEMD_CGROUP=<resolved path>` |
| Lines | 50 |
| Colors | Reuse Service-tab PID palette / paint helper |
| Duplicate | Raise existing window for that name |

## Architecture

```
double-click row "sensor_fusion"
        │
        ▼
find /sys/fs/cgroup/**/task_sensor_fusion
        │
        ▼
journalctl _SYSTEMD_CGROUP=<that path> -n 50 -o json
        │
        ▼
parse_journal_json → ProcessLogWindow (QTextEdit, PID colors)
        refresh 2s
```

## Components

### 1. Cgroup resolve (`systemd_logs.py` or small helper)

```text
def find_task_cgroup(process_name: str) -> str | None
    # Walk /sys/fs/cgroup (follow dirs, skip permission errors)
    # Match basename == "task_" + process_name
    # Return first match as the path journald expects
    # (typically the path as stored in _SYSTEMD_CGROUP, e.g.
    #  /system.slice/…/task_sensor_fusion  or the sysfs path
    #  converted to the cgroup v2 path starting at /)
```

**Path for journalctl:** Use the cgroup path relative to the cgroup root, starting with `/` (journald `_SYSTEMD_CGROUP` is usually `/system.slice/foo` or `/user.slice/…/task_name`). If sysfs path is `/sys/fs/cgroup/system.slice/task_foo`, strip the `/sys/fs/cgroup` prefix so the filter is `/system.slice/task_foo`.

If not found: window still opens; body shows a clear error (`cgroup task_<name> not found under /sys/fs/cgroup`).

### 2. Fetch

Extend `systemd_logs.py`:

```text
def fetch_journal_cgroup(cgroup_path: str, lines: int = 50) -> list[JournalLine]
    journalctl _SYSTEMD_CGROUP=<cgroup_path> -n <lines> --no-pager -o json
```

Reuse `parse_journal_json`. Same encoding / timeout rules as existing fetch.

### 3. `ProcessLogWindow`

- Title: `Logs — <processName>` (subtitle: `cgroup task_<name>` + resolved path).
- Read-only `QTextEdit`, wrap, PID colors (own map or shared helper).
- Worker or QTimer + background fetch (do **not** block UI). Prefer a small worker loop like `SystemdLogWorker` scoped to this window; stop on close.
- Close: stop worker, remove from the parent’s open-window dict.

### 4. Table hook (`ProcessMonitorWindow`)

- `table.cellDoubleClicked` / `itemDoubleClicked` → process name from column 0 (or row key).
- Ignore double-click on the Actions column (buttons).
- `self._log_windows: dict[str, ProcessLogWindow]`
- If name in dict and window still valid: `raise_()`, `activateWindow()`.
- Else: create, `show()`, store; `destroyed` / `finished` pops the dict.

## Error handling

| Situation | Behavior |
|-----------|----------|
| Not Linux / no `/sys/fs/cgroup` | Window: “cgroup logs require Linux” |
| Cgroup not found | Window open, error text, keep retrying resolve each tick (process may start later) |
| journalctl fail | Show stderr in the window |
| Process name empty | Ignore double-click |

## Testing

- Unit: `find_task_cgroup` with a fake tree (temp dirs `task_foo`).
- Unit: path prefix strip `/sys/fs/cgroup/system.slice/task_foo` → `/system.slice/task_foo`.
- Manual: double-click running process; window shows logs; second double-click focuses same window.

## Success criteria

1. Double-click opens (or focuses) a log window for that process.
2. Logs come from cgroup `task_<processName>`.
3. Lines are full text, colored by `_PID`.
4. Processes table / Service tab / commands unchanged.

## Open items resolved

- Trigger: double-click, new window
- Source: cgroup `task_<processName>`
- Resolve: walk `/sys/fs/cgroup`, match basename
- Duplicate windows: reuse/raise
