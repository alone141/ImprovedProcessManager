"""What the pages show: the detailed report first, the health record when there
is no current report, one graph sample per manager snapshot, and this machine's
nvidia-smi only as a fallback for a manager that measures no GPU use."""

import random

import pytest

import mock_publisher as mp
import process_monitor_gui as pmg
from health_structs import (
    RuntimeState,
    ServiceState,
    parse_health_reports,
    parse_report_frames,
    report_to_dict,
)
from process_views import DisplayState

SEC = 1_000_000_000
MB = 1024**2


# ── service_view ───────────────────────────────────────────────────────────

HEALTH = {
    "processName": "svc", "pid": 42, "state": RuntimeState.STARTING, "memoryUsageInBytes": 10 * MB,
    "cpuUsageInUsec": 5_000, "start_time": 40 * SEC, "lastSeen": 99 * SEC, "missedBeats": 1,
    "restartCount": 2, "snapshotTime": 100 * SEC, "_cpu_pct": 7.5,
}
RECORD = {
    "name": "svc", "pid": 42, "state": ServiceState.BACKOFF, "memoryBytes": 12 * MB,
    "cpuPercent": 33.0, "startTime": 40 * SEC, "lastSeen": 99 * SEC, "missedBeats": 1,
    "restartCount": 3, "nextRestartTime": 104 * SEC,
}
REPORT = {"snapshotTime": 100 * SEC, "services": [RECORD]}


def test_the_report_record_is_shown_in_the_health_records_keys():
    view = pmg.service_view(HEALTH, RECORD, REPORT)
    assert view == {
        "processName": "svc", "pid": 42, "state": DisplayState.BACKOFF, "memoryUsageInBytes": 12 * MB,
        "_cpu_pct": 33.0, "start_time": 40 * SEC, "lastSeen": 99 * SEC, "missedBeats": 1,
        "restartCount": 3, "snapshotTime": 100 * SEC, "nextRestartTime": 104 * SEC, "source": "report",
    }


def test_the_managers_unknown_cpu_figure_stays_unknown():
    assert pmg.service_view(HEALTH, dict(RECORD, cpuPercent=None), REPORT)["_cpu_pct"] is None


def test_without_a_record_the_health_record_is_shown():
    view = pmg.service_view(HEALTH, None, None)
    assert view["state"] == DisplayState.STARTING and view["source"] == "health"
    assert view["_cpu_pct"] == 7.5 and view["restartCount"] == 2
    assert HEALTH["state"] == RuntimeState.STARTING  # the record itself is left alone


# ── the feed monitor ───────────────────────────────────────────────────────

def test_the_second_socket_of_a_snapshot_proves_the_feed_live_but_is_no_interval():
    feed = pmg.FeedMonitor()
    feed.on_connected(0.0)
    for t in (1.0, 2.0, 3.0):
        feed.on_report(t)  # health record
        feed.on_report(t + 0.001, new_snapshot=False)  # the same snapshot's detailed report
    assert feed.report_interval() == pytest.approx(1.0)
    assert feed.last_report_at == pytest.approx(3.001)


# ── the window ─────────────────────────────────────────────────────────────

class FakeGpuWorker:
    def __init__(self):
        self.paused = []

    def set_paused(self, paused):
        self.paused.append(paused)

    def stop(self):
        pass


@pytest.fixture
def clock(monkeypatch):
    now = [1_000.0]
    monkeypatch.setattr(pmg.time, "monotonic", lambda: now[0])
    return now


@pytest.fixture
def window(app, monkeypatch, clock):
    for starter in (
        "_start_zmq_worker", "_start_systemd_worker", "_start_gpu_worker",
        "_start_cgroup_members_worker", "_start_detail_journal_worker",
    ):
        monkeypatch.setattr(pmg.ProcessMonitorWindow, starter, lambda self: None)
    w = pmg.ProcessMonitorWindow(
        "tcp://127.0.0.1:1", "tcp://127.0.0.1:2", report_endpoint="tcp://127.0.0.1:3"
    )
    w.gpu_worker = FakeGpuWorker()
    w._on_connection_status("connected")
    yield w
    w.close()


