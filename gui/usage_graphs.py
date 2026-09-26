"""Usage graphs: CPU, memory and GPU use per process over recent time.

Samples are recorded by the main window from health reports (GPU columns
from the manager's detailed report, or from nvidia-smi without one). The
charts are drawn with QPainter, so graphing needs no dependency beyond PyQt6.
"""

from __future__ import annotations

import dataclasses
import html
import math
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Deque, Dict, List, Optional, Sequence, Tuple

from PyQt6.QtCore import QPointF, QRectF, Qt, QTimer
from PyQt6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PyQt6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPushButton,
    QSplitter,
    QToolTip,
    QVBoxLayout,
    QWidget,
)

from health_structs import format_bytes

SERIES_COLORS = tuple(
    QColor(c)
    for c in (
        "#4dd0e1", "#ffd54f", "#81c784", "#f48fb1", "#64b5f6",
        "#ffb74d", "#ce93d8", "#ff8a65", "#aed581", "#4fc3f7",
    )
)
CHART_BG = QColor("#1e1e1e")
GRID_COLOR = QColor("#333333")
AXIS_COLOR = QColor("#9e9e9e")
TITLE_COLOR = QColor("#dddddd")
HOVER_COLOR = QColor("#aaaaaa")


# ──────────────────────────────────────────────────────────────────────────────
# History
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class UsageSample:
    t: float  # time.monotonic()
    cpu_pct: Optional[float]
    mem_bytes: Optional[int]
    gpu_pct: Optional[float]  # None: no GPU data
    vram_bytes: Optional[int]
    state: Optional[int] = None  # process_views.DisplayState value; None when not recorded


class UsageHistory:
    """Recent samples per process. Recording starts with the app, so graphs
    opened later still show the last few minutes."""

    KEEP_SEC = 15 * 60.0
    MIN_INTERVAL_SEC = 1.0  # at most one sample a second per process
    MIN_GAP_SEC = 5.0  # samples further apart than gap_sec are not joined

    def __init__(self) -> None:
        self._series: Dict[str, Deque[UsageSample]] = {}
        self.gap_sec = self.MIN_GAP_SEC

    def set_report_interval(self, seconds: float) -> None:
        """A manager may publish every 10 s or more; only a silence of more than
        two and a half report intervals counts as a hole in the graphs."""
        self.gap_sec = max(self.MIN_GAP_SEC, 2.5 * seconds)

    def add(self, name: str, sample: UsageSample, replace_since: Optional[float] = None) -> None:
        """Append ``sample``, at most one a second. With ``replace_since``, it
        takes the place of a last sample taken at or after that time instead
        (the same manager snapshot, recorded a moment before from the other
        socket), at that sample's time."""
        series = self._series.setdefault(name, deque())
        if replace_since is not None and series and series[-1].t >= replace_since:
            series[-1] = dataclasses.replace(sample, t=series[-1].t)
            return
        if series and sample.t - series[-1].t < self.MIN_INTERVAL_SEC:
            return
        series.append(sample)

    def prune(self, now: float) -> None:
        """Forget samples older than KEEP_SEC, and processes left with none."""
        for name in list(self._series):
            series = self._series[name]
            while series and now - series[0].t > self.KEEP_SEC:
                series.popleft()
            if not series:
                del self._series[name]

    def names(self) -> List[str]:
        return sorted(self._series)

    def samples(self, name: str, since: float) -> List[UsageSample]:
        return [s for s in self._series.get(name, ()) if s.t >= since]


# ──────────────────────────────────────────────────────────────────────────────
# Axis helpers
# ──────────────────────────────────────────────────────────────────────────────

def nice_ticks(max_value: float, target: int = 4) -> Tuple[float, float]:
    """A round axis top and tick step for 0..max_value: about ``target``
    steps of 1, 2, 2.5 or 5 × 10^n."""
    raw = max(max_value, 1e-9) / target
    magnitude = 10 ** math.floor(math.log10(raw))
    step = next(
        m * magnitude for m in (1, 2, 2.5, 5, 10) if m * magnitude >= raw * (1 - 1e-9)
    )
    top = step * math.ceil(max_value / step - 1e-9)
    return max(top, step), step


