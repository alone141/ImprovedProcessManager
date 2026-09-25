"""Window sizes follow the UI font and stay inside the screen."""

from PyQt6.QtCore import QSize
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import QWidget

import process_monitor_gui as pmg


def test_window_size_capped_to_screen(app):
    window = QWidget()
    avail = window.screen().availableGeometry()
    huge = QSize(avail.width() * 2, avail.height() * 2)
    pmg.fit_to_screen(window, huge, minimum=huge)
    assert window.width() <= avail.width()
    assert window.height() <= avail.height()
    assert window.minimumWidth() <= avail.width()


def test_window_size_follows_ui_font(app):
    base = QWidget()
    pmg.fit_to_screen(base, QSize(300, 200))
    font = app.font()
    try:
        bigger = QFont(font)
        bigger.setPointSizeF(font.pointSizeF() * 1.5)
        app.setFont(bigger)
        scaled = QWidget()
        pmg.fit_to_screen(scaled, QSize(300, 200))
        assert scaled.width() > base.width()
    finally:
        app.setFont(font)
