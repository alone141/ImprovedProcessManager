"""The details panel and the host overview: text from the detailed report,
rows that come and go, the hint without a report, and greying."""

import process_views as pv
import usage_graphs as ug
from health_structs import RestartMode, ServiceState

SEC = 1_000_000_000
MB = 1024**2
GB = 1024**3


def record(**overrides):
    base = {
        "name": "vision",
        "binary": "/opt/beray/bin/vision",
        "description": "Camera capture",
        "pid": 42,
        "state": ServiceState.RUNNING,
        "restartMode": RestartMode.ON_FAILURE,
        "autostart": True,
        "heartbeat": True,
        "cgroup": True,
        "usageValid": True,
        "gpuValid": True,
        "removing": False,
        "restartCount": 2,
        "missedBeats": 0,
        "lastExitCode": -9,
        "processCount": 1,
        "threadCount": 12,
        "openFiles": 34,
        "startTime": 89 * SEC,
        "lastSeen": 99 * SEC,
        "lastExitTime": 80 * SEC,
        "nextRestartTime": 0,
        "cpuTimeUsec": 0,
        "cpuPercent": 4.0,
        "memoryBytes": 10 * MB,
        "memoryPeakBytes": 20 * MB,
        "memoryLimitBytes": GB,
        "ioReadBytes": 3 * MB,
        "ioWriteBytes": MB,
        "gpuPercent": 30.5,
        "gpuMemoryBytes": 210 * MB,
        "oomKills": 0,
        "cpuLimitPercent": 200,
    }
    base.update(overrides)
    return base


def report(**overrides):
    base = {
        "managerPid": 12246,
        "snapshotTime": 100 * SEC,
        "managerStartTime": 89 * SEC,
        "publishIntervalMs": 1000,
        "cgroups": True,
        "gpuMonitoring": True,
        "stopping": False,
        "windows": False,
        "hostCpuPercent": 16.25,
        "memoryTotalBytes": 16 * GB,
        "memoryAvailableBytes": 3 * GB,
        "loadAverage": (0.52, 0.58, 0.59),
        "uptimeSeconds": 4000,
        "cpuCount": 16,
        "hostName": "rig-01",
        "managerVersion": "1.0.0",
        "services": [record(), record(name="planner", state=ServiceState.FAILED)],
        "gpus": [{
            "name": "NVIDIA RTX A4000", "uuid": "GPU-0123", "index": 0, "temperatureC": 61,
            "utilizationPercent": 34.0, "memoryUtilizationPercent": 19.0,
            "memoryTotalBytes": 16 * GB, "memoryUsedBytes": 3 * GB, "powerMilliwatts": 74_000,
        }],
    }
    base.update(overrides)
    return base


# ── text ─────────────────────────────────────────────────────────────────────

def test_details_read_like_the_cli():
    values = pv.details_values(record(), report())
    assert values == {
        "state": "running",
        "binary": "/opt/beray/bin/vision",
        "description": "Camera capture",
        "restart": "on-failure · autostart",
        "exit": "signal 9 (KILL), 20s ago",
        "procs": "1 · 12 · 34",
        "io": "read 3.0 MB · written 1.0 MB",
        "peak": "20.0 MB",
        "limits": "memory 1.0 GB · CPU 200 % of one core",
        "accounting": "cgroup task_vision",
        "heartbeat": "supervised · last beat 1s ago",
        "gpu": "30.5 % · 210.0 MB",
    }


def test_details_of_a_service_in_backoff_without_figures():
    values = pv.details_values(
        record(
            state=ServiceState.BACKOFF, nextRestartTime=102 * SEC + SEC // 2, usageValid=False,
            gpuValid=False, heartbeat=False, cgroup=False, memoryLimitBytes=0, cpuLimitPercent=0,
            description="", lastExitTime=0, missedBeats=0, autostart=False,
        ),
        report(),
    )
    assert values["state"] == "backoff 2s" and values["next"] == "in 2s"
    assert values["exit"] == "never" and values["restart"] == "on-failure"
    assert values["limits"] == "none" and values["accounting"] == "session and descendants"
    assert values["heartbeat"] == "not supervised"
    assert values["gpu"] == "no GPU figures for this service"
    assert "description" not in values and "procs" not in values and "io" not in values


