import os

# Widget tests run without a display.
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest


@pytest.fixture(scope="session")
def app():
    from PyQt6.QtWidgets import QApplication

    return QApplication.instance() or QApplication([])
