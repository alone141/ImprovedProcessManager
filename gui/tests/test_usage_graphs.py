"""Usage graphs: history recording, axes, hover values and the window."""

from types import MethodType, SimpleNamespace

import pytest
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor

import health_structs as hs
import process_monitor_gui as pmg
import usage_graphs as ug
from gpu_sampler import GpuProcessUsage
from process_monitor_gui import ProcessMonitorWindow

MB = 1024**2


def sample(t, cpu=1.0, mem=100 * MB, gpu=None, vram=None):
    return ug.UsageSample(t, cpu, mem, gpu, vram)


# ── axes ──────────────────────────────────────────────────────────────────

@pytest.mark.parametrize(
    "max_value, top, step",
    [(37, 40, 10), (100, 100, 25), (10, 10, 2.5), (0.3, 0.3, 0.1), (1234, 1500, 500)],
)
def test_nice_ticks(max_value, top, step):
    got_top, got_step = ug.nice_ticks(max_value)
    assert got_top == pytest.approx(top) and got_step == pytest.approx(step)


def test_nice_ticks_for_zero_has_a_positive_top():
    top, step = ug.nice_ticks(0)
    assert top == step > 0


def test_byte_unit():
    assert ug.byte_unit(500 * MB) == ("MB", MB)
    assert ug.byte_unit(3 * 1024 * MB) == ("GB", 1024 * MB)
    assert ug.byte_unit(100) == ("B", 1.0)


def test_time_step():
    assert ug.time_step(60) == 10
    assert ug.time_step(300) == 60
    assert ug.time_step(900) == 180


def test_split_segments_breaks_on_missing_values_and_gaps():
    points = [(0, 1.0), (1, 2.0), (2, None), (3, 4.0), (20, 5.0), (21, 6.0)]
    assert ug.split_segments(points, gap_sec=5) == [
        [(0, 1.0), (1, 2.0)],
        [(3, 4.0)],
        [(20, 5.0), (21, 6.0)],
    ]


# ── history ───────────────────────────────────────────────────────────────

def test_history_keeps_one_sample_a_second():
    history = ug.UsageHistory()
    for t in (0.0, 0.4, 0.9, 1.0, 1.5, 2.2):
        history.add("a", sample(t))
    assert [s.t for s in history.samples("a", since=0)] == [0.0, 1.0, 2.2]


def test_history_forgets_old_samples_and_gone_processes():
    history = ug.UsageHistory()
    history.add("old", sample(0.0))
    history.add("a", sample(0.0))
    history.add("a", sample(history.KEEP_SEC))
    history.prune(now=history.KEEP_SEC + 1)
    assert history.names() == ["a"]
    assert [s.t for s in history.samples("a", since=0)] == [history.KEEP_SEC]


# ── chart ─────────────────────────────────────────────────────────────────

def cpu_chart():
    return ug.UsageChart(ug.METRICS[0])


def test_values_at_picks_nearest_sample_per_process(app):
    chart = cpu_chart()
    red, blue = QColor("red"), QColor("blue")
    chart.set_data(
        [
            ("a", red, [(10.0, 5.0), (11.0, 7.0), (12.0, None)]),
            ("b", blue, [(10.0, 50.0), (11.0, 60.0)]),
            ("far", red, [(100.0, 1.0)]),  # nothing near t=11.2
        ],
        now=20.0, wall_now=1_000.0, span=60.0,
    )
    assert chart.values_at(11.2) == [("b", blue, 60.0), ("a", red, 7.0)]


@pytest.mark.parametrize("metric", ug.METRICS, ids=lambda m: m.title)
def test_chart_paints_data_gaps_and_empty(app, metric):
    chart = ug.UsageChart(metric)
    chart.resize(400, 200)
    points = [(t, float(t % 7) * MB if metric.is_bytes else float(t % 7)) for t in range(50)]
    points[20] = (20, None)
    chart.set_data([("a", QColor("red"), points)], now=50.0, wall_now=1_000.0, span=60.0)
    assert not chart.grab().isNull()
    chart.set_data([], now=50.0, wall_now=1_000.0, span=60.0)
    assert not chart.grab().isNull()


