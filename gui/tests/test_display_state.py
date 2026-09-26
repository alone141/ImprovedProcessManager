"""The display state: the manager's eight states from the detailed report,
the health record's five mapped onto them, and how the pages show each."""

import pytest
from PyQt6.QtGui import QColor, QImage, QPainter

import process_views as pv
import usage_graphs as ug
from health_structs import CommandEnum, RuntimeState, ServiceState
from process_views import DisplayState

SEC = 1_000_000_000

# The manager's fold of its eight states into the health record's five
# (docs/protocol.md), which the buttons have to keep following.
PROTOCOL_FOLD = {
    ServiceState.STOPPED: RuntimeState.STOPPED,
    ServiceState.STOPPING: RuntimeState.STOPPED,
    ServiceState.WAITING: RuntimeState.STARTING,
    ServiceState.STARTING: RuntimeState.STARTING,
    ServiceState.BACKOFF: RuntimeState.STARTING,
    ServiceState.RUNNING: RuntimeState.RUNNING,
    ServiceState.UNHEALTHY: RuntimeState.UNHEALTHY,
    ServiceState.FAILED: RuntimeState.UNHEALTHY,
}


def test_both_feeds_map_onto_the_display_state():
    for state in ServiceState:
        assert pv.display_state(state) == DisplayState(int(state))
        assert pv.display_state(state).name == state.name
    assert pv.display_state(RuntimeState.RUNNING) == DisplayState.RUNNING
    assert pv.display_state(RuntimeState.STARTING) == DisplayState.STARTING
    assert pv.display_state(RuntimeState.STOPPED) == DisplayState.STOPPED
    assert pv.display_state(RuntimeState.UNHEALTHY) == DisplayState.UNHEALTHY
    assert pv.display_state(RuntimeState.UNKNOWN) == DisplayState.UNKNOWN
    assert pv.display_state(DisplayState.BACKOFF) == DisplayState.BACKOFF
    assert pv.display_state(None) == DisplayState.UNKNOWN


def test_stored_states_come_back_and_anything_else_is_unknown():
    for state in DisplayState:
        assert pv.stored_state(int(state)) == state
    assert pv.stored_state(99) == DisplayState.UNKNOWN


def test_every_state_has_its_own_colour_label_and_meaning():
    assert set(pv.STATE_COLORS) == set(pv.STATE_LABELS) == set(pv.STATE_MEANINGS) == set(DisplayState)
    assert len(set(pv.STATE_COLORS.values())) == len(DisplayState)
    # The five the health record knows keep the colours they always had.
    assert pv.state_color(RuntimeState.RUNNING) == "#4caf50"
    assert pv.state_color(RuntimeState.STARTING) == "#ff9800"
    assert pv.state_color(RuntimeState.STOPPED) == "#9e9e9e"
    assert pv.state_color(RuntimeState.UNHEALTHY) == "#f44336"
    assert pv.state_color(RuntimeState.UNKNOWN) == "#9c27b0"


@pytest.mark.parametrize("command", [CommandEnum.START, CommandEnum.STOP, CommandEnum.RESTART])
def test_the_buttons_allow_what_they_did_before(command):
    for state in RuntimeState:
        assert pv.action_enabled(command, state) == (state in pv.ACTION_STATES[command])
    for state, folded in PROTOCOL_FOLD.items():
        assert pv.action_enabled(command, state) == (folded in pv.ACTION_STATES[command])


def test_uptime_counts_while_a_process_lives():
    for state in (ServiceState.STARTING, ServiceState.RUNNING, ServiceState.UNHEALTHY, ServiceState.STOPPING):
        assert pv.uptime_text(state, 10 * SEC, 75 * SEC) == "01:05"
    for state in (ServiceState.WAITING, ServiceState.BACKOFF, ServiceState.FAILED, ServiceState.STOPPED):
        assert pv.uptime_text(state, 10 * SEC, 75 * SEC) == "—"


def view(state, **extra):
    return {"state": state, "snapshotTime": 100 * SEC, "missedBeats": 0, "_cpu_pct": 12.0, **extra}


