"""Master-detail views for the health monitor.

The sidebar lists the manager unit and one row per reported process. The
process detail page shows the selection: a header with state and actions,
metric tiles, a state band, and Graphs / cgroup PIDs / Journal tabs. The
service page shows the manager unit (systemctl status + journal).

Pure widgets: no threads, sockets or subprocesses. The main window feeds
them data and reacts to their signals.
"""

from __future__ import annotations

import bisect
import html
import re
import time
from typing import Dict, List, Optional, Sequence, Tuple

from PyQt6.QtCore import QPoint, QPointF, QRect, QRectF, QSize, Qt, pyqtSignal
from PyQt6.QtGui import (
    QColor,
    QFont,
    QFontMetrics,
    QPainter,
    QPainterPath,
    QPalette,
    QTextOption,
)
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QButtonGroup,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLayout,
    QListWidget,
    QListWidgetItem,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QStyle,
    QStyledItemDelegate,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from health_structs import (
    ALL_SERVICES,
    RESTART_MODE_TEXT,
    CommandEnum,
    RuntimeState,
    ServiceState,
    exit_text,
    format_bytes,
    format_duration_ns,
    format_duration_short,
    service_state_text,
)
from journal_view import JournalView
from systemd_logs import SERVICE_UNIT, task_cgroup_name
from ui_scale import MONO_FONT_FAMILIES, ui_font, ui_point_size
from usage_graphs import (
    METRICS,
    SERIES_COLORS,
    UsageChart,
    UsageHistory,
    build_series,
    state_segments,
)

MANAGER_NAME = "berayprocessmanager"

# ── theme ─────────────────────────────────────────────────────────────────────

ACCENT = "#3a7bd5"
BG = "#1e1e1e"
SIDEBAR_BG = "#242424"
ROW_SELECTED_BG = "#2f2f2f"
ROW_HOVER_BG = "#292929"
TILE_BG = "#2a2a2a"
BORDER = "#333333"
LOG_BG = "#1a1a1a"
LOG_FG = "#c8e6c9"
TEXT = "#dddddd"
TEXT_DIM = "#aaaaaa"
TEXT_MUTED = "#888888"
DANGER = "#f44336"
STALE_COLOR = QColor("#777777")

STATE_COLORS: Dict[RuntimeState, str] = {
    RuntimeState.UNKNOWN: "#9c27b0",
    RuntimeState.STARTING: "#ff9800",
    RuntimeState.RUNNING: "#4caf50",
    RuntimeState.STOPPED: "#9e9e9e",
    RuntimeState.UNHEALTHY: "#f44336",
}
STATE_LABELS: Dict[RuntimeState, str] = {
    RuntimeState.UNKNOWN: "Unknown",
    RuntimeState.STARTING: "Starting",
    RuntimeState.RUNNING: "Running",
    RuntimeState.STOPPED: "Stopped",
    RuntimeState.UNHEALTHY: "Unhealthy",
}
# States with a live process, where an uptime means something.
ALIVE_STATES = (RuntimeState.STARTING, RuntimeState.RUNNING, RuntimeState.UNHEALTHY)
# Which command makes sense in which state.
ACTION_STATES: Dict[CommandEnum, Tuple[RuntimeState, ...]] = {
    CommandEnum.START: (RuntimeState.STOPPED, RuntimeState.UNKNOWN, RuntimeState.UNHEALTHY),
    CommandEnum.STOP: (RuntimeState.RUNNING, RuntimeState.STARTING, RuntimeState.UNHEALTHY),
    CommandEnum.RESTART: (
        RuntimeState.RUNNING,
        RuntimeState.STARTING,
        RuntimeState.UNHEALTHY,
        RuntimeState.STOPPED,
    ),
}


def _action_style(normal: str, hover: str) -> str:
    return (
        f"QPushButton {{ background: {normal}; color: white; border: none; "
        "border-radius: 3px; padding: 4px 10px; font-weight: bold; }"
        f"QPushButton:hover {{ background: {hover}; }}"
        "QPushButton:disabled { background: #444; color: #888; }"
    )


ACTION_STYLES: Dict[CommandEnum, str] = {
    CommandEnum.START: _action_style("#2e7d32", "#388e3c"),
    CommandEnum.STOP: _action_style("#c62828", "#d32f2f"),
    CommandEnum.RESTART: _action_style("#ef6c00", "#fb8c00"),
    CommandEnum.RELOAD: _action_style("#37474f", "#455a64"),
}

# The manager page's whole-manager actions: label, tooltip.
MANAGER_ACTIONS: Tuple[Tuple[CommandEnum, str, str], ...] = (
    (CommandEnum.START, "Start all", "Start every service, each after what it depends on"),
    (CommandEnum.STOP, "Stop all", "Stop every service, dependents first"),
    (CommandEnum.RESTART, "Restart all", "Restart every service"),
    (
        CommandEnum.RELOAD,
        "Reload configuration",
        "Re-read the configuration file: new services are added (and started when autostart), "
        "removed ones are stopped, changed ones take their settings at their next start",
    ),
)

TAB_STYLE = f"""
    QTabWidget::pane {{ border: 1px solid {BORDER}; background: {BG}; }}
    QTabBar::tab {{
        background: #2d2d2d; color: {TEXT_DIM};
        padding: 6px 16px; margin-right: 2px;
        border-top-left-radius: 4px; border-top-right-radius: 4px;
    }}
    QTabBar::tab:selected {{ background: {BG}; color: {TEXT}; font-weight: bold; }}
    QTabBar::tab:hover {{ color: #fff; }}
"""
LOG_EDIT_STYLE = (
    f"QTextEdit {{ background: {LOG_BG}; color: {LOG_FG}; "
    f"font-family: {MONO_FONT_FAMILIES}; border: 1px solid {BORDER}; }}"
)
STATUS_EDIT_STYLE = (
    f"QPlainTextEdit {{ background: {LOG_BG}; color: {TEXT}; "
    f"font-family: {MONO_FONT_FAMILIES}; border: 1px solid {BORDER}; }}"
)
TABLE_STYLE = f"""
    QTableWidget {{ background: {BG}; color: {TEXT}; border: none; }}
    QTableWidget::item {{ padding: 2px 4px; }}
    QTableWidget::item:selected {{ background: #3a5f8a; }}
    QHeaderView::section {{
        background: #2d2d2d; color: {TEXT_DIM}; padding: 4px 6px;
        border: none; font-weight: bold;
    }}
"""
SMALL_BUTTON_STYLE = f"""
    QPushButton {{
        background: #37474f; color: white; border: none;
        border-radius: 3px; padding: 3px 10px; font-weight: bold;
    }}
    QPushButton:hover {{ background: #455a64; }}
    QPushButton:pressed {{ background: #263238; }}
"""


def mono_style(color: str) -> str:
    return f"color: {color}; font-family: {MONO_FONT_FAMILIES};"


# ── pure helpers ─────────────────────────────────────────────────────────────


def state_color(state: RuntimeState) -> str:
    return STATE_COLORS.get(state, "#ffffff")


def tint(color: str, alpha: float = 0.18) -> str:
    """A translucent version of ``color`` for style sheets."""
    c = QColor(color)
    return f"rgba({c.red()}, {c.green()}, {c.blue()}, {int(alpha * 255)})"


def pill_style(color: str) -> str:
    radius = max(6, int(ui_point_size() * 0.9))
    return (
        f"QLabel {{ color: {color}; background: {tint(color)}; "
        f"border-radius: {radius}px; padding: 1px {radius}px; font-weight: bold; }}"
    )