# ── window ────────────────────────────────────────────────────────────────

def test_window_lists_processes_and_graphs_ticked_ones(app, monkeypatch):
    monkeypatch.setattr(ug.time, "monotonic", lambda: 100.0)
    history = ug.UsageHistory()
    for name in ("b", "a"):
        history.add(name, sample(99.0))
    window = ug.UsageGraphWindow(history)
    names = [window.processes.item(i).text() for i in range(window.processes.count())]
    assert names == ["a", "b"]
    assert window.checked_names() == ["a", "b"]

    window.processes.item(0).setCheckState(Qt.CheckState.Unchecked)  # refreshes
    assert [name for name, _c, _p in window.charts[0].series] == ["b"]

    window._show_only(window.processes.item(0))
    assert window.checked_names() == ["a"]
    window.close()


def test_window_remembers_unticked_process_out_of_view(app, monkeypatch):
    clock = {"now": 1_000.0}
    monkeypatch.setattr(ug.time, "monotonic", lambda: clock["now"])
    history = ug.UsageHistory()
    history.add("a", sample(999.0))
    history.add("b", sample(999.0))
    window = ug.UsageGraphWindow(history)
    window.processes.item(1).setCheckState(Qt.CheckState.Unchecked)  # hide "b"
    assert window.checked_names() == ["a"]

    clock["now"] = 999.0 + 301  # "b" left the 5-minute view...
    history.add("a", sample(clock["now"] - 1))
    window.refresh()
    assert [window.processes.item(i).text() for i in range(window.processes.count())] == ["a"]
    history.add("b", sample(clock["now"] - 1))  # ...and comes back
    window.refresh()
    assert window.checked_names() == ["a"]  # still hidden
    window.close()


def test_window_drops_processes_without_samples_in_view(app, monkeypatch):
    monkeypatch.setattr(ug.time, "monotonic", lambda: 1_000.0)
    history = ug.UsageHistory()
    history.add("recent", sample(990.0))
    history.add("old", sample(100.0))  # outside the default 5 min
    window = ug.UsageGraphWindow(history)
    assert window.checked_names() == ["recent"]
    window.close()


# ── recording from health reports ─────────────────────────────────────────

def recorder(gpu_available, gpu_by_pid, report=None):
    """A stand-in window with the real GPU-source and recording methods; the
    detailed report, when given, is current."""
    window = SimpleNamespace(
        _current={
            "svc": {"pid": 42, "_cpu_pct": 12.5, "memoryUsageInBytes": 300 * MB},
            "stopped": {"pid": 0, "_cpu_pct": 0.0, "memoryUsageInBytes": 0},
        },
        _gpu_by_pid=gpu_by_pid,
        _gpu_available=gpu_available,
        _usage=ug.UsageHistory(),
        _report=report,
        _report_at=None if report is None else 1_000.0,
        _report_services={s["name"]: s for s in (report or {}).get("services", [])},
        _stale=False,
        _feed=pmg.FeedMonitor(),
    )
    for name in ("_gpu_for", "_details_for", "_report_current", "_report_age_stale", "_manager_gpu"):
        setattr(window, name, MethodType(getattr(ProcessMonitorWindow, name), window))
    return window


def manager_report(gpu_valid=True):
    return {
        "gpuMonitoring": True,
        "services": [
            {"name": "svc", "gpuValid": gpu_valid, "gpuPercent": 55.0, "gpuMemoryBytes": 7 * MB},
            {"name": "stopped", "gpuValid": False, "gpuPercent": None, "gpuMemoryBytes": 0},
        ],
    }


