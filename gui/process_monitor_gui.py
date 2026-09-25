#!/usr/bin/env python3
"""
Process Manager Health Monitor – PyQt6 + ZMQ

DEALER wire format (exactly as specified by you):
    setsockopt(IDENTITY, b"PMC")
    send(b"BPM", SNDMORE)
    send(CommandMessage)          # packed: uint8 cmd + char[32] name + char[32] args

SUB: receives array of DetailedHealthReport (raw or length-prefixed)
SUB (report): the manager's detailed report, frames "report" + payload
    (per-service GPU figures, threads, I/O, exit codes; host figures)

Layout: the sidebar lists the manager unit and every reported process; the
pane on the right shows the selection — metric tiles, a state band and the
Graphs / cgroup PIDs / Journal tabs for a process, or systemctl status and
the unit journal for the manager. The widgets live in process_views.py;
this module owns the workers and wires them to the views.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from collections import deque
from queue import Empty, SimpleQueue
from typing import Deque, Dict, List, Optional, Tuple

import zmq
from PyQt6.QtCore import QObject, QSize, QThread, QTimer, pyqtSignal, pyqtSlot, Qt
from PyQt6.QtGui import QColor, QFont, QPalette, QTextOption
from PyQt6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QTextEdit,
    QToolBar,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from health_structs import (
    REPORT_TOPIC,
    CommandEnum,
    describe_reply,
    make_command_message,
    parse_command_reply,
    parse_health_reports,
    parse_report_frames,
    report_to_dict,
)
from gpu_sampler import GpuSampler, GpuProcessUsage
from journal_view import (  # noqa: F401  re-exported: former home of these names
    JournalView,
    NO_PID_COLOR,
    PID_PALETTE,
    color_for_pid,
    new_journal_entries,
)
from process_views import (  # noqa: F401  re-exported
    ACCENT,
    ALIVE_STATES,
    BORDER,
    MANAGER_NAME,
    SIDEBAR_BG,
    STALE_COLOR,
    TEXT,
    TEXT_DIM,
    TEXT_MUTED,
    ElidedLabel,
    ProcessDetailPage,
    ProcessSidebar,
    ServiceDetailPage,
    mono_style,
    pill_style,
    service_active_text,
    uptime_text,
)
from systemd_logs import (
    JOURNAL_LINES,
    REFRESH_MS,
    SERVICE_UNIT,
    fetch_journal_cgroup,
    fetch_journal_pid,
    fetch_snapshot,
    find_task_cgroup,
    snapshot_cgroup_members,
    task_cgroup_name,
)
from ui_scale import (  # noqa: F401  re-exported
    DESIGN_UI_POINT_SIZE,
    EMPHASIS_FONT_SCALE,
    MONO_FONT_FAMILIES,
    fit_to_screen,
    ui_font,
    ui_point_size,
)
from usage_graphs import UsageGraphWindow, UsageHistory, UsageSample

_color_for_pid = color_for_pid  # former name

# The manager's default ports (docs/protocol.md), as a client on the same host sees them.
DEFAULT_SUB_ENDPOINT = "tcp://127.0.0.1:6667"
DEFAULT_DEALER_ENDPOINT = "tcp://127.0.0.1:5557"
DEFAULT_REPORT_ENDPOINT = "tcp://127.0.0.1:6668"


# ──────────────────────────────────────────────────────────────────────────────
# ZMQ Worker
# ──────────────────────────────────────────────────────────────────────────────

class ZmqWorker(QObject):
    reports_received  = pyqtSignal(list)    # list[dict]
    report_received   = pyqtSignal(object)  # dict: the detailed report (port 6668)
    connection_status = pyqtSignal(str)
    log_message       = pyqtSignal(str)

    def __init__(
        self,
        sub_endpoint: str = DEFAULT_SUB_ENDPOINT,
        dealer_endpoint: str = DEFAULT_DEALER_ENDPOINT,
        sub_topic: bytes = b"",
        parent: Optional[QObject] = None,
        report_endpoint: str = DEFAULT_REPORT_ENDPOINT,
    ):
        super().__init__(parent)
        self.sub_endpoint    = sub_endpoint
        self.dealer_endpoint = dealer_endpoint
        self.sub_topic       = sub_topic
        self.report_endpoint = report_endpoint  # empty: the detailed report is not read
        self._running        = True   # until stop(); start() never re-arms it
        self._ctx: Optional[zmq.Context] = None
        self._sub: Optional[zmq.Socket] = None
        self._report: Optional[zmq.Socket] = None
        self._dealer: Optional[zmq.Socket] = None
        self._command_queue: SimpleQueue = SimpleQueue()  # (label, payload)
        self._pending: Optional[Tuple[str, bytes]] = None  # command being sent
        self._pending_logged = False
        self._report_error: Optional[str] = None  # last parse error shown

    @pyqtSlot()
    def start(self):
        self._ctx = zmq.Context.instance()
        try:
            # SUB
            self._sub = self._ctx.socket(zmq.SUB)
            self._sub.setsockopt(zmq.RCVHWM, 10)
            self._sub.setsockopt(zmq.SUBSCRIBE, self.sub_topic)
            self._sub.connect(self.sub_endpoint)
            self._sub.setsockopt(zmq.RCVTIMEO, 100)

            # SUB for the detailed report: its own socket on the manager, topic "report"
            if self.report_endpoint:
                self._report = self._ctx.socket(zmq.SUB)
                self._report.setsockopt(zmq.RCVHWM, 10)
                self._report.setsockopt(zmq.SUBSCRIBE, REPORT_TOPIC)
                self._report.connect(self.report_endpoint)
                self._report.setsockopt(zmq.RCVTIMEO, 100)

            # DEALER – exact identity required by process manager
            self._dealer = self._ctx.socket(zmq.DEALER)
            self._dealer.setsockopt(zmq.IDENTITY, b"PMC")
            self._dealer.setsockopt(zmq.SNDHWM, 100)
            self._dealer.setsockopt(zmq.SNDTIMEO, 500)
            self._dealer.setsockopt(zmq.LINGER, 0)
            self._dealer.connect(self.dealer_endpoint)

            self.connection_status.emit("connected")
            self.log_message.emit(
                f"SUB → {self.sub_endpoint}  |  "
                + (f"REPORT → {self.report_endpoint}  |  " if self._report is not None else "")
                + f"DEALER(identity=PMC) → {self.dealer_endpoint}"
            )
        except zmq.ZMQError as e:
            self.connection_status.emit(f"error: {e}")
            self.log_message.emit(f"ZMQ connect failed: {e}")
            self._running = False
            return

        poller = zmq.Poller()
        poller.register(self._sub, zmq.POLLIN)
        if self._report is not None:
            poller.register(self._report, zmq.POLLIN)
        # The C++ manager answers every command; older managers never do.
        poller.register(self._dealer, zmq.POLLIN)

        while self._running:
            # ── drain command queue in order (thread-safe SimpleQueue) ──
            while True:
                if self._pending is None:
                    try:
                        self._pending = self._command_queue.get_nowait()
                    except Empty:
                        break
                    self._pending_logged = False
                label, payload = self._pending
                try:
                    # Original wire format (DEALER → ROUTER):
                    #   send(b"BPM", SNDMORE)
                    #   send(CommandMessage)
                    self._dealer.send(b"BPM", flags=zmq.SNDMORE | zmq.NOBLOCK)
                    self._dealer.send(payload, flags=zmq.NOBLOCK)
                    self.log_message.emit(f"Sent {label}")
                except zmq.Again:
                    # Keep it first in line; say so once, not every poll.
                    if not self._pending_logged:
                        self.log_message.emit(
                            f"Waiting to send {label} (DEALER queue full)"
                        )
                        self._pending_logged = True
                    break
                except zmq.ZMQError as e:
                    self.log_message.emit(f"Could not send {label}: {e}")
                self._pending = None

            # ── poll health reports ──────────────────────────────────────
            try:
                events = dict(poller.poll(50))
            except zmq.ZMQError:
                break

            if self._sub in events:
                try:
                    frames = self._sub.recv_multipart(flags=zmq.NOBLOCK)
                    data = frames[-1]          # last frame = payload
                    # An empty frame is a report too: the manager has no services.
                    reports = parse_health_reports(data)
                    dicts = [report_to_dict(r) for r in reports]
                    self.reports_received.emit(dicts)
                except zmq.Again:
                    pass
                except ValueError as e:
                    self.log_message.emit(f"Parse error: {e}")
                except Exception as e:
                    self.log_message.emit(f"Recv error: {e}")

            if self._report is not None and self._report in events:
                self._read_report()

            if self._dealer in events:
                self._read_replies()

        self._cleanup()
        self.connection_status.emit("disconnected")

    def stop(self):
        self._running = False

    def _read_report(self) -> None:
        """One detailed report; a payload this reader cannot parse is reported once."""
        try:
            frames = self._report.recv_multipart(flags=zmq.NOBLOCK)
        except zmq.Again:
            return
        except zmq.ZMQError as e:
            self.log_message.emit(f"Report recv error: {e}")
            return
        try:
            report = parse_report_frames(frames)
        except ValueError as e:
            # Every interval brings another one; say it once, not each time.
            if str(e) != self._report_error:
                self._report_error = str(e)
                self.log_message.emit(f"Report parse error: {e}")
            return
        if report is not None:
            self._report_error = None
            self.report_received.emit(report)

    def _read_replies(self) -> None:
        """Show each reply in the status bar, e.g. "restart vision: ok (restarting)"."""
        while True:
            try:
                frames = self._dealer.recv_multipart(flags=zmq.NOBLOCK)
            except zmq.Again:
                return
            except zmq.ZMQError as e:
                self.log_message.emit(f"Reply error: {e}")
                return
            reply = parse_command_reply(frames)
            if reply is not None:
                self.log_message.emit(describe_reply(reply))

    def send_command(self, command: CommandEnum, service_name: str, args: str = ""):
        """Thread-safe: SimpleQueue from any thread; worker drains it."""
        payload = make_command_message(command, service_name, args)
        self._command_queue.put((f"{command.name} for {service_name}", payload))

    def _cleanup(self):
        if self._sub:
            self._sub.close(linger=0)
            self._sub = None
        if self._report:
            self._report.close(linger=0)
            self._report = None
        if self._dealer:
            self._dealer.close(linger=0)
            self._dealer = None


# ──────────────────────────────────────────────────────────────────────────────
# Systemd / GPU / cgroup workers
# ──────────────────────────────────────────────────────────────────────────────

class SystemdLogWorker(QObject):
    """Poll systemctl status + journalctl off the UI thread."""

    snapshot_ready = pyqtSignal(str, list, str)  # status, [(pid, text), ...], error
    finished = pyqtSignal()

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._running = True  # until stop(); start() never re-arms it

    @pyqtSlot()
    def start(self):
        while self._running:
            try:
                snap = fetch_snapshot(SERVICE_UNIT, JOURNAL_LINES)
                err = snap.error or ""
                pairs = [
                    (ln.pid if ln.pid is not None else 0, ln.text)
                    for ln in snap.journal_lines
                ]
                if not pairs and snap.journal_text:
                    pairs = [(0, snap.journal_text)]
                self.snapshot_ready.emit(snap.status_text, pairs, err)
            except Exception as e:
                self.snapshot_ready.emit("", [], str(e))
            slept = 0
            interval_ms = REFRESH_MS
            while self._running and slept < interval_ms:
                QThread.msleep(100)
                slept += 100
        self.finished.emit()

    def stop(self):
        self._running = False


class GpuSampleWorker(QObject):
    """nvidia-smi sampling off the UI thread."""

    sampled = pyqtSignal(object, object, bool)  # map, error_or_None, available
    finished = pyqtSignal()

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._running = True  # until stop(); start() never re-arms it
        self._sampler: Optional[GpuSampler] = None

    @pyqtSlot()
    def start(self):
        self._sampler = GpuSampler()
        self._sampler.init()
        while self._running:
            try:
                if not self._sampler.available():
                    self.sampled.emit({}, self._sampler.last_error(), False)
                else:
                    result = self._sampler.sample()
                    err = None
                    if self._sampler.last_error() and not result:
                        err = self._sampler.last_error()
                        result = {}
                    self.sampled.emit(result, err, True)
            except Exception as e:
                self.sampled.emit({}, str(e), False)
            slept = 0
            while self._running and slept < 1000:
                QThread.msleep(100)
                slept += 100
        if self._sampler:
            self._sampler.shutdown()
        self.finished.emit()

    def stop(self):
        self._running = False


class CgroupLogWorker(QObject):
    """Poll journalctl for a task_<name> cgroup or a single PID (log windows)."""

    lines_ready = pyqtSignal(list, str, str)  # pairs, cgroup_path_or_empty, error
    finished = pyqtSignal()

    def __init__(
        self,
        process_name: str = "",
        pid: Optional[int] = None,
        parent: Optional[QObject] = None,
    ):
        super().__init__(parent)
        self.process_name = process_name
        self.pid = pid
        self._running = True  # until stop(); start() never re-arms it

    @pyqtSlot()
    def start(self):
        while self._running:
            try:
                if self.pid:
                    lines, err = fetch_journal_pid(self.pid, JOURNAL_LINES)
                    pairs = [
                        (ln.pid if ln.pid is not None else 0, ln.text) for ln in lines
                    ]
                    self.lines_ready.emit(
                        pairs, f"_PID={self.pid}", err or ""
                    )
                else:
                    cg = find_task_cgroup(self.process_name)
                    if not cg:
                        self.lines_ready.emit(
                            [],
                            "",
                            f"cgroup {task_cgroup_name(self.process_name)} "
                            f"not found under /sys/fs/cgroup",
                        )
                    else:
                        lines, err = fetch_journal_cgroup(cg, JOURNAL_LINES)
                        pairs = [
                            (ln.pid if ln.pid is not None else 0, ln.text)
                            for ln in lines
                        ]
                        self.lines_ready.emit(pairs, cg, err or "")
            except Exception as e:
                self.lines_ready.emit([], "", str(e))
            slept = 0
            while self._running and slept < REFRESH_MS:
                QThread.msleep(100)
                slept += 100
        self.finished.emit()

    def stop(self):
        self._running = False


class DetailJournalWorker(QObject):
    """Poll journalctl for the cgroup of whichever process is selected.

    The target follows the sidebar selection; changing it wakes the loop,
    so the new journal appears without waiting out the refresh interval.
    """

    # process, pairs, cgroup_path_or_empty, error
    lines_ready = pyqtSignal(str, list, str, str)
    finished = pyqtSignal()

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._running = True  # until stop(); start() never re-arms it
        self._name = ""
        self._changed = False

    def set_process(self, name: str) -> None:
        """Thread-safe: single attribute writes, read by the loop each round."""
        self._name = name or ""
        self._changed = True

    @pyqtSlot()
    def start(self):
        while self._running:
            self._changed = False
            name = self._name
            if name:
                try:
                    cg = find_task_cgroup(name)
                    if not cg:
                        self.lines_ready.emit(
                            name,
                            [],
                            "",
                            f"cgroup {task_cgroup_name(name)} not found under /sys/fs/cgroup",
                        )
                    else:
                        lines, err = fetch_journal_cgroup(cg, JOURNAL_LINES)
                        pairs = [
                            (ln.pid if ln.pid is not None else 0, ln.text) for ln in lines
                        ]
                        self.lines_ready.emit(name, pairs, cg, err or "")
                except Exception as e:
                    self.lines_ready.emit(name, [], "", str(e))
            slept = 0
            while self._running and slept < REFRESH_MS and not self._changed:
                QThread.msleep(100)
                slept += 100
        self.finished.emit()

    def stop(self):
        self._running = False


class CgroupMembersWorker(QObject):
    """Read task_<name>/cgroup.procs + /proc off the UI thread."""

    members_ready = pyqtSignal(object)  # dict[str, list]
    finished = pyqtSignal()

    def __init__(self, parent: Optional[QObject] = None):
        super().__init__(parent)
        self._running = True  # until stop(); start() never re-arms it
        self._names: List[str] = []

    def set_names(self, names: List[str]) -> None:
        self._names = list(names)

    @pyqtSlot()
    def start(self):
        while self._running:
            try:
                names = list(self._names)
                self.members_ready.emit(snapshot_cgroup_members(names))
            except Exception:
                self.members_ready.emit({})
            slept = 0
            while self._running and slept < REFRESH_MS:
                QThread.msleep(100)
                slept += 100
        self.finished.emit()

    def stop(self):
        self._running = False


class ProcessLogWindow(QMainWindow):
    """A journal window for one PID (or, kept for scripts, one cgroup)."""

    def __init__(
        self,
        process_name: str = "",
        parent: Optional[QWidget] = None,
        pid: Optional[int] = None,
        comm: str = "",
    ):
        super().__init__(parent)
        self.process_name = process_name
        self.pid = pid
        if pid:
            title = f"Logs — {comm or 'pid'} ({pid})"
            meta = f"journalctl _PID={pid}"
        else:
            title = f"Logs — {process_name}"
            meta = f"cgroup {task_cgroup_name(process_name)}"
        self.setWindowTitle(title)
        fit_to_screen(self, QSize(860, 480))
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(8, 8, 8, 8)
        self.lbl_meta = QLabel(meta)
        self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        self.lbl_meta.setWordWrap(True)
        layout.addWidget(self.lbl_meta)
        self.txt = QTextEdit()
        self.txt.setReadOnly(True)
        self.txt.setLineWrapMode(QTextEdit.LineWrapMode.WidgetWidth)
        self.txt.setWordWrapMode(QTextOption.WrapMode.WrapAnywhere)
        self.txt.setStyleSheet(
            "QTextEdit { background: #1a1a1a; color: #c8e6c9; "
            f"font-family: {MONO_FONT_FAMILIES}; "
            "border: 1px solid #333; }"
        )
        self._journal = JournalView(self.txt)
        layout.addWidget(self.txt, stretch=1)
        self.setCentralWidget(central)

        self._thread = QThread(self)
        self._worker = CgroupLogWorker(process_name=process_name, pid=pid)
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.start)
        self._worker.lines_ready.connect(self._on_lines)
        self._thread.start()

    @pyqtSlot(list, str, str)
    def _on_lines(self, pairs: list, cgroup_path: str, error: str):
        if self.pid:
            self.lbl_meta.setText(error or f"journalctl _PID={self.pid}")
            self.lbl_meta.setStyleSheet(mono_style("#f44336" if error else TEXT_DIM))
            self._journal.show([(0, error)] if error and not pairs else pairs)
            return
        cg = task_cgroup_name(self.process_name)
        if cgroup_path:
            self.lbl_meta.setText(f"{cg}  →  {cgroup_path}")
            self.lbl_meta.setStyleSheet(mono_style(TEXT_DIM))
        else:
            self.lbl_meta.setText(error or f"{cg} not found")
            self.lbl_meta.setStyleSheet(mono_style("#f44336"))
        self._journal.show([(0, error)] if error and not pairs else pairs)

    def closeEvent(self, event):
        if getattr(self, "_worker", None):
            self._worker.stop()
        if getattr(self, "_thread", None):
            self._thread.quit()
            # journalctl may take its full 5 s timeout.
            if not self._thread.wait(8000):
                self._thread.terminate()
                self._thread.wait(1000)
        super().closeEvent(event)


# ──────────────────────────────────────────────────────────────────────────────
# Report metrics and feed freshness
# ──────────────────────────────────────────────────────────────────────────────

# Clamps sampling glitches only: the counters come from the manager's host,
# which may have many more cores than the machine showing the GUI.
CPU_PCT_MAX = 100.0 * 4096
# Health data is stale after this long without a report (or three report
# intervals, for a publisher slower than that).
STALE_AFTER_SEC = 5.0
FEED_COLORS = {"live": "#4caf50", "waiting": "orange", "stale": "#f44336"}


def cpu_percent(
    prev: Optional[Tuple[int, int]], cpu_usec: int, snap_ns: int
) -> Optional[float]:
    """CPU use since the previous (cpu_usec, snapshot_ns) sample, in percent
    of one core (400 = four busy cores); None without a usable sample."""
    if prev is None:
        return None
    delta_cpu = cpu_usec - prev[0]
    delta_ns = snap_ns - prev[1]
    if delta_ns <= 0 or delta_cpu < 0:  # clock went back / counter reset
        return None
    return min(delta_cpu / (delta_ns / 1000.0) * 100.0, CPU_PCT_MAX)


class FeedMonitor:
    """Tells live health data from stale, from when reports arrive.

    Times are time.monotonic() seconds. A ZMQ connect succeeds even when
    nothing is listening, so only arriving reports prove the feed is live.
    """

    def __init__(self) -> None:
        self.connected_at: Optional[float] = None
        self.last_report_at: Optional[float] = None
        self._gaps: Deque[float] = deque(maxlen=8)

    def on_connected(self, now: float) -> None:
        self.connected_at = now
        self._gaps.clear()

    def on_disconnected(self) -> None:
        self.connected_at = None

    def on_report(self, now: float) -> None:
        if self._received_since_connect():
            self._gaps.append(now - self.last_report_at)
        self.last_report_at = now

    def _received_since_connect(self) -> bool:
        return (
            self.connected_at is not None
            and self.last_report_at is not None
            and self.last_report_at >= self.connected_at
        )

    def report_interval(self) -> Optional[float]:
        """Median seconds between recent reports, or None before two arrived."""
        return statistics.median(self._gaps) if self._gaps else None

    def stale_after(self) -> float:
        if not self._gaps:
            return STALE_AFTER_SEC
        return max(STALE_AFTER_SEC, 3 * statistics.median(self._gaps))

    def data_stale(self, now: float) -> bool:
        """The last report received is too old to show as current."""
        return (
            self.last_report_at is not None
            and now - self.last_report_at > self.stale_after()
        )

    def status(self, now: float) -> Optional[Tuple[str, str]]:
        """(text, level) for the connection indicator, level being "live",
        "waiting" or "stale"; None while not connected."""
        if self.connected_at is None:
            return None
        if not self._received_since_connect():
            waited = now - self.connected_at
            if waited <= self.stale_after():
                return "Waiting for data…", "waiting"
            return f"No data for {waited:.0f}s", "stale"
        age = now - self.last_report_at
        if age > self.stale_after():
            return f"No data for {age:.0f}s", "stale"
        return "Live", "live"


# ──────────────────────────────────────────────────────────────────────────────
# Main window
# ──────────────────────────────────────────────────────────────────────────────

TOOLBAR_STYLE = f"""
    QToolBar {{
        spacing: 10px; padding: 4px 10px; background: #252525;
        border-bottom: 1px solid {BORDER};
    }}
    QToolBar QLabel {{ color: {TEXT_DIM}; }}
    QToolButton {{
        color: {TEXT}; background: transparent; border: 1px solid #444;
        border-radius: 3px; padding: 3px 10px;
    }}
    QToolButton:hover {{ background: #2f2f2f; }}
    QToolButton:checked {{ background: #333; border-color: {ACCENT}; }}
"""
CONNECTION_STYLE = f"""
    QWidget#connection {{ background: #232323; border-bottom: 1px solid {BORDER}; }}
    QWidget#connection QLabel {{ color: {TEXT_DIM}; }}
    QLineEdit {{
        background: #1a1a1a; color: {TEXT};
        border: 1px solid #444; border-radius: 3px;
        padding: 3px 6px; min-width: 190px;
        font-family: {MONO_FONT_FAMILIES};
    }}
    QLineEdit:focus {{ border-color: {ACCENT}; }}
"""
RECONNECT_STYLE = """
    QPushButton {
        background: #1565c0; color: white; border: none;
        border-radius: 3px; padding: 4px 14px; font-weight: bold;
    }
    QPushButton:hover { background: #1976d2; }
    QPushButton:pressed { background: #0d47a1; }
"""
FILTER_STYLE = f"""
    QLineEdit {{
        background: #1a1a1a; color: {TEXT}; border: 1px solid #3a3a3a;
        border-radius: 3px; padding: 3px 6px;
    }}
    QLineEdit:focus {{ border-color: {ACCENT}; }}
"""
STATUSBAR_STYLE = f"""
    QStatusBar {{ background: #252525; border-top: 1px solid {BORDER}; }}
    QStatusBar::item {{ border: none; }}
    QStatusBar QLabel {{ color: {TEXT_MUTED}; padding: 0 6px; }}
"""


class ProcessMonitorWindow(QMainWindow):
    def __init__(
        self,
        sub_endpoint: str,
        dealer_endpoint: str,
        parent: Optional[QWidget] = None,
        report_endpoint: str = DEFAULT_REPORT_ENDPOINT,
    ):
        super().__init__(parent)
        self.sub_endpoint    = sub_endpoint
        self.dealer_endpoint = dealer_endpoint
        self.report_endpoint = report_endpoint

        self.setWindowTitle("Process Manager – Health Monitor")
        fit_to_screen(self, QSize(1180, 760), minimum=QSize(900, 600))

        self._prev: Dict[str, Tuple[int, int]] = {}   # name → (cpu_usec, snap_ns)
        self._current: Dict[str, dict] = {}
        self._gpu_by_pid: Dict[int, GpuProcessUsage] = {}
        self._log_windows: Dict[str, ProcessLogWindow] = {}
        self._cgroup_members: dict = {}
        self._feed = FeedMonitor()
        self._stale = False  # the pages show data that stopped updating
        self._link_status: Optional[Tuple[str, str, str]] = None
        self._last_gpu_error: Optional[str] = None
        self._gpu_error: Optional[str] = "GPU sampler starting…"
        self._gpu_available = False
        self._gpu_status: Optional[Tuple[str, str, str]] = None
        # The detailed report (port 6668): the latest one, when it came, and
        # its service records by name.
        self._report: Optional[dict] = None
        self._report_at: Optional[float] = None
        self._report_services: Dict[str, dict] = {}
        self._report_state: Optional[Tuple[bool, bool]] = None  # (greyed, current)
        self._usage = UsageHistory()  # recorded from the start, for the graphs
        self._usage_window: Optional[UsageGraphWindow] = None
        self._auto_select = True  # show the first process once reports arrive
        self._link_up = False  # the ZMQ worker has its sockets

        self._build_ui()
        self._feed_timer = QTimer(self)
        self._feed_timer.setInterval(1000)
        self._feed_timer.timeout.connect(self._refresh_feed_status)
        self._feed_timer.start()
        self._start_zmq_worker()
        self._start_systemd_worker()
        self._start_gpu_worker()
        self._start_cgroup_members_worker()
        self._start_detail_journal_worker()

    # ── UI ────────────────────────────────────────────────────────────────

    def _build_ui(self):
        toolbar = QToolBar("Main")
        toolbar.setMovable(False)
        toolbar.setStyleSheet(TOOLBAR_STYLE)
        self.addToolBar(toolbar)

        title = QLabel("Process manager")
        title.setFont(ui_font(1.1, QFont.Weight.Bold))
        title.setStyleSheet(f"color: {TEXT}; padding: 0 4px;")
        toolbar.addWidget(title)

        self.lbl_status = QLabel()
        toolbar.addWidget(self.lbl_status)
        self._set_link_status("Connecting…", "orange")

        # Takes the spare toolbar width and elides, so the pills and the
        # Connection button never fall into the toolbar's overflow menu.
        self.lbl_endpoints = ElidedLabel()
        self.lbl_endpoints.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        )
        self.lbl_endpoints.setStyleSheet(mono_style(TEXT_MUTED))
        toolbar.addWidget(self.lbl_endpoints)

        self.lbl_gpu = QLabel("GPU …")
        toolbar.addWidget(self.lbl_gpu)
        self._refresh_gpu_status()

        self.btn_connection = QToolButton()
        self.btn_connection.setText("Connection")
        self.btn_connection.setCheckable(True)
        self.btn_connection.setToolTip("Show the ZMQ endpoints and reconnect")
        toolbar.addWidget(self.btn_connection)

        # Endpoints strip: shown on demand, and when connecting fails.
        self.connection_strip = QWidget()
        self.connection_strip.setObjectName("connection")
        self.connection_strip.setStyleSheet(CONNECTION_STYLE)
        strip = QHBoxLayout(self.connection_strip)
        strip.setContentsMargins(12, 6, 12, 6)
        strip.setSpacing(8)
        strip.addWidget(QLabel("SUB"))
        self.edit_sub = QLineEdit(self.sub_endpoint)
        self.edit_sub.setPlaceholderText("tcp://host:port")
        self.edit_sub.setToolTip("ZMQ SUB endpoint for health report broadcasts")
        self.edit_sub.setClearButtonEnabled(True)
        self.edit_sub.returnPressed.connect(self._reconnect)
        strip.addWidget(self.edit_sub)
        strip.addSpacing(8)
        strip.addWidget(QLabel("REPORT"))
        self.edit_report = QLineEdit(self.report_endpoint)
        self.edit_report.setPlaceholderText("tcp://host:port")
        self.edit_report.setToolTip(
            "ZMQ SUB endpoint for the manager's detailed report (topic 'report')"
        )
        self.edit_report.setClearButtonEnabled(True)
        self.edit_report.returnPressed.connect(self._reconnect)
        strip.addWidget(self.edit_report)
        strip.addSpacing(8)
        strip.addWidget(QLabel("DEALER"))
        self.edit_dealer = QLineEdit(self.dealer_endpoint)
        self.edit_dealer.setPlaceholderText("tcp://host:port")
        self.edit_dealer.setToolTip(
            "ZMQ DEALER endpoint (ROUTER side). Identity is fixed to 'PMC'"
        )
        self.edit_dealer.setClearButtonEnabled(True)
        self.edit_dealer.returnPressed.connect(self._reconnect)
        strip.addWidget(self.edit_dealer)
        self.btn_reconnect = QPushButton("Reconnect")
        self.btn_reconnect.setStyleSheet(RECONNECT_STYLE)
        self.btn_reconnect.setToolTip("Apply new endpoints and reconnect")
        self.btn_reconnect.clicked.connect(self._reconnect)
        strip.addWidget(self.btn_reconnect)
        wire = ElidedLabel("identity PMC · frames BPM + CommandMessage (65 B)")
        wire.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        wire.setStyleSheet(mono_style(TEXT_MUTED))
        strip.addWidget(wire)
        self.connection_strip.setVisible(False)
        self.btn_connection.toggled.connect(self.connection_strip.setVisible)

        # Sidebar: filter + manager / process list
        side = QWidget()
        side.setStyleSheet(f"background: {SIDEBAR_BG};")
        side_layout = QVBoxLayout(side)
        side_layout.setContentsMargins(8, 8, 8, 8)
        side_layout.setSpacing(8)
        self.sidebar_filter = QLineEdit()
        self.sidebar_filter.setPlaceholderText("Filter processes")
        self.sidebar_filter.setClearButtonEnabled(True)
        self.sidebar_filter.setStyleSheet(FILTER_STYLE)
        side_layout.addWidget(self.sidebar_filter)
        self.sidebar = ProcessSidebar()
        self.sidebar_filter.textChanged.connect(self.sidebar.set_filter)
        self.sidebar.manager_selected.connect(self._on_manager_selected)
        self.sidebar.process_selected.connect(self._on_process_selected)
        side_layout.addWidget(self.sidebar, stretch=1)

        # Detail: one page per kind of selection
        self.detail = ProcessDetailPage(self._usage)
        self.detail.command_requested.connect(self._send_cmd)
        self.detail.pid_activated.connect(self._on_pid_activated)
        self.detail.pop_out_requested.connect(self._open_usage_graphs)
        self.service_page = ServiceDetailPage()
        self.stack = QStackedWidget()
        self.stack.addWidget(self.service_page)
        self.stack.addWidget(self.detail)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setChildrenCollapsible(False)
        splitter.setHandleWidth(1)
        splitter.setStyleSheet(f"QSplitter::handle {{ background: {BORDER}; }}")
        splitter.addWidget(side)
        splitter.addWidget(self.stack)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        side_w = self.fontMetrics().averageCharWidth() * 34
        splitter.setSizes([side_w, side_w * 4])

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.connection_strip)
        layout.addWidget(splitter, stretch=1)
        self.setCentralWidget(central)

        # Status bar: last message at the left, counts at the right
        bar = self.statusBar()
        bar.setStyleSheet(STATUSBAR_STYLE)
        self.lbl_log = QLabel("Ready")
        # Ignored: a long message must not raise the window's minimum width.
        self.lbl_log.setSizePolicy(
            QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred
        )
        bar.addPermanentWidget(self.lbl_log, 1)
        self.lbl_process_count = QLabel("Processes: 0")
        self.lbl_last_update = QLabel("Last update: —")
        bar.addPermanentWidget(self.lbl_process_count)
        bar.addPermanentWidget(self.lbl_last_update)

        self._show_endpoints()
        # Start on the manager page; the first report selects a process.
        self.sidebar.blockSignals(True)
        self.sidebar.select_manager()
        self.sidebar.blockSignals(False)
        self.stack.setCurrentWidget(self.service_page)
        self.sidebar.setFocus()

    def _show_endpoints(self) -> None:
        self.lbl_endpoints.setText(
            f"SUB {self.sub_endpoint}   ·   REPORT {self.report_endpoint}   ·   "
            f"DEALER {self.dealer_endpoint}"
        )

    def _show_status(self, message: str) -> None:
        # One line: multi-line parse errors would make the status bar grow.
        self.lbl_log.setText(" ".join(message.split()))
        self.lbl_log.setToolTip(message)

    # ── selection ─────────────────────────────────────────────────────────

    def _on_process_selected(self, name: str) -> None:
        self._auto_select = False
        self.stack.setCurrentWidget(self.detail)
        report = self._current.get(name)
        self.detail.set_process(
            name, report, self._gpu_for(report, name), self._cgroup_members.get(name) or []
        )
        self.detail.set_details(self._details_for(name), self._report)
        self.detail.set_stale(self._stale)
        worker = getattr(self, "detail_journal_worker", None)
        if worker is not None:
            worker.set_process(name)

    def _on_manager_selected(self) -> None:
        self._auto_select = False
        self.stack.setCurrentWidget(self.service_page)
        worker = getattr(self, "detail_journal_worker", None)
        if worker is not None:
            worker.set_process("")  # nothing to tail while the manager is shown

    def _gpu_for(self, report: Optional[dict], name: str = "") -> Optional[GpuProcessUsage]:
        """GPU use of a process: the manager's figures (NVML on its host, summed
        over the service's processes) when it has them, else local nvidia-smi
        joined by PID, else None."""
        if not report:
            return None
        record = self._details_for(name or report.get("processName", ""))
        if record is not None and record.get("gpuValid"):
            return GpuProcessUsage(
                util_pct=record.get("gpuPercent"), vram_bytes=int(record.get("gpuMemoryBytes", 0))
            )
        if not report.get("pid"):
            return None
        return self._gpu_by_pid.get(report["pid"])

    # ── the detailed report ───────────────────────────────────────────────

    def _report_age_stale(self) -> bool:
        return (
            self._report_at is not None
            and time.monotonic() - self._report_at > self._feed.stale_after()
        )

    def _report_current(self) -> bool:
        """A report arrived recently, or the whole feed is stale (then the last
        report stays on show, greyed out like everything else)."""
        return self._report_at is not None and (self._stale or not self._report_age_stale())

    def _details_for(self, name: str) -> Optional[dict]:
        """The service's record of the detailed report, while the report is current."""
        return self._report_services.get(name) if self._report_current() else None

    def _manager_gpu(self) -> bool:
        """The manager measures GPU use itself (NVML on its host)."""
        return self._report_current() and bool(self._report and self._report.get("gpuMonitoring"))

    def _apply_report_state(self) -> None:
        """Grey the host overview when the report (or the feed) is stale; drop
        the details and the manager's GPU figures once the report is gone."""
        state = (self._stale or self._report_age_stale(), self._report_current())
        if state == self._report_state:
            return
        self._report_state = state
        self.service_page.set_report_stale(state[0])
        self._refresh_gpu_status()
        self._refresh_detail()

    @pyqtSlot(object)
    def _on_report(self, report) -> None:
        if not isinstance(report, dict):
            return
        self._report = report
        self._report_at = time.monotonic()
        self._report_services = {s["name"]: s for s in report.get("services", [])}
        self.service_page.show_report(report)
        self._apply_report_state()
        self._refresh_gpu_status()
        self._refresh_detail()

    def _refresh_detail(self) -> None:
        name = self.detail.name
        if not name or self.stack.currentWidget() is not self.detail:
            return
        report = self._current.get(name)
        if report is not None:
            self.detail.update_report(report, self._gpu_for(report, name))
        self.detail.set_details(self._details_for(name), self._report)

    def _apply_stale(self) -> None:
        self.sidebar.set_stale(self._stale)
        self.detail.set_stale(self._stale)
        self._apply_report_state()

    # ── ZMQ worker lifecycle ──────────────────────────────────────────────

    def _get_endpoints(self) -> tuple[str, str, str]:
        sub    = self.edit_sub.text().strip()    or self.sub_endpoint
        dealer = self.edit_dealer.text().strip() or self.dealer_endpoint
        report = self.edit_report.text().strip() or self.report_endpoint
        return sub, dealer, report

    def _start_zmq_worker(self):
        sub, dealer, report = self._get_endpoints()
        self.sub_endpoint    = sub
        self.dealer_endpoint = dealer
        self.report_endpoint = report
        self.edit_sub.setText(sub)
        self.edit_dealer.setText(dealer)
        self.edit_report.setText(report)
        self._show_endpoints()

        self.worker_thread = QThread()
        self.worker = ZmqWorker(sub, dealer, report_endpoint=report)
        self.worker.moveToThread(self.worker_thread)

        self.worker_thread.started.connect(self.worker.start)
        self.worker.reports_received.connect(self._on_reports)
        self.worker.report_received.connect(self._on_report)
        self.worker.connection_status.connect(self._on_connection_status)
        self.worker.log_message.connect(self._on_log)

        self.worker_thread.start()

    def _stop_zmq_worker(self, timeout_ms: int = 4000) -> None:
        if hasattr(self, "worker") and self.worker is not None:
            for sig in (
                self.worker.reports_received,
                self.worker.report_received,
                self.worker.connection_status,
                self.worker.log_message,
            ):
                try:
                    sig.disconnect()
                except TypeError:
                    pass
            self.worker.stop()
        if hasattr(self, "worker_thread") and self.worker_thread is not None:
            self.worker_thread.quit()
            if not self.worker_thread.wait(timeout_ms):
                self.worker_thread.terminate()
                self.worker_thread.wait(1000)
        self.worker = None
        self.worker_thread = None

    def _stop_background_worker(
        self, worker_attr: str, thread_attr: str, timeout_ms: int
    ) -> None:
        worker = getattr(self, worker_attr, None)
        thread = getattr(self, thread_attr, None)
        if worker is not None:
            for name in ("sampled", "snapshot_ready", "members_ready", "lines_ready"):
                signal = getattr(worker, name, None)
                if signal is None:
                    continue
                try:
                    signal.disconnect()
                except TypeError:
                    pass
            worker.stop()
        if thread is not None:
            thread.quit()
            if not thread.wait(timeout_ms):
                thread.terminate()
                thread.wait(1000)
        setattr(self, worker_attr, None)
        setattr(self, thread_attr, None)

    def _start_cgroup_members_worker(self):
        self.cgroup_members_thread = QThread()
        self.cgroup_members_worker = CgroupMembersWorker()
        self.cgroup_members_worker.moveToThread(self.cgroup_members_thread)
        self.cgroup_members_thread.started.connect(self.cgroup_members_worker.start)
        self.cgroup_members_worker.members_ready.connect(self._on_cgroup_members)
        self.cgroup_members_thread.start()

    def _start_gpu_worker(self):
        self.gpu_thread = QThread()
        self.gpu_worker = GpuSampleWorker()
        self.gpu_worker.moveToThread(self.gpu_thread)
        self.gpu_thread.started.connect(self.gpu_worker.start)
        self.gpu_worker.sampled.connect(self._on_gpu_sampled)
        self.gpu_thread.start()

    def _start_systemd_worker(self):
        self.systemd_thread = QThread()
        self.systemd_worker = SystemdLogWorker()
        self.systemd_worker.moveToThread(self.systemd_thread)
        self.systemd_thread.started.connect(self.systemd_worker.start)
        self.systemd_worker.snapshot_ready.connect(self._on_systemd_snapshot)
        self.systemd_thread.start()

    def _start_detail_journal_worker(self):
        self.detail_journal_thread = QThread()
        self.detail_journal_worker = DetailJournalWorker()
        self.detail_journal_worker.moveToThread(self.detail_journal_thread)
        self.detail_journal_thread.started.connect(self.detail_journal_worker.start)
        self.detail_journal_worker.lines_ready.connect(self._on_detail_journal)
        self.detail_journal_thread.start()
        if self.stack.currentWidget() is self.detail and self.detail.name:
            self.detail_journal_worker.set_process(self.detail.name)

    # ── slots: data in ────────────────────────────────────────────────────

    @pyqtSlot(str, list, str)
    def _on_systemd_snapshot(self, status: str, pairs: list, error: str):
        self.service_page.show_snapshot(status, pairs, error)
        self.sidebar.set_manager_hint(service_active_text(status))

    @pyqtSlot(str, list, str, str)
    def _on_detail_journal(self, name: str, pairs: list, cgroup_path: str, error: str):
        if name and name == self.detail.name:
            self.detail.set_journal(pairs, cgroup_path, error)

    @pyqtSlot(list)
    def _on_reports(self, reports: List[dict]):
        new_current: Dict[str, dict] = {}
        for r in reports:
            name = r["processName"]
            r["_cpu_pct"] = cpu_percent(
                self._prev.get(name), r["cpuUsageInUsec"], r["snapshotTime"]
            )
            self._prev[name] = (r["cpuUsageInUsec"], r["snapshotTime"])
            new_current[name] = r

        for gone in set(self._current) - set(new_current):
            self._prev.pop(gone, None)
        self._current = new_current
        if getattr(self, "cgroup_members_worker", None):
            self.cgroup_members_worker.set_names(sorted(self._current.keys()))

        now = time.monotonic()
        self._feed.on_report(now)
        interval = self._feed.report_interval()
        if interval is not None:
            self._usage.set_report_interval(interval)
        self._record_usage(now)
        if self._stale:
            self._stale = False
            self._apply_stale()
        shown = self.detail.name
        if self.stack.currentWidget() is self.detail and shown and shown not in self._current:
            self.sidebar.select_manager()  # the shown process left the reports
        self.sidebar.set_processes(self._current)
        self._refresh_detail()
        self._auto_select_first()
        self.lbl_last_update.setText(
            f"Last update: {time.strftime('%H:%M:%S')}"
        )
        self.lbl_process_count.setText(f"Processes: {len(self._current)}")
        self._refresh_feed_status()

    def _auto_select_first(self) -> None:
        """Once reports arrive, show the first process the sidebar filter lets through."""
        if not (self._auto_select and self._current):
            return
        visible = [n for n in sorted(self._current) if not self.sidebar.is_hidden(n)]
        if visible:
            self._auto_select = False
            self.sidebar.select_process(visible[0])

    def _record_usage(self, now: float) -> None:
        """Sample every process for the usage graphs and the state band."""
        for name, r in self._current.items():
            gpu = self._gpu_for(r, name)
            if gpu is not None:
                gpu_pct, vram = gpu.util_pct, gpu.vram_bytes
            elif self._gpu_available or self._manager_gpu():
                gpu_pct, vram = 0.0, 0  # measured, and not on the list: using none of it
            else:
                gpu_pct = vram = None
            state = r.get("state")
            self._usage.add(
                name,
                UsageSample(
                    now,
                    r["_cpu_pct"],
                    r["memoryUsageInBytes"],
                    gpu_pct,
                    vram,
                    None if state is None else int(state),
                ),
            )
        self._usage.prune(now)

    @pyqtSlot(str)
    def _on_connection_status(self, status: str):
        self._link_up = status == "connected"
        if status == "connected":
            # Sockets are up; the feed only counts as live once reports arrive.
            self._feed.on_connected(time.monotonic())
            self._refresh_feed_status()
            return
        self._feed.on_disconnected()
        if status.startswith("error"):
            self._set_link_status(status, "#f44336")
            self.btn_connection.setChecked(True)  # show the endpoints to fix them
        else:
            self._set_link_status("Disconnected", "#f44336")

    def _refresh_feed_status(self) -> None:
        now = time.monotonic()
        stale = self._feed.data_stale(now)
        if stale != self._stale:
            self._stale = stale
            self._apply_stale()  # grey out / restore the pages
        self._apply_report_state()
        if self.stack.currentWidget() is self.detail:
            self.detail.tick(now, time.time())
        status = self._feed.status(now)
        if status is None:  # not connected: keep the error / reconnect text
            return
        text, level = status
        tips = {
            "live": f"Receiving health reports from {self.sub_endpoint}",
            "waiting": f"Connected to {self.sub_endpoint}; no health report yet",
            "stale": f"No health reports from {self.sub_endpoint}"
                     + ("; the pages show the last values received (greyed out)"
                        if self._current else ""),
        }
        self._set_link_status(text, FEED_COLORS[level], tips[level])

    def _set_link_status(self, text: str, color: str, tooltip: str = "") -> None:
        if self._link_status == (text, color, tooltip):
            return
        self._link_status = (text, color, tooltip)
        self.lbl_status.setText(text)
        self.lbl_status.setStyleSheet(pill_style(color))
        self.lbl_status.setToolTip(tooltip)

    @pyqtSlot(str)
    def _on_log(self, msg: str):
        self._show_status(msg)

    def gpu_source(self) -> Tuple[str, str, str]:
        """(text, colour, tooltip) of the toolbar pill: where GPU figures come from."""
        if self._manager_gpu():
            return (
                "GPU · manager",
                "#4caf50",
                "GPU figures measured on the manager's host (NVML), summed over each "
                "service's processes"
                + ("; this machine's nvidia-smi is not used" if self._gpu_available else ""),
            )
        if self._gpu_available:
            return (
                "GPU · nvidia-smi",
                "#4caf50",
                "GPU figures from this machine's nvidia-smi, joined by PID: only right "
                "when the GUI runs on the manager's host",
            )
        return (
            "GPU unavailable",
            "#f44336",
            (self._gpu_error or "nvidia-smi unavailable")
            + "; the manager reports no GPU monitoring",
        )

    def _refresh_gpu_status(self) -> None:
        status = self.gpu_source()
        if status == self._gpu_status:
            return
        self._gpu_status = status
        text, color, tooltip = status
        self.lbl_gpu.setText(text)
        self.lbl_gpu.setStyleSheet(pill_style(color))
        self.lbl_gpu.setToolTip(tooltip)

    @pyqtSlot(object, object, bool)
    def _on_gpu_sampled(self, result, err, available: bool):
        new_map: Dict[int, GpuProcessUsage] = result if result else {}
        failed = bool(err) and not new_map
        if failed:
            self._gpu_by_pid = {}
            # Samples come every second; post an error once, not each time.
            if err != self._last_gpu_error:
                self._show_status(f"GPU sample error: {err}")
        else:
            self._gpu_by_pid = new_map
        self._last_gpu_error = err if failed else None
        self._gpu_available = available and not failed
        self._gpu_error = err if isinstance(err, str) else None
        self._refresh_gpu_status()
        self._refresh_detail()

    @pyqtSlot(object)
    def _on_cgroup_members(self, mapping) -> None:
        self._cgroup_members = mapping if isinstance(mapping, dict) else {}
        name = self.detail.name
        if name:
            self.detail.set_members(self._cgroup_members.get(name) or [])

    # ── slots: user actions ───────────────────────────────────────────────

    def _on_pid_activated(self, pid: int, comm: str) -> None:
        self._open_log_window(f"pid:{pid}", pid=pid, comm=comm)

    def _open_log_window(
        self,
        key: str,
        process_name: str = "",
        pid: Optional[int] = None,
        comm: str = "",
    ) -> None:
        existing = self._log_windows.get(key)
        if existing is not None:
            try:
                existing.raise_()
                existing.activateWindow()
                return
            except RuntimeError:
                self._log_windows.pop(key, None)
        win = ProcessLogWindow(process_name=process_name, pid=pid, comm=comm)
        win.destroyed.connect(lambda *_n, k=key: self._log_windows.pop(k, None))
        self._log_windows[key] = win
        win.show()

    def _open_usage_graphs(self) -> None:
        if self._usage_window is not None:
            try:
                self._usage_window.raise_()
                self._usage_window.activateWindow()
                return
            except RuntimeError:  # already deleted
                self._usage_window = None
        win = UsageGraphWindow(self._usage)
        win.destroyed.connect(lambda *_: setattr(self, "_usage_window", None))
        fit_to_screen(win, QSize(1000, 640))
        self._usage_window = win
        win.show()

    def _reconnect(self):
        self.btn_reconnect.setEnabled(False)
        self._feed.on_disconnected()
        self._link_up = False
        self._prev.clear()
        self._forget_report()
        self._set_link_status("Reconnecting…", "orange")

        self._stop_zmq_worker(timeout_ms=4000)
        self._start_zmq_worker()
        self.btn_reconnect.setEnabled(True)
        self._show_status(
            f"Connecting to SUB={self.sub_endpoint}  REPORT={self.report_endpoint}  "
            f"DEALER={self.dealer_endpoint}  (id=PMC)"
        )

    def _forget_report(self) -> None:
        """The next connection starts without a report, like the first one."""
        self._report = None
        self._report_at = None
        self._report_services = {}
        self._report_state = None
        self.service_page.show_report(None)
        self._apply_report_state()

    def _send_cmd(self, cmd: CommandEnum, name: str):
        reply = QMessageBox.question(
            self,
            f"Confirm {cmd.name.title()}",
            f"Are you sure you want to <b>{cmd.name.lower()}</b> process "
            f"<b>{name}</b>?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if reply != QMessageBox.StandardButton.Yes:
            return
        if not getattr(self, "worker", None) or not self._link_up:
            self._show_status("Not connected — command not sent")
            return
        self.worker.send_command(cmd, name, args="")
        # The worker reports "Sent …" once the command is actually handed off.
        self._show_status(f"Sending {cmd.name} for {name}…")

    def closeEvent(self, event):
        for w in list(self._log_windows.values()):
            try:
                w.close()
            except RuntimeError:
                pass
        self._log_windows.clear()
        if self._usage_window is not None:
            try:
                self._usage_window.close()
            except RuntimeError:
                pass
        self._stop_background_worker(
            "cgroup_members_worker", "cgroup_members_thread", 4000
        )
        self._stop_background_worker(
            "detail_journal_worker", "detail_journal_thread", 12000
        )
        # One sample runs nvidia-smi twice, 3 s each at worst.
        self._stop_background_worker("gpu_worker", "gpu_thread", 8000)
        self._stop_background_worker("systemd_worker", "systemd_thread", 12000)
        self._stop_zmq_worker(timeout_ms=4000)
        super().closeEvent(event)


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Process Manager Health Monitor (PyQt6 + ZMQ)"
    )
    parser.add_argument(
        "--sub",
        default=DEFAULT_SUB_ENDPOINT,
        help=f"ZMQ SUB endpoint for the health report (default: {DEFAULT_SUB_ENDPOINT})",
    )
    parser.add_argument(
        "--dealer",
        default=DEFAULT_DEALER_ENDPOINT,
        help=f"ZMQ DEALER endpoint for commands (default: {DEFAULT_DEALER_ENDPOINT})",
    )
    parser.add_argument(
        "--report",
        default=DEFAULT_REPORT_ENDPOINT,
        help=f"ZMQ SUB endpoint for the detailed report (default: {DEFAULT_REPORT_ENDPOINT})",
    )
    args = parser.parse_args()

    app = QApplication(sys.argv)
    apply_dark_theme(app)

    win = ProcessMonitorWindow(args.sub, args.dealer, report_endpoint=args.report)
    win.show()
    sys.exit(app.exec())


def apply_dark_theme(app: QApplication) -> None:
    """Fusion style with the dark palette every window is designed for."""
    app.setStyle("Fusion")
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window,          QColor(30, 30, 30))
    palette.setColor(QPalette.ColorRole.WindowText,      QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Base,            QColor(25, 25, 25))
    palette.setColor(QPalette.ColorRole.AlternateBase,   QColor(40, 40, 40))
    palette.setColor(QPalette.ColorRole.ToolTipBase,     QColor(40, 40, 40))
    palette.setColor(QPalette.ColorRole.ToolTipText,     QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Text,            QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.Button,          QColor(50, 50, 50))
    palette.setColor(QPalette.ColorRole.ButtonText,      QColor(220, 220, 220))
    palette.setColor(QPalette.ColorRole.BrightText,      QColor(255, 0, 0))
    palette.setColor(QPalette.ColorRole.Link,            QColor(42, 130, 218))
    palette.setColor(QPalette.ColorRole.Highlight,       QColor(42, 130, 218))
    palette.setColor(QPalette.ColorRole.HighlightedText, QColor(0, 0, 0))
    app.setPalette(palette)


if __name__ == "__main__":
    main()
