"""Local systemd status + journal fetch for the Service tab. No Qt."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Optional, Sequence, Tuple


SERVICE_UNIT = "berayprocessmanager.service"
CGROUP_ROOT = "/sys/fs/cgroup"
REFRESH_MS = 2000
# Number of journal *entries* (each line shown in full — no field ellipsis).
JOURNAL_LINES = 50
# Safety only for pathological dumps (UI still shows full lines within the tail).
MAX_DISPLAY_CHARS = 200_000
SUBPROCESS_TIMEOUT_SEC = 5.0


@dataclass
class JournalLine:
    pid: Optional[int]
    text: str
    raw_message: str


@dataclass
class CgroupProc:
    pid: int
    comm: str
    rss_bytes: int = 0
    cmdline: str = ""


@dataclass
class SystemdSnapshot:
    status_text: str
    journal_text: str
    journal_lines: List[JournalLine] = field(default_factory=list)
    error: Optional[str] = None


def _decode_message(msg) -> str:
    if msg is None:
        return ""
    if isinstance(msg, str):
        return msg
    if isinstance(msg, list):
        # A field that appears more than once in an entry arrives as a list of strings.
        if msg and all(isinstance(part, str) for part in msg):
            return " | ".join(msg)
        try:
            return bytes(int(b) & 0xFF for b in msg).decode("utf-8", errors="replace")
        except (TypeError, ValueError):
            return str(msg)
    return str(msg)


def _format_ts(entry: dict) -> str:
    """Match journalctl -o short-iso (local time, no tz suffix if local)."""
    raw = entry.get("__REALTIME_TIMESTAMP")
    try:
        us = int(raw)
        dt = datetime.fromtimestamp(us / 1_000_000.0)
        return dt.strftime("%Y-%m-%dT%H:%M:%S")
    except (TypeError, ValueError, OSError):
        return "?"


def _format_short_iso(entry: dict, pid: Optional[int], message: str) -> str:
    """Reproduce journalctl -o short-iso:
    2019-10-14T15:22:03 hostname ident[pid]: message
    """
    ts = _format_ts(entry)
    host = str(entry.get("_HOSTNAME") or "").strip()
    ident = str(
        entry.get("SYSLOG_IDENTIFIER")
        or entry.get("_COMM")
        or "unknown"
    ).strip()
    parts = [ts]
    if host:
        parts.append(host)
    if pid is not None:
        parts.append(f"{ident}[{pid}]:")
    else:
        parts.append(f"{ident}:")
    parts.append(message)
    return " ".join(parts)


def parse_journal_json(text: str) -> List[JournalLine]:
    """Parse newline-delimited journalctl -o json records."""
    lines: List[JournalLine] = []
    for raw in (text or "").splitlines():
        raw = raw.strip()
        if not raw:
            continue
        try:
            entry = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if not isinstance(entry, dict):
            continue
        pid: Optional[int] = None
        pid_raw = entry.get("_PID")
        if pid_raw is not None and str(pid_raw).strip() != "":
            try:
                pid = int(pid_raw)
            except (TypeError, ValueError):
                pid = None
        message = _decode_message(entry.get("MESSAGE"))
        display = _format_short_iso(entry, pid, message)
        lines.append(JournalLine(pid=pid, text=display, raw_message=message))
    return lines


def _which(name: str) -> Optional[str]:
    return shutil.which(name)


def _run(exe: str, args: Sequence[str]) -> tuple[int, str, str]:
    kwargs = {
        "capture_output": True,
        "text": True,
        "encoding": "utf-8",
        "errors": "replace",
        "timeout": SUBPROCESS_TIMEOUT_SEC,
        "shell": False,
        "check": False,
        "env": {
            **os.environ,
            "SYSTEMD_COLORS": "0",
            "SYSTEMD_PAGER": "",
            "PAGER": "cat",
        },
    }
    if sys.platform == "win32":
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        cp = subprocess.run([exe, *args], **kwargs)
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {SUBPROCESS_TIMEOUT_SEC}s: {exe} {' '.join(args)}"
    except OSError as e:
        return 127, "", str(e)
    return cp.returncode, cp.stdout or "", cp.stderr or ""


def _clamp_text(text: str, max_chars: int = MAX_DISPLAY_CHARS) -> str:
    """Keep only the tail of the text (most recent log lines tend to be at the end)."""
    if not text or len(text) <= max_chars:
        return text
    omitted = len(text) - max_chars
    return f"… [{omitted} earlier characters omitted] …\n" + text[-max_chars:]


def fetch_systemctl_status(unit: str = SERVICE_UNIT) -> tuple[str, Optional[str]]:
    """Return (stdout_or_combined, error_message_or_None).

    Uses ``-n 0`` so status does **not** embed a journal tail (that lives in
    the journal pane only). Avoids doubling huge log dumps in the UI.
    """
    exe = _which("systemctl")
    if not exe:
        return "", "systemctl not found on PATH (Service tab requires Linux systemd)"
    # -n 0: no journal lines in status; -l: full unit lines (not truncated properties)
    code, out, err = _run(exe, ["status", unit, "--no-pager", "-l", "-n", "0"])
    text = out if out.strip() else err
    if not text.strip() and code != 0:
        return "", err.strip() or f"systemctl status failed (exit {code})"
    # systemctl status returns non-zero when inactive/failed — still useful text
    return _clamp_text(text), None


def task_cgroup_name(process_name: str) -> str:
    return f"task_{process_name}"


# Hybrid hosts (Ubuntu 20.04, RHEL 8) mount cgroup v2 at /sys/fs/cgroup/unified
# and systemd's v1 hierarchy at /sys/fs/cgroup/systemd. journald records the
# path inside the hierarchy, so the mount directory is not part of it.
HIERARCHY_DIRS = ("unified", "systemd")


def journal_cgroup_filter(sysfs_path: str, root: str = CGROUP_ROOT) -> str:
    """Turn /sys/fs/cgroup/system.slice/task_foo (or .../unified/system.slice/task_foo)
    into /system.slice/task_foo, the form of journald's _SYSTEMD_CGROUP."""
    root_n = root.replace("\\", "/").rstrip("/")
    path_n = sysfs_path.replace("\\", "/")
    if path_n == root_n or path_n.startswith(root_n + "/"):
        rest = path_n[len(root_n) :] or "/"
        for directory in HIERARCHY_DIRS:
            prefix = "/" + directory
            if rest == prefix or rest.startswith(prefix + "/"):
                rest = rest[len(prefix) :] or "/"
                break
        return rest
    return path_n if path_n.startswith("/") else "/" + path_n