def test_records_cpu_memory_and_gpu(monkeypatch):
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0)
    window = recorder(True, {42: GpuProcessUsage(util_pct=30.0, vram_bytes=2 * MB)})
    ProcessMonitorWindow._record_usage(window, 5.0)
    assert window._usage.samples("svc", 0) == [ug.UsageSample(5.0, 12.5, 300 * MB, 30.0, 2 * MB)]
    # Not using the GPU while nvidia-smi works: zero, not "no data"
    assert window._usage.samples("stopped", 0) == [ug.UsageSample(5.0, 0.0, 0, 0.0, 0)]


def test_records_no_gpu_data_without_nvidia_smi(monkeypatch):
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0)
    window = recorder(False, {})
    ProcessMonitorWindow._record_usage(window, 5.0)
    [only] = window._usage.samples("svc", 0)
    assert only.gpu_pct is None and only.vram_bytes is None


def test_the_managers_gpu_figures_win_over_nvidia_smi(monkeypatch):
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0)
    window = recorder(True, {42: GpuProcessUsage(util_pct=30.0, vram_bytes=2 * MB)}, manager_report())
    assert window._manager_gpu()
    assert window._gpu_for(window._current["svc"], "svc") == GpuProcessUsage(util_pct=55.0, vram_bytes=7 * MB)
    ProcessMonitorWindow._record_usage(window, 5.0)
    assert window._usage.samples("svc", 0) == [ug.UsageSample(5.0, 12.5, 300 * MB, 55.0, 7 * MB)]
    # No figures for a stopped service under a GPU-monitoring manager: zero, not "no data"
    assert window._usage.samples("stopped", 0) == [ug.UsageSample(5.0, 0.0, 0, 0.0, 0)]


def test_without_manager_figures_for_a_service_nvidia_smi_fills_in(monkeypatch):
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0)
    window = recorder(True, {42: GpuProcessUsage(util_pct=30.0, vram_bytes=2 * MB)}, manager_report(gpu_valid=False))
    assert window._gpu_for(window._current["svc"], "svc") == GpuProcessUsage(util_pct=30.0, vram_bytes=2 * MB)


def test_a_gpu_figure_that_is_not_finite_is_recorded_as_no_data(monkeypatch):
    # On the graph's axis it would make nice_ticks raise while painting.
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0)
    raw = hs.encode_detailed_report(
        {"gpuMonitoring": True, "services": [{"name": "svc", "gpuValid": True, "gpuPercent": float("inf")}]}
    )
    window = recorder(True, {}, hs.parse_detailed_report(raw))
    ProcessMonitorWindow._record_usage(window, 5.0)
    [only] = window._usage.samples("svc", 0)
    assert only.gpu_pct is None


def test_a_report_that_stopped_arriving_no_longer_counts(monkeypatch):
    monkeypatch.setattr(pmg.time, "monotonic", lambda: 1_000.0 + pmg.STALE_AFTER_SEC + 1)
    window = recorder(False, {}, manager_report())
    assert not window._report_current() and not window._manager_gpu()
    assert window._details_for("svc") is None
    assert window._gpu_for(window._current["svc"], "svc") is None
    window._stale = True  # the whole feed is stale: the last report stays on show
    assert window._report_current() and window._details_for("svc")["name"] == "svc"


def test_history_gap_follows_the_report_interval():
    history = ug.UsageHistory()
    assert history.gap_sec == ug.UsageHistory.MIN_GAP_SEC
    history.set_report_interval(10.0)
    assert history.gap_sec == 25.0
    history.set_report_interval(0.5)
    assert history.gap_sec == ug.UsageHistory.MIN_GAP_SEC


def test_chart_joins_samples_as_far_apart_as_the_report_interval(app):
    chart = cpu_chart()
    red = QColor("red")
    series = [("a", red, [(10.0, 5.0), (20.0, 7.0)])]
    chart.set_data(series, now=30.0, wall_now=1_000.0, span=60.0)
    assert chart.values_at(14.0) == []  # 4 s from a sample: past half the default gap
    chart.set_data(series, now=30.0, wall_now=1_000.0, span=60.0, gap_sec=25.0)
    assert chart.values_at(14.0) == [("a", red, 5.0)]