def segment_style(first: bool, last: bool) -> str:
    radius = "4px"
    corners = (
        f"border-top-left-radius: {radius if first else '0'}; "
        f"border-bottom-left-radius: {radius if first else '0'}; "
        f"border-top-right-radius: {radius if last else '0'}; "
        f"border-bottom-right-radius: {radius if last else '0'};"
    )
    return (
        f"QPushButton {{ background: {TILE_BG}; color: {TEXT_DIM}; border: 1px solid #444; "
        f"padding: 2px 10px; {corners} }}"
        f"QPushButton:checked {{ background: {ACCENT}; color: white; border-color: {ACCENT}; }}"
        f"QPushButton:hover {{ color: {TEXT}; }}"
    )


def action_enabled(command: CommandEnum, state: RuntimeState) -> bool:
    return state in ACTION_STATES[command]


def uptime_text(state: RuntimeState, start_ns: int, snap_ns: int) -> str:
    # A stopped process keeps its last start time; that isn't an uptime.
    if state not in ALIVE_STATES or not start_ns:
        return "—"
    return format_duration_ns(snap_ns - start_ns)


def last_seen_text(report: dict) -> str:
    snap = report.get("snapshotTime", 0)
    last = report.get("lastSeen", 0)
    if not last:
        return "never"  # the manager has not seen it run yet
    return format_duration_ns(snap - last) + " ago"


def snapshot_clock(snap_ns: int) -> str:
    if snap_ns <= 0:
        return "—"
    return time.strftime("%H:%M:%S", time.localtime(snap_ns / 1e9))


def meta_text(report: dict) -> str:
    """The line under the process name: PID, uptime, heartbeat age, snapshot."""
    pid = report.get("pid") or 0
    snap = report.get("snapshotTime", 0)
    return " · ".join(
        (
            f"PID {pid}" if pid else "PID —",
            f"up {uptime_text(report['state'], report.get('start_time', 0), snap)}",
            f"last seen {last_seen_text(report)}",
            f"snapshot {snapshot_clock(snap)}",
        )
    )


def cpu_text(report: dict) -> str:
    cpu = report.get("_cpu_pct")
    return "—" if cpu is None else f"{cpu:.1f} %"


def gpu_texts(gpu) -> Tuple[str, str]:
    """(GPU %, VRAM) texts for a GpuProcessUsage, or dashes without one."""
    if gpu is None:
        return "—", "—"
    util = "—" if gpu.util_pct is None else f"{gpu.util_pct:.1f} %"
    vram = "—" if gpu.vram_bytes is None else format_bytes(gpu.vram_bytes)
    return util, vram


def row_hint(report: dict) -> Tuple[str, str]:
    """Right-hand text of a sidebar row and its kind: "cpu", "badge" or "muted"."""
    state = report["state"]
    if state == RuntimeState.UNHEALTHY:
        missed = int(report.get("missedBeats", 0) or 0)
        return (f"{missed} missed" if missed else "unhealthy"), "badge"
    if state == RuntimeState.RUNNING:
        cpu = report.get("_cpu_pct")
        return ("—" if cpu is None else f"{cpu:.0f} %"), "cpu"
    return STATE_LABELS.get(state, state.name).lower(), "muted"


def service_active_text(status_text: str) -> str:
    """The unit's active state ("active", "failed", …) from systemctl status."""
    match = re.search(r"^\s*Active:\s*(\S+)", status_text or "", re.MULTILINE)
    return match.group(1) if match else ""


# ── the detailed report, as text ─────────────────────────────────────────────

NO_REPORT_TEXT = (
    "No detailed report from the manager (port 6668): the tiles show the health record only"
)
WAITING_REPORT_TEXT = "Waiting for the manager's detailed report (port 6668)…"

# Rows of the process page's details panel, in display order. A value the
# report does not have hides its row.
DETAIL_ROWS = (
    ("state", "Manager state"),
    ("binary", "Binary"),
    ("description", "Description"),
    ("restart", "Restart policy"),
    ("exit", "Last exit"),
    ("next", "Next restart"),
    ("procs", "Processes · threads · files"),
    ("io", "I/O"),
    ("peak", "Peak memory"),
    ("limits", "Limits"),
    ("accounting", "Accounting"),
    ("heartbeat", "Heartbeat"),
    ("gpu", "GPU (manager)"),
)

# Rows of the manager page's host overview.
HOST_ROWS = (
    ("host", "Host"),
    ("manager", "Manager"),
    ("publish", "Publishing"),
    ("accounting", "Accounting"),
    ("cpu", "CPU"),
    ("memory", "Memory"),
    ("load", "Load"),
    ("uptime", "Host uptime"),
    ("services", "Services"),
    ("gpus", "GPUs"),
)


def ago_text(snapshot_ns: int, when_ns: int) -> str:
    return format_duration_short(snapshot_ns - when_ns) + " ago"


def details_values(record: dict, report: dict) -> Dict[str, str]:
    """The details panel's rows for one service record of the detailed report."""
    snapshot = int(report.get("snapshotTime", 0) or 0)
    windows = bool(report.get("windows"))
    values: Dict[str, str] = {"state": service_state_text(record, snapshot)}
    values["binary"] = record.get("binary") or "—"
    if record.get("description"):
        values["description"] = record["description"]
    policy = RESTART_MODE_TEXT.get(record.get("restartMode"), "—")
    values["restart"] = policy + (" · autostart" if record.get("autostart") else "")
    exit_time = int(record.get("lastExitTime", 0) or 0)
    if exit_time:
        values["exit"] = f"{exit_text(int(record.get('lastExitCode', 0)), windows)}, {ago_text(snapshot, exit_time)}"
    else:
        values["exit"] = "never"
    next_restart = int(record.get("nextRestartTime", 0) or 0)
    if next_restart > snapshot:
        values["next"] = "in " + format_duration_short(next_restart - snapshot)
    if record.get("usageValid"):
        files = record.get("openFiles")
        values["procs"] = (
            f"{record.get('processCount', 0)} · {record.get('threadCount', 0)} · "
            f"{'—' if files is None else files}"
        )
        values["io"] = (
            f"read {format_bytes(int(record.get('ioReadBytes', 0)))} · "
            f"written {format_bytes(int(record.get('ioWriteBytes', 0)))}"
        )
        values["peak"] = format_bytes(int(record.get("memoryPeakBytes", 0)))
    limits = []
    if record.get("memoryLimitBytes"):
        limits.append("memory " + format_bytes(int(record["memoryLimitBytes"])))
    if record.get("cpuLimitPercent"):
        limits.append(f"CPU {record['cpuLimitPercent']} % of one core")
    values["limits"] = " · ".join(limits) if limits else "none"
    if record.get("cgroup"):
        accounting = "cgroup " + task_cgroup_name(record.get("name", ""))
        if record.get("oomKills"):
            accounting += f" · {record['oomKills']} OOM kills"
        values["accounting"] = accounting
    else:
        values["accounting"] = "session and descendants"
    if record.get("heartbeat"):
        beat = "supervised"
        last_seen = int(record.get("lastSeen", 0) or 0)
        if last_seen and record.get("state") in (ServiceState.RUNNING, ServiceState.UNHEALTHY):
            beat += " · last beat " + ago_text(snapshot, last_seen)
        if record.get("missedBeats"):
            beat += f" · {record['missedBeats']} missed"
        values["heartbeat"] = beat
    else:
        values["heartbeat"] = "not supervised"
    if record.get("gpuValid"):
        gpu = record.get("gpuPercent")
        values["gpu"] = (
            ("—" if gpu is None else f"{gpu:.1f} %")
            + " · " + format_bytes(int(record.get("gpuMemoryBytes", 0)))
        )
    elif report.get("gpuMonitoring"):
        values["gpu"] = "no GPU figures for this service"
    return values