def index_task_cgroups(root: str = CGROUP_ROOT) -> dict:
    """Map processName -> sysfs path for every task_<name> directory."""
    found = {}
    if not os.path.isdir(root):
        return found
    prefix = "task_"
    try:
        for dirpath, _dirnames, _files in os.walk(root):
            try:
                base = os.path.basename(dirpath)
            except OSError:
                continue
            if base.startswith(prefix) and len(base) > len(prefix):
                found[base[len(prefix) :]] = dirpath
    except OSError:
        return found
    return found


def find_task_cgroup_sysfs(
    process_name: str, root: str = CGROUP_ROOT
) -> Optional[str]:
    want = task_cgroup_name(process_name)
    if not process_name or not os.path.isdir(root):
        return None
    try:
        for dirpath, _dirnames, _files in os.walk(root):
            try:
                if os.path.basename(dirpath) == want:
                    return dirpath
            except OSError:
                continue
    except OSError:
        return None
    return None


def find_task_cgroup(
    process_name: str, root: str = CGROUP_ROOT
) -> Optional[str]:
    """Return journald _SYSTEMD_CGROUP path for task_<processName>, or None."""
    sysfs = find_task_cgroup_sysfs(process_name, root=root)
    if not sysfs:
        return None
    return journal_cgroup_filter(sysfs, root)


def _read_pids_file(path: str) -> List[int]:
    pids: List[int] = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    pids.append(int(line))
                except ValueError:
                    continue
    except OSError:
        return []
    return pids


def list_cgroup_pids(sysfs_path: str) -> List[int]:
    """PIDs in this cgroup (v2 cgroup.procs, else v1 tasks)."""
    if not sysfs_path:
        return []
    for fname in ("cgroup.procs", "tasks"):
        pids = _read_pids_file(os.path.join(sysfs_path, fname))
        if pids:
            return pids
    return []