def byte_unit(max_bytes: float) -> Tuple[str, float]:
    for unit, size in (("GB", 1024.0**3), ("MB", 1024.0**2), ("KB", 1024.0)):
        if max_bytes >= size:
            return unit, size
    return "B", 1.0


_TIME_STEPS = (5, 10, 15, 30, 60, 120, 180, 300, 600, 900, 1800)


def time_step(span_sec: float, max_ticks: int = 6) -> int:
    """Seconds between time-axis labels."""
    return next((s for s in _TIME_STEPS if span_sec / s <= max_ticks), _TIME_STEPS[-1])


def split_segments(
    points: List[Tuple[float, Optional[float]]], gap_sec: float
) -> List[List[Tuple[float, float]]]:
    """Split (t, value) points into runs drawn as connected lines: a missing
    value, or samples further apart than gap_sec, end a run."""
    runs: List[List[Tuple[float, float]]] = []
    run: List[Tuple[float, float]] = []
    prev_t: Optional[float] = None
    for t, v in points:
        if run and (v is None or t - prev_t > gap_sec):
            runs.append(run)
            run = []
        if v is not None:
            run.append((t, v))
        prev_t = t
    if run:
        runs.append(run)
    return runs


def state_segments(
    samples: Sequence[UsageSample], since: float, now: float, gap_sec: float = 5.0
) -> List[Tuple[float, float, int]]:
    """Runs of one state over [since, now], as (start, end, state).

    Consecutive samples with the same state form one run. A gap longer than
    ``gap_sec`` ends the run at the last sample seen; a state change ends it
    at the sample that changed. The final run reaches ``now`` only while the
    last sample is recent, so a process that stopped reporting shows a hole.
    """
    runs: List[Tuple[float, float, int]] = []
    start = last_t = 0.0
    state: Optional[int] = None
    for s in samples:
        if s.state is None:
            continue
        t = max(s.t, since)
        if state is None:
            start, state = t, s.state
        elif s.t - last_t > gap_sec:
            runs.append((start, last_t, state))
            start, state = t, s.state
        elif s.state != state:
            runs.append((start, t, state))
            start, state = t, s.state
        last_t = s.t
    if state is not None:
        end = now if now - last_t <= gap_sec else last_t
        runs.append((start, max(end, start), state))
    return runs


# ──────────────────────────────────────────────────────────────────────────────
# Chart
# ──────────────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Metric:
    title: str
    value: Callable[[UsageSample], Optional[float]]
    is_bytes: bool  # else a percentage
    floor: float  # smallest axis top, so idle noise stays flat
    empty_text: str


NO_GPU_TEXT = "No GPU data (no figures from the manager, nvidia-smi unavailable)"
METRICS = (
    Metric("CPU %  (100 = one core)", lambda s: s.cpu_pct, False, 10.0, "No data yet"),
    Metric("Memory", lambda s: s.mem_bytes, True, 16 * 1024.0**2, "No data yet"),
    Metric("GPU %", lambda s: s.gpu_pct, False, 10.0, NO_GPU_TEXT),
    Metric("VRAM", lambda s: s.vram_bytes, True, 16 * 1024.0**2, NO_GPU_TEXT),
)

Series = Tuple[str, QColor, List[Tuple[float, Optional[float]]]]


def build_series(
    samples: Dict[str, List[UsageSample]],
    names: Sequence[str],
    color_for: Callable[[str], QColor],
    metric: Metric,
) -> List[Series]:
    """One (name, colour, points) series per name for a chart's metric."""
    return [
        (name, color_for(name), [(s.t, metric.value(s)) for s in samples.get(name, ())])
        for name in names
    ]


