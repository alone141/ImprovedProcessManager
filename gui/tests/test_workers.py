"""Workers and the command path: an early stop sticks, commands need a live link."""

import threading
from types import SimpleNamespace

import pytest

import process_monitor_gui as pmg

WORKERS = [
    pmg.ZmqWorker,
    pmg.SystemdLogWorker,
    pmg.CgroupLogWorker,
    pmg.DetailJournalWorker,
    pmg.CgroupMembersWorker,
]


@pytest.mark.parametrize("make", WORKERS, ids=lambda cls: cls.__name__)
def test_a_worker_stopped_before_it_starts_returns_at_once(app, make):
    worker = make()
    worker.stop()  # the window closed before the thread got going
    runner = threading.Thread(target=worker.start, daemon=True)
    runner.start()
    runner.join(5.0)
    still_running = runner.is_alive()
    worker.stop()
    assert not still_running


def link_window(**extra):
    window = SimpleNamespace(
        _link_up=False,
        _feed=pmg.FeedMonitor(),
        _refresh_feed_status=lambda: None,
        _set_link_status=lambda *args: None,
        btn_connection=SimpleNamespace(setChecked=lambda on: None),
    )
    for key, value in extra.items():
        setattr(window, key, value)
    return window


def test_the_link_follows_the_connection_status():
    window = link_window()
    pmg.ProcessMonitorWindow._on_connection_status(window, "connected")
    assert window._link_up
    pmg.ProcessMonitorWindow._on_connection_status(window, "disconnected")
    assert not window._link_up
    pmg.ProcessMonitorWindow._on_connection_status(window, "error: bad endpoint")
    assert not window._link_up


def test_commands_need_a_connected_worker(monkeypatch):
    buttons = pmg.QMessageBox.StandardButton
    monkeypatch.setattr(
        pmg,
        "QMessageBox",
        SimpleNamespace(question=lambda *args: buttons.Yes, StandardButton=buttons),
    )
    sent, shown = [], []
    window = SimpleNamespace(
        worker=SimpleNamespace(send_command=lambda cmd, name, args="": sent.append((cmd, name))),
        _link_up=False,
        _show_status=shown.append,
    )
    pmg.ProcessMonitorWindow._send_cmd(window, pmg.CommandEnum.STOP, "svc")
    assert sent == [] and shown[-1].startswith("Not connected")

    window._link_up = True
    pmg.ProcessMonitorWindow._send_cmd(window, pmg.CommandEnum.STOP, "svc")
    assert sent == [(pmg.CommandEnum.STOP, "svc")]


def test_reconnect_forgets_cpu_baselines_and_the_link():
    window = link_window(
        _link_up=True,
        _prev={"svc": (1_000, 2_000)},
        btn_reconnect=SimpleNamespace(setEnabled=lambda on: None),
        _stop_zmq_worker=lambda timeout_ms: None,
        _start_zmq_worker=lambda: None,
        _show_status=lambda text: None,
        sub_endpoint="tcp://host:6667",
        dealer_endpoint="tcp://host:5557",
    )
    pmg.ProcessMonitorWindow._reconnect(window)
    assert window._prev == {} and not window._link_up


def select_window(hidden):
    chosen = []
    window = SimpleNamespace(
        _auto_select=True,
        _current={"alpha": {}, "beta": {}, "gamma": {}},
        sidebar=SimpleNamespace(is_hidden=lambda name: name in hidden, select_process=chosen.append),
    )
    return window, chosen


def test_auto_select_skips_processes_the_filter_hides():
    window, chosen = select_window({"alpha"})
    pmg.ProcessMonitorWindow._auto_select_first(window)
    assert chosen == ["beta"] and not window._auto_select


def test_auto_select_waits_while_the_filter_hides_everything():
    window, chosen = select_window({"alpha", "beta", "gamma"})
    pmg.ProcessMonitorWindow._auto_select_first(window)
    assert chosen == [] and window._auto_select
