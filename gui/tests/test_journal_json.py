import json

import systemd_logs
from systemd_logs import _journal_result, parse_journal_json


def test_two_pids():
    recs = [
        {"__REALTIME_TIMESTAMP": "1700000000000000", "_PID": "100", "MESSAGE": "hello"},
        {"__REALTIME_TIMESTAMP": "1700000001000000", "_PID": "200", "MESSAGE": "world"},
    ]
    text = "\n".join(json.dumps(r) for r in recs)
    lines = parse_journal_json(text)
    assert len(lines) == 2
    assert lines[0].pid == 100 and "hello" in lines[0].text and "[100]:" in lines[0].text
    assert lines[1].pid == 200 and "world" in lines[1].text and "[200]:" in lines[1].text


def test_missing_pid():
    text = json.dumps({"__REALTIME_TIMESTAMP": "1", "MESSAGE": "no pid"})
    lines = parse_journal_json(text)
    assert len(lines) == 1
    assert lines[0].pid is None
    assert "no pid" in lines[0].text
    assert "[None]" not in lines[0].text


def test_skips_bad_json():
    text = "not json\n" + json.dumps({"_PID": "3", "MESSAGE": "ok"})
    lines = parse_journal_json(text)
    assert len(lines) == 1
    assert lines[0].pid == 3


def test_message_as_byte_list():
    text = json.dumps({"_PID": "9", "MESSAGE": [72, 105]})  # "Hi"
    lines = parse_journal_json(text)
    assert lines[0].raw_message == "Hi"
    assert lines[0].pid == 9


def test_empty():
    assert parse_journal_json("") == []
    assert parse_journal_json("   \n") == []


def test_message_as_list_of_strings():
    # A field an entry carries twice arrives as a list.
    [line] = parse_journal_json(json.dumps({"_PID": "7", "MESSAGE": ["first", "second"]}))
    assert line.raw_message == "first | second"


def test_journalctl_explains_an_empty_answer():
    assert _journal_result(0, "", "No journal files were found.") == (
        [], "No journal files were found."
    )
    assert _journal_result(1, "", "") == ([], "journalctl failed (exit 1)")
    lines, error = _journal_result(0, json.dumps({"MESSAGE": "hi"}), "Hint: add yourself to adm")
    assert error is None and [ln.raw_message for ln in lines] == ["hi"]


def test_journal_queries_keep_long_fields_and_pid_queries_stay_in_this_boot(monkeypatch):
    calls = []
    monkeypatch.setattr(systemd_logs, "_which", lambda name: "journalctl")
    monkeypatch.setattr(
        systemd_logs, "_run", lambda exe, args: calls.append(list(args)) or (0, "", "")
    )
    systemd_logs.fetch_journal_pid(42)
    systemd_logs.fetch_journal_cgroup("/system.slice/task_x")
    systemd_logs.fetch_journal("pm.service")
    assert len(calls) == 3 and all("--all" in args for args in calls)
    assert "-b" in calls[0] and "_PID=42" in calls[0]