def host_values(report: dict) -> Dict[str, str]:
    """The host overview's rows for a detailed report's header and GPU records."""
    snapshot = int(report.get("snapshotTime", 0) or 0)
    values: Dict[str, str] = {"host": report.get("hostName") or "?"}
    manager = f"v{report.get('managerVersion') or '?'} · PID {report.get('managerPid', 0)}"
    started = int(report.get("managerStartTime", 0) or 0)
    if started:
        manager += " · up " + format_duration_short(snapshot - started)
    if report.get("stopping"):
        manager += " · shutting down"
    values["manager"] = manager
    interval = int(report.get("publishIntervalMs", 0) or 0)
    values["publish"] = (
        (f"every {interval / 1000:.1f} s" if interval else "interval unknown")
        + f" · snapshot {snapshot_clock(snapshot)}"
    )
    accounting = "cgroups" if report.get("cgroups") else "sessions (no cgroups)"
    accounting += " · GPU monitoring (NVML)" if report.get("gpuMonitoring") else " · no GPU monitoring"
    if report.get("windows"):
        accounting += " · Windows host"
    values["accounting"] = accounting
    cpu = report.get("hostCpuPercent")
    cores = int(report.get("cpuCount", 0) or 0)
    values["cpu"] = ("—" if cpu is None else f"{cpu:.1f} %") + (f" of {cores} cores" if cores else "")
    total = int(report.get("memoryTotalBytes", 0) or 0)
    available = int(report.get("memoryAvailableBytes", 0) or 0)
    used = total - available if total > available else 0
    values["memory"] = f"{format_bytes(used)} used of {format_bytes(total)}" if total else "—"
    load = tuple(report.get("loadAverage") or ())
    if len(load) == 3 and (any(load) or not report.get("windows")):
        values["load"] = " ".join(f"{value:.2f}" for value in load)
    uptime = int(report.get("uptimeSeconds", 0) or 0)
    if uptime:
        values["uptime"] = format_duration_short(uptime * 1_000_000_000)
    services = report.get("services") or []
    counts: Dict[str, int] = {}
    for service in services:
        state = service.get("state")
        key = state.name.lower() if isinstance(state, ServiceState) else str(state)
        counts[key] = counts.get(key, 0) + 1
    summary = str(len(services))
    if counts:
        summary += " · " + " · ".join(f"{key} {count}" for key, count in sorted(counts.items()))
    values["services"] = summary
    gpus = report.get("gpus") or []
    if gpus:
        lines = []
        for gpu in gpus:
            util = gpu.get("utilizationPercent")
            line = (
                f"gpu{gpu.get('index', 0)} · {gpu.get('name') or '?'} · "
                f"{'—' if util is None else f'{util:.0f} %'} busy · "
                f"{format_bytes(int(gpu.get('memoryUsedBytes', 0)))} of "
                f"{format_bytes(int(gpu.get('memoryTotalBytes', 0)))}"
            )
            if gpu.get("temperatureC"):
                line += f" · {gpu['temperatureC']} °C"
            if gpu.get("powerMilliwatts"):
                line += f" · {gpu['powerMilliwatts'] / 1000:.0f} W"
            lines.append(line)
        values["gpus"] = "\n".join(lines)
    elif report.get("gpuMonitoring"):
        values["gpus"] = "none found"
    return values


# ── sidebar ──────────────────────────────────────────────────────────────────

KIND_ROLE = Qt.ItemDataRole.UserRole  # "header" | "manager" | "svc"
REPORT_ROLE = Qt.ItemDataRole.UserRole + 1  # svc rows: the health report dict
HINT_ROLE = Qt.ItemDataRole.UserRole + 2  # manager row: right-hand text