class UsageChart(QWidget):
    """Line chart of one metric, one line per process, over recent time.
    Hovering shows every process's value at that moment."""

    GAP_SEC = 5.0  # samples further apart than this aren't joined

    def __init__(self, metric: Metric, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.metric = metric
        self.series: List[Series] = []
        self._now = 0.0  # monotonic time at the right edge
        self._wall_now = 0.0  # wall-clock time at the right edge
        self._span = 300.0
        self.gap_sec = self.GAP_SEC
        self._plot = QRectF()
        self._hover_x: Optional[float] = None
        self.setMouseTracking(True)
        fm = self.fontMetrics()
        # Title, four tick labels and the time axis fit in eight lines.
        self.setMinimumSize(fm.averageCharWidth() * 36, fm.height() * 8)

    def set_data(
        self,
        series: List[Series],
        now: float,
        wall_now: float,
        span: float,
        gap_sec: Optional[float] = None,
    ) -> None:
        self.series = series
        self._now, self._wall_now, self._span = now, wall_now, span
        if gap_sec is not None:
            self.gap_sec = gap_sec
        self.update()

    def format_value(self, value: float) -> str:
        if self.metric.is_bytes:
            return format_bytes(int(value))
        return f"{value:.1f} %"

    def values_at(self, t: float) -> List[Tuple[str, QColor, float]]:
        """Each series' sample nearest to time t, largest value first."""
        rows = []
        for name, color, points in self.series:
            best: Optional[Tuple[float, float]] = None
            for pt_t, value in points:
                if value is None:
                    continue
                distance = abs(pt_t - t)
                if distance <= self.gap_sec / 2 and (best is None or distance < best[0]):
                    best = (distance, value)
            if best is not None:
                rows.append((name, color, best[1]))
        rows.sort(key=lambda row: -row[2])
        return rows

    # ── painting ─────────────────────────────────────────────────────────

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.fillRect(self.rect(), CHART_BG)
        fm = self.fontMetrics()
        line_h = fm.height()
        pad = line_h * 0.6

        title_font = QFont(self.font())
        title_font.setBold(True)
        p.setFont(title_font)
        p.setPen(TITLE_COLOR)
        p.drawText(
            QRectF(pad, pad * 0.5, self.width() - 2 * pad, line_h),
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
            self.metric.title,
        )
        p.setFont(self.font())

        values = [v for _n, _c, pts in self.series for _t, v in pts if v is not None]
        top_value = max([self.metric.floor, *values])
        unit, size = byte_unit(top_value) if self.metric.is_bytes else ("%", 1.0)
        top, step = nice_ticks(top_value / size)
        labels = [f"{i * step:g} {unit}" for i in range(round(top / step) + 1)]
        label_w = max(fm.horizontalAdvance(label) for label in labels)

        plot = QRectF()
        plot.setLeft(pad + label_w + pad * 0.5)
        plot.setTop(pad * 1.5 + line_h)
        plot.setRight(self.width() - pad)
        plot.setBottom(self.height() - pad * 0.5 - line_h)
        self._plot = plot
        if plot.width() < 10 or plot.height() < 10:
            return

        for i, label in enumerate(labels):
            y = plot.bottom() - i * step / top * plot.height()
            p.setPen(GRID_COLOR)
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))
            p.setPen(AXIS_COLOR)
            p.drawText(
                QRectF(0, y - line_h / 2, plot.left() - pad * 0.5, line_h),
                Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                label,
            )

        tick = time_step(self._span)
        clock = "%H:%M:%S" if tick < 60 else "%H:%M"
        wall = math.ceil((self._wall_now - self._span) / tick) * tick
        label_end = -math.inf
        while wall <= self._wall_now:
            x = plot.right() - (self._wall_now - wall) / self._span * plot.width()
            p.setPen(GRID_COLOR)
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
            text = time.strftime(clock, time.localtime(wall))
            text_w = fm.horizontalAdvance(text)
            text_x = min(max(x - text_w / 2, plot.left()), plot.right() - text_w)
            # Edge labels are pulled inside the plot; skip one that would collide.
            if text_x >= label_end + fm.averageCharWidth():
                p.setPen(AXIS_COLOR)
                p.drawText(QPointF(text_x, plot.bottom() + pad * 0.3 + fm.ascent()), text)
                label_end = text_x + text_w
            wall += tick

        if not values:
            p.setPen(AXIS_COLOR)
            p.drawText(plot, Qt.AlignmentFlag.AlignCenter, self.metric.empty_text)
            return

        p.setClipRect(plot)
        for _name, color, points in self.series:
            p.setPen(QPen(color, 1.5))
            for run in split_segments(points, self.gap_sec):
                mapped = [self._to_plot(t, v / size, top) for t, v in run]
                if len(mapped) == 1:
                    p.setBrush(color)
                    p.drawEllipse(mapped[0], 1.5, 1.5)
                    p.setBrush(Qt.BrushStyle.NoBrush)
                    continue
                path = QPainterPath(mapped[0])
                for point in mapped[1:]:
                    path.lineTo(point)
                p.drawPath(path)
        p.setClipping(False)

        if self._hover_x is not None:
            p.setPen(QPen(HOVER_COLOR, 1, Qt.PenStyle.DashLine))
            p.drawLine(
                QPointF(self._hover_x, plot.top()), QPointF(self._hover_x, plot.bottom())
            )

    def _to_plot(self, t: float, value: float, top: float) -> QPointF:
        plot = self._plot
        x = plot.right() - (self._now - t) / self._span * plot.width()
        y = plot.bottom() - value / top * plot.height()
        return QPointF(x, y)

    # ── hover ────────────────────────────────────────────────────────────

    def mouseMoveEvent(self, event) -> None:
        pos = event.position()
        if not self._plot.contains(pos):
            self._clear_hover()
            return
        t = self._now - (self._plot.right() - pos.x()) / self._plot.width() * self._span
        self._hover_x = pos.x()
        self.update()
        rows = self.values_at(t)
        if not rows:
            QToolTip.hideText()
            return
        when = time.strftime("%H:%M:%S", time.localtime(self._wall_now - (self._now - t)))
        lines = [
            f'<span style="color:{color.name()}">■</span> '
            f"{html.escape(name)}: {self.format_value(value)}"
            for name, color, value in rows[:12]
        ]
        QToolTip.showText(
            event.globalPosition().toPoint(), f"<b>{when}</b><br>" + "<br>".join(lines), self
        )

    def leaveEvent(self, event) -> None:
        self._clear_hover()
        super().leaveEvent(event)

    def _clear_hover(self) -> None:
        if self._hover_x is not None:
            self._hover_x = None
            self.update()
        QToolTip.hideText()