def mock():
    return mp.MockManager(rng=random.Random(3), now_ns=1_000 * SEC, interval_ms=1000)


def health(m, t):
    return [report_to_dict(r) for r in parse_health_reports(m.health_frame(t * SEC))]


def detailed(m, t):
    return parse_report_frames(m.report_frames(t * SEC))


def samples(window, name):
    return window._usage.samples(name, 0)


def test_a_manager_with_only_the_health_record_is_shown_as_before(window, clock):
    m = mock()
    window._on_reports(health(m, 1_001))
    clock[0] += 1
    m.advance(1_002 * SEC)
    window._on_reports(health(m, 1_002))
    view = window._current["control_loop"]
    assert view["source"] == "health" and view["state"] == DisplayState.RUNNING
    assert view["_cpu_pct"] is not None  # from the two health records
    assert len(samples(window, "control_loop")) == 2
    assert window.sidebar.process_names() == sorted(mp.PROCESSES)


def test_the_report_wins_and_each_snapshot_is_one_sample(window, clock):
    m = mock()
    for t in (1_001, 1_002, 1_003):
        m.advance(t * SEC)
        window._on_reports(health(m, t))
        clock[0] += 0.002
        window._on_report(detailed(m, t))
        clock[0] += 0.998
    view = window._current["vision_pipeline"]
    assert view["source"] == "report" and view["state"] == pmg.display_state(m.state["vision_pipeline"])
    record = window._report_services["vision_pipeline"]
    assert view["_cpu_pct"] == record["cpuPercent"]
    assert len(samples(window, "vision_pipeline")) == 3
    assert window._feed.report_interval() == pytest.approx(1.0)
    assert window.lbl_status.text() == "Live"
    assert "detailed report" in window.lbl_status.toolTip()


def test_the_report_corrects_a_snapshot_the_health_records_went_first_for(window, clock):
    m = mock()
    m.crash("control_loop", 1_001 * SEC, 3)
    window._on_reports(health(m, 1_001))  # no report yet: the graphs take the health records
    [first] = samples(window, "control_loop")
    assert first.state == int(DisplayState.STARTING)
    clock[0] += 0.003
    window._on_report(detailed(m, 1_001))  # the same snapshot, from port 6668
    [corrected] = samples(window, "control_loop")
    assert corrected.state == int(DisplayState.BACKOFF) and corrected.t == first.t
    window._on_report(detailed(m, 1_001))  # again: nothing more to correct
    assert samples(window, "control_loop") == [corrected]


def test_a_replacing_sample_takes_the_place_of_the_same_snapshots():
    history = pmg.UsageHistory()
    history.add("svc", pmg.UsageSample(10.0, 1.0, 1, None, None, 2))
    history.add("svc", pmg.UsageSample(10.004, 5.0, 1, None, None, 6), replace_since=10.0)
    assert history.samples("svc", 0) == [pmg.UsageSample(10.0, 5.0, 1, None, None, 6)]
    # A service the earlier socket did not have gets its sample appended.
    history.add("new", pmg.UsageSample(10.004, 5.0, 1, None, None, 3), replace_since=10.0)
    assert len(history.samples("new", 0)) == 1
    # An older last sample is kept.
    history.add("svc", pmg.UsageSample(11.5, 7.0, 1, None, None, 3), replace_since=11.0)
    assert [s.t for s in history.samples("svc", 0)] == [10.0, 11.5]


def test_backoff_and_failed_reach_the_pages(window, clock):
    m = mock()
    m.crash("control_loop", 1_001 * SEC, 3)
    m.state["logger_daemon"] = ServiceState.FAILED
    window._on_report(detailed(m, 1_001))
    assert window._current["control_loop"]["state"] == DisplayState.BACKOFF
    window.sidebar.select_process("control_loop")
    assert window.detail.pill.text() == "Backoff 1s"
    assert window.detail.buttons[pmg.CommandEnum.STOP].isEnabled()  # as for "starting" before
    window.sidebar.select_process("logger_daemon")
    assert window.detail.pill.text() == "Failed"
    assert window.detail.buttons[pmg.CommandEnum.START].isEnabled()  # as for "unhealthy" before
    assert samples(window, "control_loop")[-1].state == int(DisplayState.BACKOFF)