class SidebarDelegate(QStyledItemDelegate):
    """Paints sidebar rows: a state dot, the name, and a CPU figure, state
    word or missed-beats badge at the right. Section headers are small
    muted text. While the feed is stale everything is grey."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.stale = False

    def sizeHint(self, option, index) -> QSize:
        height = option.fontMetrics.height()
        factor = 1.8 if index.data(KIND_ROLE) == "header" else 2.1
        return QSize(0, int(height * factor))

    def paint(self, painter: QPainter, option, index) -> None:
        kind = index.data(KIND_ROLE)
        rect = QRectF(option.rect)
        fm = option.fontMetrics
        pad = fm.averageCharWidth()
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if kind == "header":
            font = QFont(option.font)
            font.setPointSizeF(font.pointSizeF() * 0.9)
            painter.setFont(font)
            painter.setPen(QColor(TEXT_MUTED))
            painter.drawText(
                rect.adjusted(pad * 1.5, 0, -pad, -pad * 0.4),
                Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignBottom,
                index.data(Qt.ItemDataRole.DisplayRole),
            )
            painter.restore()
            return

        if option.state & QStyle.StateFlag.State_Selected:
            painter.fillRect(rect, QColor(ROW_SELECTED_BG))
            painter.fillRect(QRectF(rect.left(), rect.top(), 2, rect.height()), QColor(ACCENT))
        elif option.state & QStyle.StateFlag.State_MouseOver:
            painter.fillRect(rect, QColor(ROW_HOVER_BG))

        if kind == "manager":
            dot = QColor(ACCENT)
            hint, hint_kind = (index.data(HINT_ROLE) or ""), "muted"
            name_color = QColor(TEXT)
        else:
            report = index.data(REPORT_ROLE) or {}
            state = report.get("state", RuntimeState.UNKNOWN)
            dot = QColor(state_color(state))
            hint, hint_kind = row_hint(report) if report else ("", "muted")
            name_color = QColor(TEXT_DIM if state == RuntimeState.STOPPED else TEXT)
        if self.stale:
            dot, name_color, hint_kind = STALE_COLOR, STALE_COLOR, "muted"

        radius = fm.height() * 0.28
        cx = rect.left() + pad * 2 + radius
        cy = rect.center().y()
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(dot)
        painter.drawEllipse(QPointF(cx, cy), radius, radius)

        right = rect.right() - pad * 1.5
        hint_w = 0.0
        if hint:
            hint_font = QFont(option.font)
            hint_font.setPointSizeF(hint_font.pointSizeF() * 0.9)
            painter.setFont(hint_font)
            hfm = QFontMetrics(hint_font)
            text_w = hfm.horizontalAdvance(hint)
            if hint_kind == "badge":
                badge_h = hfm.height() + 2
                hint_w = text_w + pad * 1.6
                badge = QRectF(right - hint_w, cy - badge_h / 2, hint_w, badge_h)
                fill = QColor(DANGER)
                fill.setAlpha(56)
                painter.setBrush(fill)
                painter.drawRoundedRect(badge, badge_h / 2, badge_h / 2)
                painter.setPen(QColor(DANGER))
                painter.drawText(badge, Qt.AlignmentFlag.AlignCenter, hint)
            else:
                hint_w = text_w
                if self.stale:
                    painter.setPen(STALE_COLOR)
                else:
                    painter.setPen(QColor(TEXT_MUTED if hint_kind == "muted" else TEXT_DIM))
                painter.drawText(
                    QRectF(right - text_w, rect.top(), text_w, rect.height()),
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                    hint,
                )

        painter.setFont(option.font)
        painter.setPen(name_color)
        x0 = cx + radius + pad
        avail = max(0.0, right - hint_w - pad - x0)
        text = fm.elidedText(
            index.data(Qt.ItemDataRole.DisplayRole), Qt.TextElideMode.ElideMiddle, int(avail)
        )
        painter.drawText(
            QRectF(x0, rect.top(), avail, rect.height()),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            text,
        )
        painter.restore()


class ProcessSidebar(QListWidget):
    """The manager unit, then one row per reported process, sorted by name."""

    manager_selected = pyqtSignal()
    process_selected = pyqtSignal(str)

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._delegate = SidebarDelegate(self)
        self.setItemDelegate(self._delegate)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setMouseTracking(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setStyleSheet(f"QListWidget {{ background: {SIDEBAR_BG}; border: none; outline: 0; }}")
        self._add_header("Manager")
        self._manager_item = QListWidgetItem(MANAGER_NAME)
        self._manager_item.setData(KIND_ROLE, "manager")
        self.addItem(self._manager_item)
        self._process_header = self._add_header("Processes")
        self._items: Dict[str, QListWidgetItem] = {}
        self._filter = ""
        self.currentItemChanged.connect(self._on_current_changed)

    def _add_header(self, text: str) -> QListWidgetItem:
        item = QListWidgetItem(text)
        item.setFlags(Qt.ItemFlag.NoItemFlags)
        item.setData(KIND_ROLE, "header")
        self.addItem(item)
        return item

    # ── data ──────────────────────────────────────────────────────────────

    def set_processes(self, reports: Dict[str, dict]) -> None:
        """One row per report, in name order; rows of gone processes leave."""
        names = sorted(reports)
        for gone in set(self._items) - set(names):
            self.takeItem(self.row(self._items.pop(gone)))
        for name in names:
            item = self._items.get(name)
            if item is None:
                item = QListWidgetItem(name)
                item.setData(KIND_ROLE, "svc")
                listed = sorted(self._items)
                position = self.row(self._process_header) + 1 + bisect.bisect(listed, name)
                self.insertItem(position, item)
                self._items[name] = item
            item.setData(REPORT_ROLE, reports[name])
            item.setHidden(not self._matches(name))
        self._process_header.setText(f"Processes · {len(names)}")
        self.viewport().update()

    def header_text(self) -> str:
        return self._process_header.text()

    def process_names(self) -> List[str]:
        return [
            self.item(i).text()
            for i in range(self.count())
            if self.item(i).data(KIND_ROLE) == "svc"
        ]

    def set_filter(self, text: str) -> None:
        self._filter = text.strip().lower()
        for name, item in self._items.items():
            item.setHidden(not self._matches(name))

    def _matches(self, name: str) -> bool:
        return self._filter in name.lower()

    def is_hidden(self, name: str) -> bool:
        return self._items[name].isHidden()

    def set_stale(self, stale: bool) -> None:
        if self._delegate.stale != stale:
            self._delegate.stale = stale
            self.viewport().update()

    def set_manager_hint(self, text: str) -> None:
        self._manager_item.setData(HINT_ROLE, text)

    # ── selection ─────────────────────────────────────────────────────────

    def select_manager(self) -> None:
        self.setCurrentItem(self._manager_item)

    def select_process(self, name: str) -> None:
        item = self._items.get(name)
        if item is not None:
            self.setCurrentItem(item)

    def selected_process(self) -> Optional[str]:
        current = self.currentItem()
        if current is not None and current.data(KIND_ROLE) == "svc":
            return current.text()
        return None

    def _on_current_changed(self, current, _previous) -> None:
        if current is None:
            return
        kind = current.data(KIND_ROLE)
        if kind == "manager":
            self.manager_selected.emit()
        elif kind == "svc":
            self.process_selected.emit(current.text())


# ── generic widgets ──────────────────────────────────────────────────────────


class FlowLayout(QLayout):
    """Lays widgets out left to right and wraps them onto new rows, like
    words in a paragraph (a port of Qt's flow layout example)."""

    def __init__(
        self, parent: Optional[QWidget] = None, h_spacing: int = 6, v_spacing: int = 6
    ):
        super().__init__(parent)
        self._items: List = []
        self._h_spacing = h_spacing
        self._v_spacing = v_spacing
        self.setContentsMargins(0, 0, 0, 0)

    def addItem(self, item) -> None:
        self._items.append(item)

    def count(self) -> int:
        return len(self._items)

    def itemAt(self, index: int):
        return self._items[index] if 0 <= index < len(self._items) else None

    def takeAt(self, index: int):
        return self._items.pop(index) if 0 <= index < len(self._items) else None

    def expandingDirections(self):
        return Qt.Orientation.Horizontal

    def hasHeightForWidth(self) -> bool:
        return True

    def heightForWidth(self, width: int) -> int:
        return self._arrange(QRect(0, 0, width, 0), test_only=True)

    def setGeometry(self, rect: QRect) -> None:
        super().setGeometry(rect)
        self._arrange(rect, test_only=False)

    def sizeHint(self) -> QSize:
        return self.minimumSize()

    def minimumSize(self) -> QSize:
        size = QSize()
        for item in self._items:
            size = size.expandedTo(item.minimumSize())
        margins = self.contentsMargins()
        return size + QSize(
            margins.left() + margins.right(), margins.top() + margins.bottom()
        )

    def _arrange(self, rect: QRect, test_only: bool) -> int:
        margins = self.contentsMargins()
        area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom())
        x, y = area.x(), area.y()
        row_height = 0
        for item in self._items:
            if item.isEmpty():  # a hidden widget takes no room
                continue
            hint = item.sizeHint()
            if x + hint.width() > area.right() + 1 and row_height > 0:
                x = area.x()
                y += row_height + self._v_spacing
                row_height = 0
            if not test_only:
                item.setGeometry(QRect(QPoint(x, y), hint))
            x += hint.width() + self._h_spacing
            row_height = max(row_height, hint.height())
        return y + row_height - rect.y() + margins.bottom()


class ElidedLabel(QLabel):
    """A single-line label that elides its text instead of widening the
    layout. The full text is the tooltip."""

    def __init__(self, text: str = "", parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        self.setText(text)

    def setText(self, text: str) -> None:  # type: ignore[override]
        super().setText(text)
        self.setToolTip(text)
        self.updateGeometry()
        self.update()

    def minimumSizeHint(self) -> QSize:
        return QSize(0, super().minimumSizeHint().height())

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        rect = self.contentsRect()
        text = self.fontMetrics().elidedText(
            self.text(), Qt.TextElideMode.ElideMiddle, rect.width()
        )
        painter.setPen(self.palette().color(QPalette.ColorRole.WindowText))
        painter.drawText(rect, int(self.alignment()), text)


# ── detail page pieces ───────────────────────────────────────────────────────


class MetricTile(QFrame):
    """A small label over a big value."""

    def __init__(self, label: str, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setObjectName("tile")
        self.setStyleSheet(f"QFrame#tile {{ background: {TILE_BG}; border-radius: 6px; }}")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 5, 10, 6)
        layout.setSpacing(0)
        self.lbl = QLabel(label)
        self.lbl.setFont(ui_font(0.9))
        self.lbl.setStyleSheet(f"color: {TEXT_DIM};")
        self.val = QLabel("—")
        self.val.setFont(ui_font(1.4, QFont.Weight.DemiBold))
        self.val.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.lbl)
        layout.addWidget(self.val)
        self._stale = False
        self._apply_color()

    def set_value(self, text: str) -> None:
        self.val.setText(text)

    def set_stale(self, stale: bool) -> None:
        self._stale = stale
        self._apply_color()

    def value_color(self) -> str:
        return STALE_COLOR.name() if self._stale else TEXT

    def _apply_color(self) -> None:
        self.val.setStyleSheet(f"color: {self.value_color()};")


