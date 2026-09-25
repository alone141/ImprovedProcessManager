"""Process detail page: header, tiles, actions, PIDs, journal, graphs and staleness."""

import process_views as pv
import usage_graphs as ug
from gpu_sampler import GpuProcessUsage
from health_structs import CommandEnum, RuntimeState
from systemd_logs import CgroupProc

MB = 1024**2
SEC = 1_000_000_000
REPORT = {
    "pid": 42, "state": RuntimeState.RUNNING, "memoryUsageInBytes": 10 * MB,
    "cpuUsageInUsec": 0, "snapshotTime": 100 * SEC, "start_time": 40 * SEC,
    "lastSeen": 100 * SEC, "missedBeats": 0, "restartCount": 1, "_cpu_pct": 12.5,
}


def page(history=None):
    return pv.ProcessDetailPage(history or ug.UsageHistory())


def enabled(p):
    return {cmd: button.isEnabled() for cmd, button in p.buttons.items()}


def test_tiles_and_header_follow_the_report(app):
    p = page()
    p.set_process("svc", dict(REPORT), GpuProcessUsage(util_pct=30.0, vram_bytes=2 * MB), [])
    assert p.lbl_name.text() == "svc"
    assert p.pill.text() == "Running" and p.pill.color() == "#4caf50"
    assert p.tile_values() == {
        "cpu": "12.5 %", "mem": "10.0 MB", "gpu": "30.0 %", "vram": "2.0 MB",
        "missed": "0", "restarts": "1",
    }
    assert p.lbl_meta.text().startswith("PID 42 · up 01:00 · last seen 0.0s ago · snapshot ")

    p.update_report({**REPORT, "pid": 0, "state": RuntimeState.STOPPED}, None)
    assert p.pill.text() == "Stopped"
    assert p.tile_values()["gpu"] == "—" and p.tile_values()["vram"] == "—"
    assert p.lbl_meta.text().startswith("PID — · up — ·")


def test_without_a_report_everything_is_blank(app):
    p = page()
    p.set_process("svc", None, None, [])
    assert set(p.tile_values().values()) == {"—"}
    assert not any(enabled(p).values())
    assert p.pill.text() == "Unknown"


def test_actions_follow_the_state(app):
    p = page()
    p.set_process("svc", dict(REPORT), None, [])
    assert enabled(p) == {CommandEnum.START: False, CommandEnum.STOP: True, CommandEnum.RESTART: True}
    p.update_report({**REPORT, "state": RuntimeState.STOPPED}, None)
    assert enabled(p) == {CommandEnum.START: True, CommandEnum.STOP: False, CommandEnum.RESTART: True}
    p.update_report({**REPORT, "state": RuntimeState.UNHEALTHY}, None)
    assert all(enabled(p).values())


def test_action_button_requests_a_command_for_the_shown_process(app):
    p = page()
    got = []
    p.command_requested.connect(lambda cmd, name: got.append((cmd, name)))
    p.buttons[CommandEnum.STOP].click()  # no process yet: nothing to send
    p.set_process("svc", dict(REPORT), None, [])
    p.buttons[CommandEnum.STOP].click()
    p.buttons[CommandEnum.RESTART].click()
    assert got == [(CommandEnum.STOP, "svc"), (CommandEnum.RESTART, "svc")]


def test_members_fill_the_pid_table_and_tab_title(app):
    p = page()
    p.set_process("svc", dict(REPORT), None, [
        CgroupProc(pid=11, comm="worker", rss_bytes=2 * MB, cmdline="/opt/svc --worker"),
        CgroupProc(pid=7, comm="svc", rss_bytes=0, cmdline=""),
    ])
    assert p.pids.rowCount() == 2
    assert [p.pids.item(r, 1).text() for r in range(2)] == ["7", "11"]  # sorted by PID
    assert [p.pids.item(r, 2).text() for r in range(2)] == ["—", "2.0 MB"]
    assert p.tabs.tabText(p.tabs.indexOf(p.pids)) == "cgroup PIDs · 2"

    activated = []
    p.pid_activated.connect(lambda pid, comm: activated.append((pid, comm)))
    p.pids._on_double_clicked(p.pids.item(1, 3))  # any cell of the row
    assert activated == [(11, "worker")]

    p.set_members([])
    assert p.pids.rowCount() == 0
    assert p.tabs.tabText(p.tabs.indexOf(p.pids)) == "cgroup PIDs · 0"


def test_stale_greys_values_and_recovers(app):
    p = page()
    p.set_process("svc", dict(REPORT), None, [])
    p.set_stale(True)
    assert {t.value_color() for t in p.tiles.values()} == {pv.STALE_COLOR.name()}
    assert p.pill.color() == pv.STALE_COLOR.name()
    p.update_report(dict(REPORT), None)  # a late report while stale stays grey
    assert p.pill.color() == pv.STALE_COLOR.name()
    p.set_stale(False)
    assert {t.value_color() for t in p.tiles.values()} == {pv.TEXT}
    assert p.pill.color() == "#4caf50"


