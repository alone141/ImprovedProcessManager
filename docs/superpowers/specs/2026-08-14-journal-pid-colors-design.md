# Journal: color by PID (one stream)

**Date:** 2026-08-14  
**Status:** Approved for implementation planning  
**Slice:** Service / journal ops — PID-colored log stream only

## Problem

The Service tab journal is a single monochrome dump. Operators cannot tell which process wrote which line when `berayprocessmanager.service` (and its children) share the journal.

## Goals

1. Keep **one** journal stream (not split panes).
2. Color each line by **`_PID`** from journald (stable color per PID for the session).
3. Lines **without** `_PID` use a default gray.
4. Status pane (`systemctl status -n 0`) stays unchanged.
5. Still last **50** entries, refresh ~2 s, same unit `berayprocessmanager.service`.

## Non-goals (this spec)

- Editable unit name, pause/copy/export, error-only highlighting
- Split panes / tabs per PID
- Matching lines to the Processes table by name
- Changing ZMQ / GPU / process table

## Decisions

| Topic | Decision |
|-------|----------|
| Layout | One journal view |
| Color key | `_PID` from journalctl JSON |
| Missing PID | Gray (`#888888`) |
| Fetch | `journalctl -u UNIT -n 50 --no-pager --full -o json` |
| UI | `QTextEdit` (rich text / extra selections) so lines can differ in color |
| Legend | None in this spec |

## Architecture

```
journalctl -o json  →  parse entries {_PID, MESSAGE, timestamp}
                    →  SystemdSnapshot.journal_lines: list[JournalLine]
                    →  GUI paints one QTextEdit, color per PID
```

## Components

### 1. `systemd_logs.py`

Add:

```text
@dataclass
class JournalLine:
    pid: Optional[int]
    text: str          # display line: "2026-08-14T… pid=1234  message"
    raw_message: str
```

- `fetch_journal` uses **`-o json`** (newline-delimited JSON objects, one per record).
- Parse `__REALTIME_TIMESTAMP` (µs) or `SYSLOG_TIMESTAMP` if present; format a short ISO-like prefix.
- Read `_PID` as int when present; else `pid=None`.
- `MESSAGE` may be str or list of bytes (journald); decode to text.
- `fetch_snapshot` carries `journal_lines: list[JournalLine]` **and** a plain `journal_text` fallback (joined lines) for errors/Windows.
- Keep `JOURNAL_LINES = 50`, `--full` not needed with JSON (full MESSAGE field).
- Existing `encoding=utf-8, errors=replace` stays.

**Parser unit tests** (string fixtures, no journalctl):

- Two PIDs → two lines with correct pid
- Missing `_PID` → `pid is None`
- Empty / bad JSON line skipped
- MESSAGE as list of ints (journald style) still decodes

### 2. GUI (`process_monitor_gui.py`)

- Replace journal `QPlainTextEdit` with `QTextEdit` (read-only).
- Worker still emits snapshot; extend signal to pass structured lines **or** emit journal as a list (JSON-serializable tuples) to stay Qt-signal friendly:

  Preferred: keep `snapshot_ready(str, str, str)` for status/error and add  
  `journal_lines_ready(list)` where each item is `(pid_or_0, text)` and `0` means no PID.

- Color map: `dict[int, QColor]` assigned on first sight of a PID. Palette: distinct hues on dark background (e.g. cycle 8–10 colors: cyan, amber, light green, pink, sky, gold, orchid, coral). Same PID always same color until app restart.
- Paint: clear document, append each line with that color + newline. Wrap at widget width; full message text.
- Skip full repaint if the list of `(pid, text)` equals last painted (same as today’s “skip if unchanged”).
- Status pane: still `QPlainTextEdit`, no colors.

### 3. Worker

`SystemdLogWorker`: after `fetch_snapshot`, emit status text, structured journal lines, error. Wrap fetch in try/except (already required).

## Error handling

| Situation | Behavior |
|-----------|----------|
| journalctl missing / fail | Error string in journal area; no crash |
| Partial JSON | Skip bad records; show the rest |
| Windows | Existing “needs systemd” message, no colors |

## Testing

- Unit tests for JSON parse / PID / missing PID / bad line.
- Manual on Linux: two PIDs in journal show two colors; unknown-pid lines gray.

## Success criteria

1. Journal is one stream; lines with `_PID` are colored by PID.
2. Same PID keeps the same color during a session.
3. Full message text is visible (wrapped).
4. Status + Processes + GPU/ZMQ unchanged.

## Open items resolved

- Slice: Service/journal
- User: color by PID in one stream
- PID source: journal `_PID`
- Approach A: JSON + QTextEdit
- No legend in this spec