def test_details_show_oom_kills_missed_beats_and_windows_exit_codes():
    values = pv.details_values(
        record(oomKills=2, missedBeats=3, state=ServiceState.UNHEALTHY, lastExitCode=-1073741819),
        report(windows=True, gpuMonitoring=False),
    )
    assert values["state"] == "unhealthy (3 missed)"
    assert values["accounting"] == "cgroup task_vision · 2 OOM kills"
    assert values["heartbeat"].endswith("· 3 missed")
    assert values["exit"].startswith("exit 0xC0000005")
    assert "gpu" in values  # the record has figures, whatever the header says


def test_host_overview_reads_like_the_cli():
    values = pv.host_values(report())
    assert values["host"] == "rig-01"
    assert values["manager"] == "v1.0.0 · PID 12246 · up 11s"
    assert values["publish"].startswith("every 1.0 s · snapshot ")
    assert values["accounting"] == "cgroups · GPU monitoring (NVML)"
    assert values["cpu"] == "16.2 % of 16 cores"
    assert values["memory"] == "13.0 GB used of 16.0 GB"
    assert values["load"] == "0.52 0.58 0.59"
    assert values["uptime"] == "1h 06m"
    assert values["services"] == "2 · failed 1 · running 1"
    assert values["gpus"] == "gpu0 · NVIDIA RTX A4000 · 34 % busy · 3.0 GB of 16.0 GB · 61 °C · 74 W"


def test_host_overview_on_a_windows_host_without_gpus():
    values = pv.host_values(
        report(windows=True, cgroups=False, gpuMonitoring=False, gpus=[], loadAverage=(0.0, 0.0, 0.0),
               hostCpuPercent=None, stopping=True, services=[])
    )
    assert values["accounting"] == "sessions (no cgroups) · no GPU monitoring · Windows host"
    assert values["cpu"] == "— of 16 cores" and values["manager"].endswith("· shutting down")
    assert "load" not in values and "gpus" not in values
    assert values["services"] == "0"
    assert pv.host_values(report(gpus=[]))["gpus"] == "none found"


# ── widgets ──────────────────────────────────────────────────────────────────

def test_key_value_panel_shows_only_the_rows_it_has(app):
    panel = pv.KeyValuePanel("Title", (("a", "A"), ("b", "B"), ("c", "C")), "nothing yet")
    assert panel.shown_values() == {} and panel.lbl_hint.isVisibleTo(panel)
    panel.set_values({"c": "3", "a": "1"})
    assert panel.shown_values() == {"a": "1", "c": "3"}  # in row order, b hidden
    assert not panel.lbl_hint.isVisibleTo(panel)
    assert panel.labels["b"].isHidden() and not panel.labels["c"].isHidden()
    panel.set_values({"a": "10", "b": "2", "c": "30"})
    assert panel.shown_values() == {"a": "10", "b": "2", "c": "30"}
    panel.set_values(None)
    assert panel.shown_values() == {} and panel.lbl_hint.isVisibleTo(panel)
    panel.set_stale(True)
    assert all(pv.STALE_COLOR.name() in v.styleSheet() for v in panel.values.values())


def test_process_page_details_follow_the_report(app):
    page = pv.ProcessDetailPage(ug.UsageHistory())
    page.set_process("vision", None, None, [])
    assert page.details.shown_values() == {}
    assert page.details.lbl_hint.text() == pv.NO_REPORT_TEXT
    page.set_details(record(), report())
    assert page.details.shown_values()["state"] == "running"
    assert page.details.shown_values()["gpu"] == "30.5 % · 210.0 MB"
    page.set_details(None, report())  # the report has no record for it (or went away)
    assert page.details.shown_values() == {}
    page.set_details(record(), report())
    page.set_process("other", None, None, [])  # another process: its details come separately
    assert page.details.shown_values() == {}


def test_manager_page_shows_the_host_overview(app):
    page = pv.ServiceDetailPage()
    assert page.host.shown_values() == {} and page.host.lbl_hint.text() == pv.WAITING_REPORT_TEXT
    page.show_report(report())
    assert page.host.shown_values()["host"] == "rig-01"
    assert page.host.shown_values()["services"] == "2 · failed 1 · running 1"
    page.set_report_stale(True)
    assert pv.STALE_COLOR.name() in page.host.values["host"].styleSheet()
    page.show_report(None)
    assert page.host.shown_values() == {}