def _proc_comm(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/comm", "r", encoding="utf-8", errors="replace") as f:
            return f.read().strip() or str(pid)
    except OSError:
        return str(pid)


def _proc_cmdline(pid: int) -> str:
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            raw = f.read().replace(b"\x00", b" ").strip()
            return raw.decode("utf-8", errors="replace")
    except OSError:
        return ""


def _proc_rss_bytes(pid: int) -> int:
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    if len(parts) >= 2:
                        return int(parts[1]) * 1024
    except (OSError, ValueError):
        return 0
    return 0


def list_cgroup_procs(sysfs_path: str) -> List[CgroupProc]:
    out: List[CgroupProc] = []
    for pid in list_cgroup_pids(sysfs_path):
        out.append(
            CgroupProc(
                pid=pid,
                comm=_proc_comm(pid),
                rss_bytes=_proc_rss_bytes(pid),
                cmdline=_proc_cmdline(pid),
            )
        )
    out.sort(key=lambda p: p.pid)
    return out


def snapshot_cgroup_members(
    process_names: Sequence[str], root: str = CGROUP_ROOT
) -> dict:
    """processName -> list[CgroupProc] from task_<name> cgroups."""
    index = index_task_cgroups(root)
    result = {}
    for name in process_names:
        sysfs = index.get(name)
        result[name] = list_cgroup_procs(sysfs) if sysfs else []
    return result


def _journal_result(code: int, out: str, err: str) -> Tuple[List[JournalLine], Optional[str]]:
    """Parsed lines, or journalctl's explanation when there are none. It exits 0 with
    "No journal files were found." or a hint about missing permissions."""
    if code != 0 and not out.strip():
        return [], err.strip() or f"journalctl failed (exit {code})"
    parsed = parse_journal_json(out)
    if not parsed and err.strip():
        return [], err.strip()
    return parsed, None


def fetch_journal_pid(
    pid: int, lines: int = JOURNAL_LINES
) -> Tuple[List[JournalLine], Optional[str]]:
    exe = _which("journalctl")
    if not exe:
        return [], "journalctl not found on PATH"
    code, out, err = _run(
        exe,
        # -b: PIDs are reused across boots; --all: long lines are not dropped.
        [f"_PID={int(pid)}", "-b", "-n", str(lines), "--all", "--no-pager", "-o", "json"],
    )
    return _journal_result(code, out, err)


def fetch_journal_cgroup(
    cgroup_path: str, lines: int = JOURNAL_LINES
) -> Tuple[List[JournalLine], Optional[str]]:
    exe = _which("journalctl")
    if not exe:
        return [], "journalctl not found on PATH"
    code, out, err = _run(
        exe,
        [
            f"_SYSTEMD_CGROUP={cgroup_path}",
            "-n",
            str(lines),
            "--all",
            "--no-pager",
            "-o",
            "json",
        ],
    )
    return _journal_result(code, out, err)


def fetch_journal(
    unit: str = SERVICE_UNIT, lines: int = JOURNAL_LINES
) -> Tuple[List[JournalLine], Optional[str]]:
    """Fetch recent journal records as structured lines (`journalctl -o json`)."""
    exe = _which("journalctl")
    if not exe:
        return [], "journalctl not found on PATH"
    code, out, err = _run(
        exe,
        ["-u", unit, "-n", str(lines), "--all", "--no-pager", "-o", "json"],
    )
    return _journal_result(code, out, err)


def fetch_snapshot(unit: str = SERVICE_UNIT, lines: int = JOURNAL_LINES) -> SystemdSnapshot:
    """Fetch both status and journal. Partial success is allowed."""
    if sys.platform == "win32":
        msg = (
            "Service logs require Linux systemd (systemctl/journalctl).\n"
            f"This host is {sys.platform}."
        )
        return SystemdSnapshot(
            status_text=msg, journal_text=msg, journal_lines=[], error=msg
        )

    status, status_err = fetch_systemctl_status(unit)
    jlines, journal_err = fetch_journal(unit, lines)
    journal_text = "\n".join(ln.text for ln in jlines)

    errors: List[str] = []
    if status_err:
        errors.append(status_err)
        if not status:
            status = status_err
    if journal_err:
        errors.append(journal_err)
        if not journal_text:
            journal_text = journal_err

    err = "; ".join(errors) if errors else None
    return SystemdSnapshot(
        status_text=status,
        journal_text=_clamp_text(journal_text),
        journal_lines=jlines,
        error=err,
    )
