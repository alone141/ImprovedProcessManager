# Master-detail redesign

**Date:** 2026-09-24
**Status:** Implemented

## Goal

Replace the tree table + Service tab + popup windows with one window: a
sidebar of processes and a detail pane for the selection. Same wire
protocol, same workers, same pure logic; only the view layer changes.
Constraint: nothing new to install on the air-gapped target, so the view
stays on PyQt6 Widgets (Qt Quick and the browser route were considered
and are documented in the session notes, not adopted).

## Layout

| Region | Content |
|--------|---------|
| Toolbar | Title, feed pill (`Live` / `Waiting for data…` / `No data for Ns`), endpoint summary, GPU pill, **Connection** toggle |
| Connection strip | SUB / DEALER fields + Reconnect; hidden until toggled, shown automatically when connecting fails |
| Sidebar | Filter box; **Manager** entry (`berayprocessmanager`, unit `Active:` state at the right); **Processes · N**, one row per report with state dot, name and CPU % / state word / missed-beats badge |
| Process page | Name, state pill, Start / Stop / Restart; meta line (PID, uptime, heartbeat age, snapshot clock); six tiles (CPU, memory, GPU, VRAM, missed beats, restarts); state band over the last 15 min; tabs **Graphs** / **cgroup PIDs · N** / **Journal**, with **Pop out** in the tab corner |
| Manager page | systemctl status + unit journal (the former Service tab) |
| Status bar | Last log message, process count, last update time |

### Graphs tab

The four `UsageChart`s from `usage_graphs.py`, for the selected process.
Span segments 1 / 5 / 15 min; **Compare** chips overlay other processes
that have samples in view, each in its own colour; hover reads every
visible series. Charts only redraw while the tab is visible. **Pop out**
opens the existing `UsageGraphWindow`.

### State band

`UsageSample` gained an optional `state`. `state_segments()` turns the
recorded samples into runs of one state; a gap longer than 5 s leaves a
hole, so a process that stopped reporting is visible as such.

## Decisions

| Topic | Choice |
|-------|--------|
| Toolkit | PyQt6 Widgets (guaranteed present where the current GUI runs) |
| Data flow | Main window keeps `_current`, `_gpu_by_pid`, `_cgroup_members`, `UsageHistory`; pages are fed explicitly (`set_process`, `update_report`, `set_members`, `set_journal`, `tick`) |
| Journal tail | One `DetailJournalWorker` whose target follows the selection; a change wakes it immediately |
| PID journals | Still `ProcessLogWindow` (double-click on the cgroup PIDs tab) |
| Stale feed | Sidebar rows and page values turn grey; the band fades |
| Selection on start | Manager page; the first report batch selects the first process unless the user already clicked |
| Vanished process | If the shown process leaves the reports, the sidebar falls back to the manager entry |
| Short windows | The process page scrolls instead of squeezing the charts until their labels clip; default 1180×760, minimum 900×600 (scaled with the UI font, capped to the screen) |
| Wide content | Compare chips wrap (`FlowLayout`); the endpoint text elides (`ElidedLabel`) so the toolbar never overflows |

## Modules

- `process_views.py` — `ProcessSidebar`, `ProcessDetailPage` (`MetricTile`, `StatePill`, `StateBand`, `GraphsPanel`, `CgroupPidsTable`), `ServiceDetailPage`, pure text helpers
- `journal_view.py` — `JournalView`, `new_journal_entries`, PID colours (moved out of the main module; re-exported there)
- `ui_scale.py` — `ui_font`, `ui_point_size`, `fit_to_screen` (moved; re-exported)
- `process_monitor_gui.py` — workers, `FeedMonitor`, metrics, `ProcessMonitorWindow`

## Tests

Pure-logic tests are untouched. View tests moved from the tree
(`test_tree_layout.py`, `test_stale_rows.py`) to `test_sidebar.py`,
`test_detail_page.py`, `test_state_band.py` and `test_window_sizing.py`;
the systemctl scroll-position test targets `ServiceDetailPage`.
