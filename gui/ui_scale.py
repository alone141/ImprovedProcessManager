"""Font- and display-relative sizing, shared by every window.

Pixel sizes (window sizes) were designed for Windows' 9pt (12px) UI font;
everything else follows the application font, so text scales with the
system font size and display scaling instead of fixed pixels.
"""

from __future__ import annotations

from typing import Optional

from PyQt6.QtCore import QSize
from PyQt6.QtGui import QFont, QGuiApplication
from PyQt6.QtWidgets import QApplication, QWidget

DESIGN_UI_POINT_SIZE = 9.0
# Bold process names / states: slightly larger than the surrounding text.
EMPHASIS_FONT_SCALE = 10 / 9
# Monospace for style sheets. A bare "monospace" is not a font on Windows;
# Qt falls back to Tahoma there, which renders narrow and squished.
MONO_FONT_FAMILIES = "Consolas, 'Courier New', monospace"


def ui_point_size(scale: float = 1.0) -> float:
    """Point size relative to the application font, so text follows the
    system font size instead of fixed pixels."""
    font = QApplication.font()
    base = font.pointSizeF()
    if base <= 0:  # application font was set in pixels
        base = font.pixelSize() * 72.0 / 96.0
    return base * scale


def ui_font(
    scale: float = 1.0, weight: QFont.Weight = QFont.Weight.Normal
) -> QFont:
    """The application (system UI) font at a relative size."""
    font = QFont(QApplication.font())
    font.setPointSizeF(ui_point_size(scale))
    font.setWeight(weight)
    return font


def fit_to_screen(
    window: QWidget, size: QSize, minimum: Optional[QSize] = None
) -> None:
    """Resize a top-level window from design sizes (at the 9pt design font).

    Sizes grow with a larger UI font so the columns still fit, and are capped
    to the screen: they are logical pixels, and at 150–200 % display scaling
    a fixed 1150×620 window can be larger than the whole desktop.
    """
    factor = max(1.0, ui_point_size() / DESIGN_UI_POINT_SIZE)
    size = size * factor
    if minimum is not None:
        minimum = minimum * factor
    screen = window.screen() or QGuiApplication.primaryScreen()
    if screen is not None:
        avail = screen.availableGeometry()
        # Leave room for the title bar and frame.
        bound = QSize(int(avail.width() * 0.95), int(avail.height() * 0.9))
        size = size.boundedTo(bound)
        if minimum is not None:
            minimum = minimum.boundedTo(bound)
    if minimum is not None:
        window.setMinimumSize(minimum)
    window.resize(size)