def test_backoff_shows_when_the_restart_comes():
    backoff = view(DisplayState.BACKOFF, nextRestartTime=104 * SEC)
    assert pv.restart_wait_ns(backoff) == 4 * SEC
    assert pv.state_text(backoff) == "Backoff 4s"
    assert pv.row_hint(backoff) == ("restart in 4s", "badge")
    # Under a second to go still reads as a second, never 0s.
    soon = view(DisplayState.BACKOFF, nextRestartTime=100 * SEC + 300_000_000)
    assert pv.state_text(soon) == "Backoff 1s" and pv.row_hint(soon) == ("restart in 1s", "badge")
    assert pv.restart_wait_ns(view(DisplayState.BACKOFF, nextRestartTime=102 * SEC + 1)) == 3 * SEC
    # Past due, or not scheduled: no countdown.
    assert pv.state_text(view(DisplayState.BACKOFF, nextRestartTime=90 * SEC)) == "Backoff"
    assert pv.row_hint(view(DisplayState.BACKOFF)) == ("backoff", "badge")
    assert pv.state_text(view(DisplayState.RUNNING, nextRestartTime=104 * SEC)) == "Running"


def test_the_sidebar_flags_what_needs_attention():
    assert pv.row_hint(view(DisplayState.FAILED)) == ("failed", "badge")
    assert pv.row_hint(view(DisplayState.UNHEALTHY, missedBeats=2)) == ("2 missed", "badge")
    assert pv.row_hint(view(DisplayState.RUNNING)) == ("12 %", "cpu")
    assert pv.row_hint(view(DisplayState.WAITING)) == ("waiting", "muted")
    assert pv.row_hint(view(DisplayState.STOPPING)) == ("stopping", "muted")
    assert pv.row_hint(view(DisplayState.STOPPED)) == ("stopped", "muted")


def test_the_pill_names_the_state_and_explains_it(app):
    pill = pv.StatePill()
    pill.set_state(ServiceState.FAILED)
    assert pill.text() == "Failed" and pill.color() == pv.STATE_COLORS[DisplayState.FAILED]
    assert pill.toolTip().startswith("Failed: will not be restarted")
    pill.set_state(DisplayState.BACKOFF, text="Backoff 4s")
    assert pill.text() == "Backoff 4s"
    pill.set_state(DisplayState.BACKOFF, stale=True)
    assert pill.text() == "Backoff" and pill.color() == pv.STALE_COLOR.name()


def contrast(a: str, b: str) -> float:
    la, lb = sorted((pv._luminance(QColor(a)), pv._luminance(QColor(b))), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def test_the_pills_text_reads_on_every_states_colour():
    for color in [*pv.STATE_COLORS.values(), pv.STALE_COLOR.name()]:
        assert contrast(pv.ink_for(color), color) >= 4.5, color
    assert pv.ink_for("#ffe57f") == "#000000" and pv.ink_for("#9c27b0") == "#ffffff"
    assert "background: #ffe57f" in pv.solid_pill_style("#ffe57f")


def test_the_legend_lists_every_state():
    legend = pv.state_legend_html()
    for state in DisplayState:
        assert pv.STATE_COLORS[state] in legend and pv.STATE_LABELS[state].lower() in legend


def row_colours(band: pv.StateBand):
    """The distinct colours along the band's middle row."""
    image = QImage(band.size(), QImage.Format.Format_ARGB32)
    image.fill(QColor("#000000"))
    painter = QPainter(image)
    band.render(painter)
    painter.end()
    y = band.height() // 2
    return {image.pixelColor(x, y).name() for x in range(2, band.width() - 2)}


@pytest.mark.parametrize(
    "state, striped",
    [(DisplayState.RUNNING, False), (DisplayState.FAILED, False), (DisplayState.BACKOFF, True),
     (DisplayState.WAITING, True), (DisplayState.STARTING, True), (DisplayState.STOPPING, True)],
)
def test_states_on_their_way_somewhere_are_striped(app, state, striped):
    band = pv.StateBand()
    band.resize(200, 16)
    band.set_segments([(0.0, 10.0, int(state))], since=0.0, now=10.0)
    colours = row_colours(band)
    assert pv.STATE_COLORS[state] in colours
    assert (len(colours) > 1) == striped


def test_the_band_paints_what_the_graphs_recorded():
    # _record_usage stores the display state as an int; the band reads it back.
    samples = [ug.UsageSample(t, None, None, None, None, int(DisplayState.BACKOFF)) for t in (0, 1)]
    [(start, end, value)] = ug.state_segments(samples, since=0, now=2, gap_sec=5)
    assert pv.stored_state(value) == DisplayState.BACKOFF
