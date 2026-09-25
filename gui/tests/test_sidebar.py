"""Sidebar: the manager entry plus one row per process, filterable and selectable."""

import process_views as pv
from health_structs import RuntimeState

SEC = 1_000_000_000


def report(state=RuntimeState.RUNNING, cpu=37.4, missed=0):
    return {
        "pid": 42, "state": state, "memoryUsageInBytes": 10 * 1024**2,
        "cpuUsageInUsec": 0, "snapshotTime": 100 * SEC, "start_time": 40 * SEC,
        "lastSeen": 100 * SEC, "missedBeats": missed, "restartCount": 1, "_cpu_pct": cpu,
    }


def test_rows_follow_the_reports_sorted_by_name(app):
    bar = pv.ProcessSidebar()
    bar.set_processes({"b": report(), "a": report()})
    assert bar.process_names() == ["a", "b"]
    assert bar.header_text() == "Processes · 2"

    bar.set_processes({"c": report(), "a": report(), "ab": report()})
    assert bar.process_names() == ["a", "ab", "c"]  # b left, ab slotted in order
    assert bar.header_text() == "Processes · 3"


def test_filter_hides_rows(app):
    bar = pv.ProcessSidebar()
    bar.set_processes({"sensor_fusion": report(), "path_planner": report()})
    bar.set_filter("  PATH ")
    assert bar.is_hidden("sensor_fusion") and not bar.is_hidden("path_planner")
    bar.set_processes({"sensor_fusion": report(), "path_planner": report(), "pathfinder": report()})
    assert not bar.is_hidden("pathfinder")  # new rows respect the filter too
    bar.set_filter("")
    assert not bar.is_hidden("sensor_fusion")


def test_selection_signals(app):
    bar = pv.ProcessSidebar()
    got = []
    bar.process_selected.connect(got.append)
    bar.manager_selected.connect(lambda: got.append("manager"))
    bar.set_processes({"a": report(), "b": report()})

    bar.select_process("b")
    assert got == ["b"] and bar.selected_process() == "b"
    bar.select_manager()
    assert got == ["b", "manager"] and bar.selected_process() is None
    bar.select_process("missing")  # no such row: selection unchanged
    assert got == ["b", "manager"]


def test_row_hint_by_state():
    assert pv.row_hint(report()) == ("37 %", "cpu")
    assert pv.row_hint(report(cpu=None)) == ("—", "cpu")
    assert pv.row_hint(report(RuntimeState.UNHEALTHY, missed=2)) == ("2 missed", "badge")
    assert pv.row_hint(report(RuntimeState.UNHEALTHY)) == ("unhealthy", "badge")
    assert pv.row_hint(report(RuntimeState.STARTING)) == ("starting", "muted")
    assert pv.row_hint(report(RuntimeState.STOPPED)) == ("stopped", "muted")


def test_sidebar_paints_live_and_stale(app):
    bar = pv.ProcessSidebar()
    bar.resize(220, 300)
    bar.show()
    bar.set_processes({
        "a": report(), "b": report(RuntimeState.UNHEALTHY, missed=3),
        "c": report(RuntimeState.STOPPED),
    })
    bar.select_process("a")
    assert not bar.grab().isNull()
    bar.set_stale(True)
    assert not bar.grab().isNull()
    bar.close()


def test_service_active_text():
    status = (
        "● berayprocessmanager.service - Process manager\n"
        "     Loaded: loaded (/etc/systemd/system/berayprocessmanager.service)\n"
        "     Active: active (running) since Wed 2026-09-24 09:00:01 UTC; 3h ago\n"
    )
    assert pv.service_active_text(status) == "active"
    assert pv.service_active_text(status.replace("active (running)", "failed (Result: exit)")) == "failed"
    assert pv.service_active_text("Unit could not be found.") == ""
    assert pv.service_active_text("") == ""
