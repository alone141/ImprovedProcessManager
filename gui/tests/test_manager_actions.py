"""Whole-manager actions (start / stop / restart all, reload) and the
endpoints remembered between runs."""

from types import SimpleNamespace

from PyQt6.QtCore import QSettings

import process_monitor_gui as pmg
import process_views as pv
from health_structs import ALL_SERVICES, CommandEnum


# ── the manager page's buttons ───────────────────────────────────────────────

def test_manager_page_buttons_need_the_command_link(app):
    page = pv.ServiceDetailPage()
    assert set(page.buttons) == {CommandEnum.START, CommandEnum.STOP, CommandEnum.RESTART, CommandEnum.RELOAD}
    assert not any(button.isEnabled() for button in page.buttons.values())
    page.set_link_up(True)
    assert all(button.isEnabled() for button in page.buttons.values())
    page.set_link_up(False)
    assert not any(button.isEnabled() for button in page.buttons.values())


def test_manager_page_buttons_request_star_or_reload(app):
    page = pv.ServiceDetailPage()
    page.set_link_up(True)
    got = []
    page.command_requested.connect(lambda cmd, name: got.append((cmd, name)))
    page.buttons[CommandEnum.STOP].click()
    page.buttons[CommandEnum.START].click()
    page.buttons[CommandEnum.RESTART].click()
    page.buttons[CommandEnum.RELOAD].click()
    assert got == [
        (CommandEnum.STOP, ALL_SERVICES),
        (CommandEnum.START, ALL_SERVICES),
        (CommandEnum.RESTART, ALL_SERVICES),
        (CommandEnum.RELOAD, ""),
    ]


# ── confirmation and sending ─────────────────────────────────────────────────

def test_only_stopping_and_restarting_ask_first():
    assert pmg.command_question(CommandEnum.RELOAD, "") is None
    assert pmg.command_question(CommandEnum.START, ALL_SERVICES) is None
    title, text = pmg.command_question(CommandEnum.STOP, ALL_SERVICES)
    assert title == "Confirm stop all" and "every service" in text
    title, text = pmg.command_question(CommandEnum.RESTART, ALL_SERVICES)
    assert title == "Confirm restart all" and "restart" in text
    # One process: every command asks, as before
    title, text = pmg.command_question(CommandEnum.START, "svc")
    assert title == "Confirm Start" and "<b>svc</b>" in text


def test_command_labels():
    assert pmg.command_label(CommandEnum.STOP, "svc") == "STOP for svc"
    assert pmg.command_label(CommandEnum.STOP, ALL_SERVICES) == "STOP for every service"
    assert pmg.command_label(CommandEnum.RELOAD, "") == "RELOAD"


def sending_window(monkeypatch, answer):
    buttons = pmg.QMessageBox.StandardButton
    asked = []

    def question(_parent, title, _text, *_rest):
        asked.append(title)
        return answer

    monkeypatch.setattr(pmg, "QMessageBox", SimpleNamespace(question=question, StandardButton=buttons))
    sent, shown = [], []
    window = SimpleNamespace(
        worker=SimpleNamespace(send_command=lambda cmd, name, args="": sent.append((cmd, name))),
        _link_up=True,
        _show_status=shown.append,
    )
    return window, asked, sent, shown


def test_stop_all_asks_and_sends_star(monkeypatch):
    window, asked, sent, shown = sending_window(monkeypatch, pmg.QMessageBox.StandardButton.Yes)
    pmg.ProcessMonitorWindow._send_cmd(window, CommandEnum.STOP, ALL_SERVICES)
    assert asked == ["Confirm stop all"]
    assert sent == [(CommandEnum.STOP, ALL_SERVICES)]
    assert shown == ["Sending STOP for every service…"]