class KeyValuePanel(QFrame):
    """A titled panel of label / value pairs, two pairs to a row, fed from a
    dict of values keyed like ``rows``. A key missing from the values hides
    its row; a hint line replaces the rows while there are no values."""

    def __init__(
        self,
        title: str,
        rows: Sequence[Tuple[str, str]],
        hint: str,
        parent: Optional[QWidget] = None,
    ):
        super().__init__(parent)
        self.setObjectName("panel")
        self.setStyleSheet(f"QFrame#panel {{ background: {TILE_BG}; border-radius: 6px; }}")
        self._rows = tuple(rows)
        self._shown: Tuple[str, ...] = ()
        self._stale = False
        outer = QVBoxLayout(self)
        outer.setContentsMargins(10, 6, 10, 8)
        outer.setSpacing(4)
        self.lbl_title = QLabel(title)
        self.lbl_title.setFont(ui_font(0.9))
        self.lbl_title.setStyleSheet(f"color: {TEXT_DIM};")
        outer.addWidget(self.lbl_title)
        self.lbl_hint = QLabel(hint)
        self.lbl_hint.setStyleSheet(f"color: {TEXT_MUTED};")
        self.lbl_hint.setWordWrap(True)
        outer.addWidget(self.lbl_hint)
        self.grid = QGridLayout()
        self.grid.setHorizontalSpacing(8)
        self.grid.setVerticalSpacing(2)
        outer.addLayout(self.grid)
        self.labels: Dict[str, QLabel] = {}
        self.values: Dict[str, QLabel] = {}
        for key, label in self._rows:
            lbl = QLabel(label)
            lbl.setStyleSheet(f"color: {TEXT_MUTED};")
            lbl.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignTop)
            val = QLabel("—")
            val.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
            val.setWordWrap(True)
            val.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
            lbl.hide()
            val.hide()
            self.labels[key] = lbl
            self.values[key] = val
        self._apply_color()

    def set_values(self, values: Optional[Dict[str, str]]) -> None:
        """Show ``values`` (None or empty: the hint instead of any row)."""
        shown = tuple(key for key, _label in self._rows if values and key in values)
        if shown != self._shown:
            self._arrange(shown)
        for key in shown:
            self.values[key].setText(values[key])
        self.lbl_hint.setVisible(not shown)

    def set_hint(self, text: str) -> None:
        self.lbl_hint.setText(text)

    def shown_values(self) -> Dict[str, str]:
        return {key: self.values[key].text() for key in self._shown}

    def set_stale(self, stale: bool) -> None:
        self._stale = stale
        self._apply_color()

    def _apply_color(self) -> None:
        color = STALE_COLOR.name() if self._stale else TEXT
        for val in self.values.values():
            val.setStyleSheet(f"color: {color};")

    def _arrange(self, shown: Tuple[str, ...]) -> None:
        for key in self._shown:
            self.grid.removeWidget(self.labels[key])
            self.grid.removeWidget(self.values[key])
            self.labels[key].hide()
            self.values[key].hide()
        for i, key in enumerate(shown):
            row, pair = divmod(i, 2)
            self.grid.addWidget(self.labels[key], row, pair * 2)
            self.grid.addWidget(self.values[key], row, pair * 2 + 1)
            self.labels[key].show()
            self.values[key].show()
        self.grid.setColumnStretch(1, 1)
        self.grid.setColumnStretch(3, 1)
        self._shown = shown