def test_when_the_report_stops_the_health_record_takes_over(window, clock):
    m = mock()
    m.crash("control_loop", 1_001 * SEC, 3)
    window._on_reports(health(m, 1_001))
    window._on_report(detailed(m, 1_001))
    assert window._current["control_loop"]["state"] == DisplayState.BACKOFF
    # Port 6668 goes quiet; the health records keep coming.
    for t in range(1_002, 1_010):
        clock[0] += 1
        window._on_reports(health(m, t))
        window._refresh_feed_status()
    view = window._current["control_loop"]
    assert view["source"] == "health"
    assert view["state"] == DisplayState.STARTING  # all the health record can say about backoff
    assert not window._stale and window.lbl_status.text() == "Live"
    assert "no detailed report" in window.lbl_status.toolTip()
    # The report counted until its stale threshold, recording nothing new;
    # since then the health records feed the graphs.
    recorded = samples(window, "control_loop")
    assert recorded[0].state == int(DisplayState.BACKOFF)
    assert recorded[-1].t == clock[0] and recorded[-1].state == int(DisplayState.STARTING)


def test_when_everything_stops_the_last_report_stays_greyed(window, clock):
    m = mock()
    m.state["logger_daemon"] = ServiceState.FAILED
    window._on_reports(health(m, 1_001))
    window._on_report(detailed(m, 1_001))
    clock[0] += 60
    window._refresh_feed_status()
    assert window._stale and window._report_current()
    assert window._current["logger_daemon"]["state"] == DisplayState.FAILED
    # Reports again: live, from the report.
    window._on_reports(health(m, 1_061))
    window._on_report(detailed(m, 1_061))
    assert not window._stale and window._current["logger_daemon"]["source"] == "report"


# ── this machine's nvidia-smi ──────────────────────────────────────────────

def test_nvidia_smi_waits_for_the_report_and_stays_off_while_the_manager_measures(window, clock):
    m = mock()
    assert window.gpu_source()[0] == "GPU · waiting"
    window._update_gpu_sampler()
    assert window.gpu_worker.paused == []  # still paused, as it started
    window._on_report(detailed(m, 1_001))  # the mock measures GPU use
    assert window.gpu_source()[0] == "GPU · manager"
    clock[0] += 60
    window._refresh_feed_status()  # the manager went quiet: the whole feed is stale,
    assert window._stale and window._report_current()  # the last report stays on show
    assert window.gpu_worker.paused == [] and window.gpu_source()[0] == "GPU · manager"


def test_without_the_managers_figures_nvidia_smi_fills_in(window, clock):
    m = mock()
    for t in range(1_001, 1_001 + int(pmg.STALE_AFTER_SEC)):  # an older manager: health only
        clock[0] += 1
        window._on_reports(health(m, t))
        if clock[0] - 1_000 < pmg.STALE_AFTER_SEC:
            assert window.gpu_worker.paused == []  # a report may still come
    assert window.gpu_worker.paused == [False] and window.gpu_source()[0] == "GPU unavailable"
    window._on_gpu_sampled({}, "nvidia-smi not found on PATH", False)
    text, _color, tooltip = window.gpu_source()
    assert text == "GPU unavailable" and "nvidia-smi not found on PATH" in tooltip
    assert "nvidia-smi" not in window.lbl_log.text()  # the status bar is for what the user did
    # The manager's figures arrive after all: nvidia-smi pauses, its results are dropped.
    window._on_report(detailed(m, 1_008))
    assert window.gpu_worker.paused == [False, True] and window.gpu_source()[0] == "GPU · manager"
    window._on_gpu_sampled({42: object()}, None, True)
    assert window._gpu_by_pid == {} and not window._gpu_available