# ──────────────────────────────────────────────────────────────────────────────
# Window
# ──────────────────────────────────────────────────────────────────────────────

class UsageGraphWindow(QMainWindow):
    """CPU, memory, GPU and VRAM charts for the processes picked on the left."""

    SPANS = (("Last 1 min", 60), ("Last 5 min", 300), ("Last 15 min", 900))

    def __init__(self, history: UsageHistory, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self.history = history
        self._colors: Dict[str, QColor] = {}
        self._hidden: set = set()  # unticked names, kept while out of view
        self.setWindowTitle("Resource usage")
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)

        side = QWidget()
        side_l = QVBoxLayout(side)
        side_l.setContentsMargins(0, 0, 0, 0)
        self.span = QComboBox()
        for label, seconds in self.SPANS:
            self.span.addItem(label, seconds)
        self.span.setCurrentIndex(1)
        self.span.currentIndexChanged.connect(self.refresh)
        side_l.addWidget(self.span)
        side_l.addWidget(QLabel("Processes"))
        self.processes = QListWidget()
        self.processes.setToolTip("Tick the processes to graph; double-click to show only one")
        # The default check box is invisible on the dark palette; draw an
        # outline, filled when ticked.
        box = max(10, self.fontMetrics().height() - 4)
        self.processes.setStyleSheet(f"""
            QListWidget::indicator {{
                width: {box}px; height: {box}px; border-radius: 3px;
                border: 1px solid #777; background: #1a1a1a;
            }}
            QListWidget::indicator:checked {{ background: #3a7bd5; border-color: #3a7bd5; }}
        """)
        self.processes.itemChanged.connect(self.refresh)
        self.processes.itemDoubleClicked.connect(self._show_only)
        side_l.addWidget(self.processes, stretch=1)
        buttons = QHBoxLayout()
        for text, checked in (("All", True), ("None", False)):
            button = QPushButton(text)
            button.clicked.connect(lambda _=False, c=checked: self._check_all(c))
            buttons.addWidget(button)
        side_l.addLayout(buttons)

        charts = QWidget()
        grid = QGridLayout(charts)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(6)
        self.charts = [UsageChart(metric) for metric in METRICS]
        for i, chart in enumerate(self.charts):
            grid.addWidget(chart, i // 2, i % 2)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(side)
        splitter.addWidget(charts)
        splitter.setStretchFactor(1, 1)
        side_w = self.fontMetrics().averageCharWidth() * 24
        splitter.setSizes([side_w, 5 * side_w])
        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.addWidget(splitter)
        self.setCentralWidget(central)
        self.statusBar().showMessage(
            "Hover a chart to read values. One sample a second per process; "
            "the last 15 minutes are kept."
        )

        self._timer = QTimer(self)
        self._timer.setInterval(1000)
        self._timer.timeout.connect(self.refresh)
        self._timer.start()
        self.refresh()

    def color(self, name: str) -> QColor:
        if name not in self._colors:
            self._colors[name] = SERIES_COLORS[len(self._colors) % len(SERIES_COLORS)]
        return self._colors[name]

    def checked_names(self) -> List[str]:
        items = (self.processes.item(i) for i in range(self.processes.count()))
        return [it.text() for it in items if it.checkState() == Qt.CheckState.Checked]

    def refresh(self) -> None:
        now, wall_now = time.monotonic(), time.time()
        span = float(self.span.currentData())
        samples = {name: self.history.samples(name, now - span) for name in self.history.names()}
        self._sync_list([name for name, s in samples.items() if s])
        shown = self.checked_names()
        listed = {self.processes.item(i).text() for i in range(self.processes.count())}
        self._hidden = (self._hidden - set(shown)) | (listed - set(shown))
        for chart in self.charts:
            chart.set_data(
                build_series(samples, shown, self.color, chart.metric),
                now,
                wall_now,
                span,
                self.history.gap_sec,
            )

    def _sync_list(self, names: List[str]) -> None:
        """List the processes with samples in view; new ones start ticked
        unless they were unticked before dropping out of view."""
        self.processes.blockSignals(True)
        try:
            for i in reversed(range(self.processes.count())):
                if self.processes.item(i).text() not in names:
                    self.processes.takeItem(i)
            listed = {self.processes.item(i).text() for i in range(self.processes.count())}
            for name in names:
                if name in listed:
                    continue
                item = QListWidgetItem(name)
                item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                item.setCheckState(
                    Qt.CheckState.Unchecked if name in self._hidden else Qt.CheckState.Checked
                )
                self.processes.addItem(item)
            self.processes.sortItems()
            # Names double as the legend; hidden ones are greyed out.
            for i in range(self.processes.count()):
                item = self.processes.item(i)
                shown = item.checkState() == Qt.CheckState.Checked
                item.setForeground(self.color(item.text()) if shown else AXIS_COLOR)
        finally:
            self.processes.blockSignals(False)

    def _set_checked(self, keep: Callable[[QListWidgetItem], bool]) -> None:
        """Tick the items ``keep`` accepts, untick the rest, refresh once."""
        self.processes.blockSignals(True)
        try:
            for i in range(self.processes.count()):
                item = self.processes.item(i)
                item.setCheckState(
                    Qt.CheckState.Checked if keep(item) else Qt.CheckState.Unchecked
                )
        finally:
            self.processes.blockSignals(False)
        self.refresh()

    def _check_all(self, checked: bool) -> None:
        self._set_checked(lambda _item: checked)

    def _show_only(self, item: QListWidgetItem) -> None:
        self._set_checked(lambda other: other is item)