def test_journal_meta_reports_the_cgroup_or_the_error(app):
    p = page()
    p.set_process("svc", dict(REPORT), None, [])
    assert "loading" in p.lbl_journal_meta.text()
    p.set_journal([(7, "hello")], "/system.slice/task_svc", "")
    assert p.lbl_journal_meta.text() == "task_svc  →  /system.slice/task_svc"
    assert p.txt_journal.toPlainText() == "hello"
    p.set_journal([], "", "cgroup task_svc not found under /sys/fs/cgroup")
    assert p.lbl_journal_meta.text() == "cgroup task_svc not found under /sys/fs/cgroup"
    assert p.txt_journal.toPlainText() == "cgroup task_svc not found under /sys/fs/cgroup"


def test_graphs_tab_shows_the_process_and_compares_on_request(app, monkeypatch):
    monkeypatch.setattr(pv.time, "monotonic", lambda: 1_000.0)
    monkeypatch.setattr(pv.time, "time", lambda: 10_000.0)
    history = ug.UsageHistory()
    for name, t in (("svc", 999.0), ("other", 999.0), ("old", 1.0)):
        history.add(name, ug.UsageSample(t, 1.0, MB, None, None, int(RuntimeState.RUNNING)))
    p = page(history)
    p.set_process("svc", dict(REPORT), None, [])
    assert p.tabs.currentWidget() is p.graphs
    assert p.graphs.shown_names() == ["svc"]
    assert list(p.graphs.chips) == ["other"]  # "old" has no sample in the 5-minute view
    assert p.graphs.span_seconds() == 300.0

    p.graphs.chips["other"].setChecked(True)
    assert p.graphs.shown_names() == ["svc", "other"]
    assert p.graphs.compared() == ["other"]
    p.graphs.chips["other"].setChecked(False)
    assert p.graphs.shown_names() == ["svc"]

    # The band covers the whole 15 minutes; the only run is the recent sample.
    assert p.band.segments() == [(999.0, 1_000.0, int(RuntimeState.RUNNING))]

    p.set_process("other", dict(REPORT), None, [])  # switching drops it from the chips
    assert p.graphs.shown_names() == ["other"]
    assert list(p.graphs.chips) == ["svc"]


def test_compare_chips_wrap_in_a_narrow_panel(app, monkeypatch):
    monkeypatch.setattr(pv.time, "monotonic", lambda: 1_000.0)
    monkeypatch.setattr(pv.time, "time", lambda: 10_000.0)
    history = ug.UsageHistory()
    for name in ("svc", *[f"another_long_process_name_{i}" for i in range(6)]):
        history.add(name, ug.UsageSample(999.0, 1.0, MB, None, None, int(RuntimeState.RUNNING)))
    p = page(history)
    p.set_process("svc", dict(REPORT), None, [])
    p.resize(520, 720)  # a narrow pane: the six long names cannot share one row
    p.show()
    for _ in range(5):
        app.processEvents()
    chips = list(p.graphs.chips.values())
    rows = {chip.geometry().y() for chip in chips}
    assert len(chips) == 6 and len(rows) > 1  # wrapped onto several rows
    assert all(chip.geometry().right() <= chip.parentWidget().width() for chip in chips)
    p.close()


def test_graphs_only_refresh_while_their_tab_is_shown(app, monkeypatch):
    monkeypatch.setattr(pv.time, "monotonic", lambda: 100.0)
    monkeypatch.setattr(pv.time, "time", lambda: 1_000.0)
    history = ug.UsageHistory()
    p = page(history)
    p.set_process("svc", dict(REPORT), None, [])
    p.tabs.setCurrentWidget(p.pids)
    history.add("svc", ug.UsageSample(99.0, 1.0, MB, None, None, int(RuntimeState.RUNNING)))
    p.tick(100.0, 1_000.0)

    def points():
        return [pt for _name, _color, pts in p.graphs.charts[0].series for pt in pts]

    assert p.graphs.shown_names() == ["svc"] and points() == []  # not redrawn while hidden
    p.tabs.setCurrentWidget(p.graphs)  # switching back redraws at once
    assert points() == [(99.0, 1.0)]


def test_unknown_vram_shows_a_dash(app):
    p = page()
    p.set_process("svc", dict(REPORT), GpuProcessUsage(util_pct=30.0, vram_bytes=None), [])
    assert p.tile_values()["gpu"] == "30.0 %" and p.tile_values()["vram"] == pv.gpu_texts(None)[1]


def test_a_process_never_seen_says_so():
    assert pv.last_seen_text({"snapshotTime": 100 * SEC, "lastSeen": 0}) == "never"
    assert pv.last_seen_text({"snapshotTime": 100 * SEC, "lastSeen": 99 * SEC}) == "1.0s ago"
