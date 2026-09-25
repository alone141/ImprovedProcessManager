"""Generic widgets: the wrapping flow layout and the eliding label."""

from PyQt6.QtWidgets import QPushButton, QWidget

import process_views as pv


def _settle(app):
    for _ in range(5):
        app.processEvents()


def test_flow_layout_wraps_and_skips_hidden_items(app):
    box = QWidget()
    flow = pv.FlowLayout(box, h_spacing=4, v_spacing=4)
    buttons = [QPushButton("button %d" % i) for i in range(6)]
    for button in buttons:
        flow.addWidget(button)
    one_row = sum(b.sizeHint().width() + 4 for b in buttons)
    assert flow.heightForWidth(one_row) == buttons[0].sizeHint().height()
    assert flow.heightForWidth(one_row // 2) > buttons[0].sizeHint().height()

    buttons[0].hide()
    box.resize(one_row // 2, 200)
    box.show()
    _settle(app)
    shown = [b for b in buttons if not b.isHidden()]
    assert len({b.geometry().y() for b in shown}) >= 2  # several rows
    assert all(b.geometry().right() <= box.width() for b in shown)
    assert min(b.geometry().x() for b in shown) == 0  # hidden one left no gap
    box.close()


def test_elided_label_never_forces_a_minimum_width(app):
    label = pv.ElidedLabel("a" * 300)
    assert label.minimumSizeHint().width() == 0
    assert label.toolTip() == "a" * 300
    label.resize(60, 20)
    label.show()
    _settle(app)
    assert not label.grab().isNull()
    label.close()