class StatePill(QLabel):
    """The process state as a coloured pill."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setFont(ui_font(0.95, QFont.Weight.Bold))
        self._color = STALE_COLOR.name()
        self.set_state(RuntimeState.UNKNOWN)

    def set_state(self, state: RuntimeState, stale: bool = False) -> None:
        self.setText(STATE_LABELS.get(state, state.name.title()))
        self._color = STALE_COLOR.name() if stale else state_color(state)
        self.setStyleSheet(pill_style(self._color))

    def color(self) -> str:
        return self._color


class StateBand(QWidget):
    """The process state over recent time as a strip of coloured segments."""

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.setFixedHeight(max(8, int(ui_point_size())))
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._segments: List[Tuple[float, float, int]] = []
        self._since = 0.0
        self._now = 1.0
        self._stale = False
        self.setToolTip("Process state over the last 15 minutes; a hole means no reports")

    def set_segments(
        self, segments: Sequence[Tuple[float, float, int]], since: float, now: float
    ) -> None:
        self._segments = list(segments)
        self._since, self._now = since, now
        self.update()

    def segments(self) -> List[Tuple[float, float, int]]:
        return list(self._segments)

    def set_stale(self, stale: bool) -> None:
        self._stale = stale
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        radius = rect.height() / 2
        path = QPainterPath()
        path.addRoundedRect(rect, radius, radius)
        p.fillPath(path, QColor(TILE_BG))
        p.setClipPath(path)
        span = max(self._now - self._since, 1e-9)
        for start, end, state in self._segments:
            x0 = (start - self._since) / span * rect.width()
            x1 = (end - self._since) / span * rect.width()
            if x1 - x0 < 1:
                x1 = x0 + 1
            color = QColor(state_color(RuntimeState.from_byte(state)))
            if self._stale:
                color.setAlpha(110)
            p.fillRect(QRectF(x0, 0, x1 - x0, rect.height()), color)


class GraphsPanel(QWidget):
    """The four usage charts for the selected process, with a time span
    selector and chips to overlay other processes in their own colours."""

    SPANS = ((60, "1 min"), (300, "5 min"), (900, "15 min"))
    DEFAULT_SPAN = 300

    def __init__(self, history: UsageHistory, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.history = history
        self.name = ""
        self._compare: set = set()
        self._colors: Dict[str, QColor] = {}
        self.chips: Dict[str, QPushButton] = {}

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 6, 8, 4)
        root.setSpacing(6)

        top = QHBoxLayout()
        top.setSpacing(10)
        self._spans = QButtonGroup(self)
        self._spans.setExclusive(True)
        segments = QHBoxLayout()
        segments.setSpacing(0)
        for i, (seconds, label) in enumerate(self.SPANS):
            button = QPushButton(label)
            button.setCheckable(True)
            button.setStyleSheet(segment_style(i == 0, i == len(self.SPANS) - 1))
            button.setChecked(seconds == self.DEFAULT_SPAN)
            self._spans.addButton(button, seconds)
            segments.addWidget(button)
        self._spans.buttonClicked.connect(lambda _button: self.refresh_now())
        top.addLayout(segments)
        self.lbl_legend = QLabel()
        top.addWidget(self.lbl_legend)
        top.addStretch()
        root.addLayout(top)

        # Compare chips wrap onto further rows when the pane is narrow.
        compare_row = QWidget()
        self._chips_layout = FlowLayout(compare_row, h_spacing=6, v_spacing=4)
        lbl_compare = QLabel("Compare")
        lbl_compare.setStyleSheet(f"color: {TEXT_MUTED};")
        self._chips_layout.addWidget(lbl_compare)
        self.lbl_none = QLabel("no other process in view")
        self.lbl_none.setStyleSheet(f"color: {TEXT_MUTED};")
        self._chips_layout.addWidget(self.lbl_none)
        root.addWidget(compare_row)

        grid = QGridLayout()
        grid.setSpacing(6)
        self.charts = [UsageChart(metric) for metric in METRICS]
        for i, chart in enumerate(self.charts):
            grid.addWidget(chart, i // 2, i % 2)
        root.addLayout(grid, 1)

        self.lbl_hint = QLabel(
            "Hover a chart to read every visible series at that moment · "
            "one sample a second · the last 15 minutes are kept"
        )
        self.lbl_hint.setStyleSheet(f"color: {TEXT_MUTED};")
        self.lbl_hint.setFont(ui_font(0.9))
        self.lbl_hint.setWordWrap(True)  # never widens the window
        root.addWidget(self.lbl_hint)

    def set_process(self, name: str) -> None:
        self.name = name
        self._compare.discard(name)
        self._update_legend()
        self.refresh_now()

    def color(self, name: str) -> QColor:
        if name not in self._colors:
            self._colors[name] = SERIES_COLORS[len(self._colors) % len(SERIES_COLORS)]
        return self._colors[name]

    def span_seconds(self) -> float:
        checked = self._spans.checkedId()
        return float(checked if checked > 0 else self.DEFAULT_SPAN)

    def compared(self) -> List[str]:
        return sorted(self._compare)

    def shown_names(self) -> List[str]:
        return [name for name, _color, _points in self.charts[0].series]

    def refresh_now(self) -> None:
        self.refresh(time.monotonic(), time.time())

    def refresh(self, now: float, wall_now: float) -> None:
        span = self.span_seconds()
        since = now - span
        in_view = [n for n in self.history.names() if self.history.samples(n, since)]
        self._sync_chips([n for n in in_view if n != self.name])
        shown = ([self.name] if self.name else []) + [
            n for n in in_view if n in self._compare and n != self.name
        ]
        samples = {n: self.history.samples(n, since) for n in shown}
        for chart in self.charts:
            chart.set_data(
                build_series(samples, shown, self.color, chart.metric),
                now,
                wall_now,
                span,
                self.history.gap_sec,
            )

    def _update_legend(self) -> None:
        if not self.name:
            self.lbl_legend.setText("")
            return
        color = self.color(self.name).name()
        self.lbl_legend.setText(
            f'<span style="color:{color}">■</span> {html.escape(self.name)}'
        )

    def _sync_chips(self, names: List[str]) -> None:
        if names == list(self.chips):
            return
        for chip in self.chips.values():
            self._chips_layout.removeWidget(chip)
            chip.hide()  # gone at once, not after the event loop deletes it
            chip.deleteLater()
        self.chips = {}
        for name in names:
            chip = QPushButton(name)
            chip.setCheckable(True)
            chip.setChecked(name in self._compare)
            chip.setStyleSheet(self._chip_style(name))
            chip.setToolTip(f"Overlay {name} on the charts")
            chip.toggled.connect(lambda checked, n=name: self._on_chip(n, checked))
            self._chips_layout.addWidget(chip)
            self.chips[name] = chip
        self.lbl_none.setVisible(not names)

    def _chip_style(self, name: str) -> str:
        color = self.color(name).name()
        radius = max(6, int(ui_point_size() * 0.9))
        return (
            f"QPushButton {{ border: 1px solid #555; border-radius: {radius}px; "
            f"padding: 1px {radius}px; color: {TEXT_DIM}; background: transparent; }}"
            f"QPushButton:checked {{ border-color: {color}; color: {color}; "
            f"background: {tint(color, 0.16)}; }}"
            f"QPushButton:hover {{ color: {TEXT}; }}"
        )

    def _on_chip(self, name: str, checked: bool) -> None:
        if checked:
            self._compare.add(name)
        else:
            self._compare.discard(name)
        self.refresh_now()


class CgroupPidsTable(QTableWidget):
    """PIDs in the process's task_<name> cgroup, with comm, RSS and cmdline."""

    pid_activated = pyqtSignal(int, str)
    HEADERS = ("comm", "PID", "RSS", "cmdline")

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(0, len(self.HEADERS), parent)
        self.setHorizontalHeaderLabels(list(self.HEADERS))
        self.verticalHeader().setVisible(False)
        self.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.setAlternatingRowColors(True)
        self.setShowGrid(False)
        self.setStyleSheet(TABLE_STYLE)
        header = self.horizontalHeader()
        header.setStretchLastSection(True)
        for i in range(len(self.HEADERS) - 1):
            header.setSectionResizeMode(i, QHeaderView.ResizeMode.ResizeToContents)
        self.setToolTip("Double-click a PID for its own journal")
        self.itemDoubleClicked.connect(self._on_double_clicked)

    def set_members(self, members: Sequence) -> None:
        keep = self.selected_pid()
        rows = sorted(members, key=lambda m: int(getattr(m, "pid", 0) or 0))
        self.setRowCount(len(rows))
        for row, proc in enumerate(rows):
            pid = int(getattr(proc, "pid", 0) or 0)
            comm = getattr(proc, "comm", "") or str(pid)
            rss = int(getattr(proc, "rss_bytes", 0) or 0)
            cmdline = getattr(proc, "cmdline", "") or ""
            cells = (comm, str(pid), format_bytes(rss) if rss else "—", cmdline)
            for col, text in enumerate(cells):
                item = QTableWidgetItem(text)
                if col in (1, 2):
                    item.setTextAlignment(
                        int(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
                    )
                if col == 3:
                    item.setToolTip(cmdline)
                    item.setForeground(QColor(TEXT_DIM))
                self.setItem(row, col, item)
            self.item(row, 0).setData(Qt.ItemDataRole.UserRole, pid)
            if pid == keep:
                self.selectRow(row)

    def selected_pid(self) -> Optional[int]:
        model = self.selectionModel()
        rows = model.selectedRows() if model is not None else []
        if not rows:
            return None
        first = self.item(rows[0].row(), 0)
        return int(first.data(Qt.ItemDataRole.UserRole)) if first is not None else None

    def _on_double_clicked(self, item: QTableWidgetItem) -> None:
        first = self.item(item.row(), 0)
        pid = int(first.data(Qt.ItemDataRole.UserRole) or 0)
        if pid > 0:
            self.pid_activated.emit(pid, first.text())


class ProcessDetailPage(QWidget):
    """Everything about the selected process: header with state and actions,
    metric tiles, the state band and the Graphs / cgroup PIDs / Journal tabs."""

    command_requested = pyqtSignal(object, str)  # CommandEnum, process name
    pid_activated = pyqtSignal(int, str)  # pid, comm
    pop_out_requested = pyqtSignal()

    TILES = (
        ("cpu", "CPU · of one core"),
        ("mem", "Memory"),
        ("gpu", "GPU"),
        ("vram", "VRAM"),
        ("missed", "Missed beats"),
        ("restarts", "Restarts"),
    )
    BAND_SPAN_SEC = UsageHistory.KEEP_SEC

    def __init__(self, history: UsageHistory, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.history = history
        self.name = ""
        self.report: Optional[dict] = None
        self._stale = False

        # The content scrolls when the window is shorter than it needs,
        # instead of squeezing the charts until their labels clip.
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setFrameShape(QFrame.Shape.NoFrame)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.scroll.viewport().setAutoFillBackground(False)
        content = QWidget()
        content.setAutoFillBackground(False)
        self.scroll.setWidget(content)
        outer.addWidget(self.scroll)

        root = QVBoxLayout(content)
        root.setContentsMargins(14, 10, 14, 8)
        root.setSpacing(8)

        header = QHBoxLayout()
        header.setSpacing(10)
        self.lbl_name = QLabel("—")
        self.lbl_name.setFont(ui_font(1.45, QFont.Weight.Bold))
        self.lbl_name.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        header.addWidget(self.lbl_name)
        self.pill = StatePill()
        header.addWidget(self.pill)
        header.addStretch()
        self.buttons: Dict[CommandEnum, QPushButton] = {}
        for command, label in (
            (CommandEnum.START, "Start"),
            (CommandEnum.STOP, "Stop"),
            (CommandEnum.RESTART, "Restart"),
        ):
            button = QPushButton(label)
            button.setStyleSheet(ACTION_STYLES[command])
            button.setEnabled(False)
            button.clicked.connect(lambda _checked=False, c=command: self._request(c))
            header.addWidget(button)
            self.buttons[command] = button
        # Equal widths from the label font rather than fixed pixels.
        width = max(b.sizeHint().width() for b in self.buttons.values())
        width += self.fontMetrics().height()
        for button in self.buttons.values():
            button.setMinimumWidth(width)
        root.addLayout(header)

        self.lbl_meta = QLabel("Waiting for a health report…")
        self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        self.lbl_meta.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.lbl_meta.setWordWrap(True)
        root.addWidget(self.lbl_meta)

        tiles = QGridLayout()
        tiles.setSpacing(8)
        self.tiles: Dict[str, MetricTile] = {}
        for i, (key, label) in enumerate(self.TILES):
            tile = MetricTile(label)
            tiles.addWidget(tile, i // 3, i % 3)
            self.tiles[key] = tile
        root.addLayout(tiles)

        # What the manager measures beyond the health record (port 6668).
        self.details = KeyValuePanel("Details · from the manager's detailed report", DETAIL_ROWS, NO_REPORT_TEXT)
        root.addWidget(self.details)

        band_row = QHBoxLayout()
        band_row.setSpacing(10)
        self.lbl_band = QLabel("State · last 15 min")
        self.lbl_band.setFont(ui_font(0.9))
        self.lbl_band.setStyleSheet(f"color: {TEXT_DIM};")
        band_row.addWidget(self.lbl_band)
        self.band = StateBand()
        band_row.addWidget(self.band, stretch=1)
        root.addLayout(band_row)

        self.tabs = QTabWidget()
        self.tabs.setStyleSheet(TAB_STYLE)
        self.graphs = GraphsPanel(history)
        self.tabs.addTab(self.graphs, "Graphs")
        self.pids = CgroupPidsTable()
        self.pids.pid_activated.connect(self.pid_activated)
        self.tabs.addTab(self.pids, "cgroup PIDs")
        journal_page = QWidget()
        journal_layout = QVBoxLayout(journal_page)
        journal_layout.setContentsMargins(8, 8, 8, 8)
        journal_layout.setSpacing(6)
        self.lbl_journal_meta = QLabel("Select a process")
        self.lbl_journal_meta.setStyleSheet(mono_style(TEXT_DIM))
        self.lbl_journal_meta.setWordWrap(True)
        journal_layout.addWidget(self.lbl_journal_meta)
        self.txt_journal = QTextEdit()
        self.txt_journal.setReadOnly(True)
        self.txt_journal.setLineWrapMode(QTextEdit.LineWrapMode.WidgetWidth)
        self.txt_journal.setWordWrapMode(QTextOption.WrapMode.WrapAnywhere)
        self.txt_journal.setStyleSheet(LOG_EDIT_STYLE)
        journal_layout.addWidget(self.txt_journal, stretch=1)
        self._journal = JournalView(self.txt_journal)
        self.tabs.addTab(journal_page, "Journal")
        self.btn_pop_out = QPushButton("Pop out")
        self.btn_pop_out.setStyleSheet(SMALL_BUTTON_STYLE)
        self.btn_pop_out.setToolTip("Open the usage graphs in their own window")
        self.btn_pop_out.clicked.connect(self.pop_out_requested)
        self.tabs.setCornerWidget(self.btn_pop_out, Qt.Corner.TopRightCorner)
        self.tabs.currentChanged.connect(self._on_tab_changed)
        root.addWidget(self.tabs, stretch=1)

    # ── data in ───────────────────────────────────────────────────────────

    def set_process(
        self, name: str, report: Optional[dict], gpu, members: Optional[Sequence]
    ) -> None:
        self.name = name
        self.lbl_name.setText(name)
        self.graphs.set_process(name)
        self.set_members(members or [])
        self.clear_journal()
        self.details.set_values(None)  # the window follows with set_details
        if report is None:
            self._show_no_report()
        else:
            self.update_report(report, gpu)
        self.tick(time.monotonic(), time.time())

    def set_details(self, record: Optional[dict], report: Optional[dict]) -> None:
        """The process's record of the manager's detailed report, or None
        while there is no (current) report: the panel explains instead."""
        if record is None or report is None:
            self.details.set_values(None)
        else:
            self.details.set_values(details_values(record, report))

    def update_report(self, report: dict, gpu) -> None:
        self.report = report
        state: RuntimeState = report["state"]
        self.pill.set_state(state, self._stale)
        self.lbl_meta.setText(meta_text(report))
        gpu_pct, vram = gpu_texts(gpu)
        values = {
            "cpu": cpu_text(report),
            "mem": format_bytes(int(report.get("memoryUsageInBytes", 0))),
            "gpu": gpu_pct,
            "vram": vram,
            "missed": str(report.get("missedBeats", 0)),
            "restarts": str(report.get("restartCount", 0)),
        }
        for key, text in values.items():
            self.tiles[key].set_value(text)
        for command, button in self.buttons.items():
            button.setEnabled(action_enabled(command, state))

    def _show_no_report(self) -> None:
        self.report = None
        self.pill.set_state(RuntimeState.UNKNOWN, self._stale)
        self.lbl_meta.setText("Waiting for a health report…")
        for tile in self.tiles.values():
            tile.set_value("—")
        for button in self.buttons.values():
            button.setEnabled(False)

    def tile_values(self) -> Dict[str, str]:
        return {key: tile.val.text() for key, tile in self.tiles.items()}

    def set_members(self, members: Sequence) -> None:
        self.pids.set_members(members)
        self.tabs.setTabText(self.tabs.indexOf(self.pids), f"cgroup PIDs · {len(members)}")

    def set_journal(self, pairs: list, cgroup_path: str, error: str) -> None:
        cgroup = task_cgroup_name(self.name)
        if cgroup_path:
            self.lbl_journal_meta.setText(f"{cgroup}  →  {cgroup_path}")
            self.lbl_journal_meta.setStyleSheet(mono_style(TEXT_DIM))
        else:
            self.lbl_journal_meta.setText(error or f"{cgroup} not found")
            self.lbl_journal_meta.setStyleSheet(mono_style(DANGER))
        self._journal.show([(0, error)] if error and not pairs else pairs)

    def clear_journal(self) -> None:
        self.lbl_journal_meta.setText(f"{task_cgroup_name(self.name)} · loading journal…")
        self.lbl_journal_meta.setStyleSheet(mono_style(TEXT_DIM))
        self._journal.reset()  # the next process starts following its newest line
        self._journal.show([(0, "Loading journal…")])

    def set_stale(self, stale: bool) -> None:
        self._stale = stale
        for tile in self.tiles.values():
            tile.set_stale(stale)
        state = self.report["state"] if self.report else RuntimeState.UNKNOWN
        self.pill.set_state(state, stale)
        self.lbl_meta.setStyleSheet(mono_style(STALE_COLOR.name() if stale else TEXT_DIM))
        self.details.set_stale(stale)
        self.band.set_stale(stale)

    def tick(self, now: float, wall_now: float) -> None:
        """Once a second: redraw the state band, and the charts when shown."""
        if not self.name:
            return
        since = now - self.BAND_SPAN_SEC
        self.band.set_segments(
            state_segments(
                self.history.samples(self.name, since), since, now, self.history.gap_sec
            ),
            since,
            now,
        )
        if self.tabs.currentWidget() is self.graphs:
            self.graphs.refresh(now, wall_now)

    # ── events ────────────────────────────────────────────────────────────

    def _request(self, command: CommandEnum) -> None:
        if self.name:
            self.command_requested.emit(command, self.name)

    def _on_tab_changed(self, _index: int) -> None:
        self.tick(time.monotonic(), time.time())


# ── manager (service) page ───────────────────────────────────────────────────


class ServiceDetailPage(QWidget):
    """The manager: whole-manager actions, the host overview from its report,
    then systemctl status and the journal of the manager unit."""

    command_requested = pyqtSignal(object, str)  # CommandEnum, "*" or "" for reload

    def __init__(self, parent: Optional[QWidget] = None):
        super().__init__(parent)
        root = QVBoxLayout(self)
        root.setContentsMargins(14, 12, 14, 10)
        root.setSpacing(8)

        header = QHBoxLayout()
        header.setSpacing(10)
        title = QLabel(MANAGER_NAME)
        title.setFont(ui_font(1.45, QFont.Weight.Bold))
        header.addWidget(title)
        self.lbl_unit = QLabel(SERVICE_UNIT)
        self.lbl_unit.setStyleSheet(mono_style(TEXT_DIM))
        header.addWidget(self.lbl_unit)
        header.addStretch()
        # Enabled while the command link is up (set_link_up).
        self.buttons: Dict[CommandEnum, QPushButton] = {}
        for command, label, tip in MANAGER_ACTIONS:
            button = QPushButton(label)
            button.setStyleSheet(ACTION_STYLES[command])
            button.setToolTip(tip)
            button.setEnabled(False)
            button.clicked.connect(lambda _checked=False, c=command: self._request(c))
            header.addWidget(button)
            self.buttons[command] = button
        root.addLayout(header)
        # Fetch errors (no systemd, journalctl failed…) get a line of their own.
        self.lbl_error = QLabel()
        self.lbl_error.setStyleSheet(f"color: {DANGER};")
        self.lbl_error.setWordWrap(True)
        self.lbl_error.setVisible(False)
        root.addWidget(self.lbl_error)

        # The manager's own figures (port 6668): shown on every platform, also
        # where there is no systemd to ask.
        self.host = KeyValuePanel("Host · from the manager's detailed report", HOST_ROWS, WAITING_REPORT_TEXT)
        root.addWidget(self.host)

        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(6)

        status_box = QWidget()
        status_layout = QVBoxLayout(status_box)
        status_layout.setContentsMargins(0, 0, 0, 0)
        status_layout.setSpacing(4)
        status_row = QHBoxLayout()
        lbl_status = QLabel("systemctl status (unit metadata only)")
        lbl_status.setStyleSheet(f"color: {TEXT_DIM};")
        status_row.addWidget(lbl_status)
        status_row.addStretch()
        self.lbl_refresh = QLabel("Last refresh: —")
        self.lbl_refresh.setStyleSheet(f"color: {TEXT_MUTED};")
        status_row.addWidget(self.lbl_refresh)
        status_layout.addLayout(status_row)
        self.txt_status = QPlainTextEdit()
        self.txt_status.setReadOnly(True)
        self.txt_status.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
        self.txt_status.setWordWrapMode(QTextOption.WrapMode.WrapAnywhere)
        self.txt_status.setStyleSheet(STATUS_EDIT_STYLE)
        self.txt_status.setPlaceholderText("Loading systemctl status…")
        self.txt_status.setMinimumHeight(100)
        status_layout.addWidget(self.txt_status, stretch=1)
        splitter.addWidget(status_box)

        journal_box = QWidget()
        journal_layout = QVBoxLayout(journal_box)
        journal_layout.setContentsMargins(0, 0, 0, 0)
        journal_layout.setSpacing(4)
        lbl_journal = QLabel(
            f"journalctl -u {SERVICE_UNIT} · new entries are appended; "
            "the view follows the newest line until you scroll up"
        )
        lbl_journal.setStyleSheet(f"color: {TEXT_DIM};")
        lbl_journal.setWordWrap(True)
        journal_layout.addWidget(lbl_journal)
        self.txt_journal = QTextEdit()
        self.txt_journal.setReadOnly(True)
        self.txt_journal.setLineWrapMode(QTextEdit.LineWrapMode.WidgetWidth)
        self.txt_journal.setWordWrapMode(QTextOption.WrapMode.WrapAnywhere)
        self.txt_journal.setStyleSheet(LOG_EDIT_STYLE)
        self.txt_journal.setPlaceholderText("Loading journal…")
        self.txt_journal.setMinimumHeight(180)
        journal_layout.addWidget(self.txt_journal, stretch=1)
        self._journal = JournalView(self.txt_journal)
        splitter.addWidget(journal_box)

        splitter.setStretchFactor(0, 1)
        splitter.setStretchFactor(1, 2)
        splitter.setSizes([180, 420])
        root.addWidget(splitter, stretch=1)

    def set_link_up(self, up: bool) -> None:
        """Whole-manager actions need the command socket."""
        for button in self.buttons.values():
            button.setEnabled(up)

    def _request(self, command: CommandEnum) -> None:
        self.command_requested.emit(command, "" if command == CommandEnum.RELOAD else ALL_SERVICES)

    def show_report(self, report: Optional[dict]) -> None:
        """The latest detailed report, or None while there is none."""
        self.host.set_values(host_values(report) if report else None)

    def set_report_stale(self, stale: bool) -> None:
        self.host.set_stale(stale)

    def show_snapshot(self, status: str, pairs: list, error: str) -> None:
        status = status or "(empty)"
        if status != self.txt_status.toPlainText():
            # Changes every refresh (CPU/memory lines); keep the reader's place.
            bar = self.txt_status.verticalScrollBar()
            keep = bar.value()
            self.txt_status.setPlainText(status)
            bar.setValue(keep)
        self._journal.show(pairs)
        self.lbl_refresh.setText(f"Last refresh: {time.strftime('%H:%M:%S')}")
        self.lbl_refresh.setStyleSheet(f"color: {DANGER if error else TEXT_MUTED};")
        self.lbl_error.setText(error or "")
        self.lbl_error.setVisible(bool(error))
