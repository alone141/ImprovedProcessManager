# Process tree view (cgroups)

**Date:** 2026-08-19  
**Status:** Approved for implementation

## Goal

Replace the flat process table with a `QTreeWidget`: root `berayprocessmanager`, children = managed processes (`task_<name>`). Same columns, GPU, Start/Stop/Restart on children only, double-click child opens log window.

## Decisions

| Topic | Choice |
|-------|--------|
| Widget | `QTreeWidget` replacing `QTableWidget` |
| Root | `berayprocessmanager` — display only, no command buttons |
| Children | All processes from health reports (not filtered by cgroup existence) |
| Cgroup | Naming `task_<processName>` for logs; tree grouping is conceptual |
| Actions | Child rows only |
| Double-click | Child → existing log window; ignore root and Actions column |

## Non-goals

- Full `/sys/fs/cgroup` walk as UI tree
- Nested task-in-task
- Start/Stop on the systemd unit from the root row
