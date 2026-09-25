"""Log views append new journal entries and keep the reader's place."""

import pytest
from PyQt6.QtWidgets import QTextEdit

from process_monitor_gui import JournalView, new_journal_entries
from process_views import ServiceDetailPage


def entries(*names):
    return [(100, n) for n in names]


def test_new_entries_after_overlap():
    assert new_journal_entries(entries("a", "b", "c"), entries("b", "c", "d", "e")) == entries("d", "e")


def test_new_entries_with_repeated_lines():
    assert new_journal_entries(entries("a", "a", "a"), entries("a", "a", "b")) == entries("b")


def test_no_new_entries_when_unchanged():
    assert new_journal_entries(entries("a", "b"), entries("a", "b")) == []


def test_no_overlap_means_repaint():
    assert new_journal_entries(entries("a", "b"), entries("x", "y")) is None
    assert new_journal_entries([], entries("x")) is None


def _settle(app):
    for _ in range(5):
        app.processEvents()


@pytest.fixture
def view(app):
    edit = QTextEdit()
    edit.resize(300, 120)
    edit.show()
    journal = JournalView(edit)
    journal.show([(100, f"line {i}") for i in range(100)])
    _settle(app)
    yield journal, edit
    edit.close()


def _lines(edit):
    return edit.toPlainText().split("\n")


def _tail(start, count=100):
    return [(100, f"line {i}") for i in range(start, start + count)]


def test_appends_instead_of_repainting(view):
    journal, edit = view
    journal.show(_tail(2))  # the tail moved on by two entries
    lines = _lines(edit)
    assert lines[0] == "line 0"  # nothing cleared
    assert lines[-2:] == ["line 100", "line 101"]
    assert len(lines) == 102


def test_follows_the_end_when_scrolled_there(view, app):
    journal, edit = view
    bar = edit.verticalScrollBar()
    assert bar.value() == bar.maximum() > 0
    journal.show(_tail(5))
    _settle(app)
    assert bar.value() == bar.maximum()


def test_keeps_place_when_scrolled_up(view, app):
    journal, edit = view
    bar = edit.verticalScrollBar()
    bar.setValue(10)
    journal.show(_tail(5))
    _settle(app)
    assert bar.value() == 10


def test_keeps_selection(view, app):
    journal, edit = view
    cursor = edit.textCursor()
    cursor.setPosition(0)
    cursor.setPosition(6, cursor.MoveMode.KeepAnchor)
    edit.setTextCursor(cursor)
    journal.show(_tail(1))
    assert edit.textCursor().selectedText() == "line 0"


def test_unrelated_tail_repaints(view):
    journal, edit = view
    journal.show([(0, "journalctl failed")])
    assert _lines(edit) == ["journalctl failed"]
    journal.show([])
    assert _lines(edit) == ["(empty)"]


def test_status_pane_keeps_scroll_position(app):
    page = ServiceDetailPage()
    page.resize(500, 320)
    page.show()
    _settle(app)
    page.show_snapshot("\n".join(f"status {i}" for i in range(100)), [], "")
    _settle(app)
    bar = page.txt_status.verticalScrollBar()
    bar.setValue(30)
    # systemctl status changes on every refresh (CPU / memory lines)
    page.show_snapshot("\n".join(f"status {i}*" for i in range(100)), [], "")
    _settle(app)
    assert bar.value() == 30
    assert page.lbl_refresh.text().startswith("Last refresh: ")
    assert page.lbl_error.isHidden()
    page.show_snapshot("", [], "systemctl not found")
    assert page.txt_status.toPlainText() == "(empty)"
    assert page.lbl_error.text() == "systemctl not found" and not page.lbl_error.isHidden()
    page.close()


def test_reset_follows_the_newest_line_of_the_next_journal(view, app):
    journal, edit = view
    bar = edit.verticalScrollBar()
    bar.setValue(0)  # scrolled up to read old lines
    _settle(app)
    journal.reset()  # another process is shown
    journal.show([(7, f"other {i}") for i in range(100)])
    _settle(app)
    assert bar.value() == bar.maximum() > 0
