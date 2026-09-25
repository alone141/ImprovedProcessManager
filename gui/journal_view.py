"""Journal (log) text view: lines coloured by PID, appended in place.

Shared by the manager page, the process Journal tab and the PID log
windows. No threads: callers hand it (pid, text) pairs.
"""

from __future__ import annotations

from typing import Dict, Optional

from PyQt6.QtGui import QColor, QTextCharFormat, QTextCursor
from PyQt6.QtWidgets import QTextEdit

from usage_graphs import SERIES_COLORS

PID_PALETTE = SERIES_COLORS  # same palette as the usage graphs
NO_PID_COLOR = QColor("#888888")


def color_for_pid(store: Dict[int, QColor], pid: int) -> QColor:
    """One palette colour per PID for the life of ``store``."""
    if pid <= 0:
        return NO_PID_COLOR
    if pid not in store:
        store[pid] = PID_PALETTE[len(store) % len(PID_PALETTE)]
    return store[pid]


def new_journal_entries(shown: list, latest: list) -> Optional[list]:
    """Entries of ``latest`` (a fresh journal tail) that follow what is
    already shown, or None when the two tails don't overlap."""
    for start in range(len(shown)):
        overlap = shown[start:]
        if latest[: len(overlap)] == overlap:
            return latest[len(overlap):]
    return None


class JournalView:
    """Shows (pid, text) journal entries in a QTextEdit, colored by PID.

    New entries are appended, so the scroll position and any selection
    survive a refresh; the view follows the newest line only while it is
    scrolled to the end.
    """

    MAX_LINES = 5000

    def __init__(self, widget: QTextEdit):
        self.widget = widget
        widget.setUndoRedoEnabled(False)
        widget.document().setMaximumBlockCount(self.MAX_LINES)
        self._colors: Dict[int, QColor] = {}
        self._shown: Optional[list] = None
        self._follow = True
        self._repainting = False
        bar = widget.verticalScrollBar()
        bar.valueChanged.connect(self._on_scrolled)
        bar.rangeChanged.connect(self._on_range_changed)

    def reset(self) -> None:
        """Forget the shown lines and follow the newest again, for another journal."""
        self._shown = None
        self._follow = True

    def show(self, pairs: list) -> None:
        if pairs == self._shown:
            return
        new = new_journal_entries(self._shown or [], pairs)
        if new is None:  # first paint, or the tails don't overlap
            bar = self.widget.verticalScrollBar()
            keep = bar.value()
            self._repainting = True
            try:
                self.widget.clear()
                self._append(pairs or [(0, "(empty)")])
            finally:
                self._repainting = False
            bar.setValue(bar.maximum() if self._follow else min(keep, bar.maximum()))
        else:
            self._append(new)  # _on_range_changed keeps a following view at the end
        self._shown = list(pairs)

    def _append(self, pairs: list) -> None:
        doc = self.widget.document()
        cursor = QTextCursor(doc)  # not the widget's cursor: keeps the selection
        cursor.movePosition(QTextCursor.MoveOperation.End)
        for pid, text in pairs:
            if not doc.isEmpty():
                cursor.insertBlock()
            fmt = QTextCharFormat()
            fmt.setForeground(color_for_pid(self._colors, int(pid)))
            cursor.insertText(str(text), fmt)

    def _on_scrolled(self, value: int) -> None:
        if not self._repainting:
            self._follow = value >= self.widget.verticalScrollBar().maximum()

    def _on_range_changed(self, _minimum: int, maximum: int) -> None:
        if self._follow and not self._repainting:
            self.widget.verticalScrollBar().setValue(maximum)