def test_reload_and_start_all_send_without_asking(monkeypatch):
    window, asked, sent, shown = sending_window(monkeypatch, pmg.QMessageBox.StandardButton.No)
    pmg.ProcessMonitorWindow._send_cmd(window, CommandEnum.RELOAD, "")
    pmg.ProcessMonitorWindow._send_cmd(window, CommandEnum.START, ALL_SERVICES)
    assert asked == []
    assert sent == [(CommandEnum.RELOAD, ""), (CommandEnum.START, ALL_SERVICES)]
    assert shown == ["Sending RELOAD…", "Sending START for every service…"]


def test_a_declined_confirmation_sends_nothing(monkeypatch):
    window, asked, sent, shown = sending_window(monkeypatch, pmg.QMessageBox.StandardButton.No)
    pmg.ProcessMonitorWindow._send_cmd(window, CommandEnum.RESTART, ALL_SERVICES)
    pmg.ProcessMonitorWindow._send_cmd(window, CommandEnum.STOP, "svc")
    assert asked == ["Confirm restart all", "Confirm Stop"] and sent == [] and shown == []


def test_the_worker_labels_whole_manager_commands(app):
    worker = pmg.ZmqWorker()
    worker.send_command(CommandEnum.RELOAD, "")
    worker.send_command(CommandEnum.STOP, ALL_SERVICES)
    assert worker._command_queue.get_nowait()[0] == "RELOAD"
    assert worker._command_queue.get_nowait()[0] == "STOP for every service"


# ── remembered endpoints ─────────────────────────────────────────────────────

def test_command_line_wins_then_stored_then_defaults():
    given = {"sub": "tcp://a:1", "dealer": None, "report": None}
    stored = {"sub": "tcp://s:1", "dealer": "tcp://s:2"}
    assert pmg.resolve_endpoints(given, stored) == ("tcp://a:1", "tcp://s:2", pmg.DEFAULT_REPORT_ENDPOINT)
    assert pmg.resolve_endpoints({}, {}) == (
        pmg.DEFAULT_SUB_ENDPOINT, pmg.DEFAULT_DEALER_ENDPOINT, pmg.DEFAULT_REPORT_ENDPOINT
    )
    assert pmg.resolve_endpoints({"sub": ""}, {"sub": "tcp://s:1"})[0] == "tcp://s:1"  # empty is not given


def test_endpoints_round_trip_through_the_settings_file(tmp_path):
    path = str(tmp_path / "ProcessMonitor.ini")
    settings = QSettings(path, QSettings.Format.IniFormat)
    assert pmg.stored_endpoints(settings) == {}
    pmg.store_endpoints(settings, "tcp://h:6667", "tcp://h:5557", "tcp://h:6668")
    del settings
    again = QSettings(path, QSettings.Format.IniFormat)
    assert pmg.stored_endpoints(again) == {
        "sub": "tcp://h:6667", "dealer": "tcp://h:5557", "report": "tcp://h:6668",
    }
    assert pmg.resolve_endpoints({"dealer": "tcp://x:1"}, pmg.stored_endpoints(again)) == (
        "tcp://h:6667", "tcp://x:1", "tcp://h:6668"
    )


def test_the_window_stores_the_endpoints_it_connects_with(app, tmp_path):
    path = str(tmp_path / "ProcessMonitor.ini")
    settings = QSettings(path, QSettings.Format.IniFormat)
    window = pmg.ProcessMonitorWindow(
        "tcp://127.0.0.1:16667", "tcp://127.0.0.1:15557",
        report_endpoint="tcp://127.0.0.1:16668", settings=settings,
    )
    try:
        assert pmg.stored_endpoints(settings) == {
            "sub": "tcp://127.0.0.1:16667", "dealer": "tcp://127.0.0.1:15557", "report": "tcp://127.0.0.1:16668",
        }
        window.edit_dealer.setText("tcp://127.0.0.1:15558")
        window._reconnect()
        assert pmg.stored_endpoints(settings)["dealer"] == "tcp://127.0.0.1:15558"
    finally:
        window.close()


def test_the_settings_file_is_per_user_ini():
    settings = pmg.open_settings()
    assert settings.format() == QSettings.Format.IniFormat
    assert settings.organizationName() == "beray" and settings.applicationName() == "ProcessMonitor"
