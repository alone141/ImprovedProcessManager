"""CPU % and uptime shown for each health report."""

from health_structs import RuntimeState
from process_monitor_gui import CPU_PCT_MAX, cpu_percent, uptime_text

SEC = 1_000_000_000  # ns


def test_cpu_percent_needs_a_previous_sample():
    assert cpu_percent(None, 1_000_000, 5 * SEC) is None


def test_cpu_percent_of_one_core():
    # 0.5 s of CPU over 1 s of wall time
    assert cpu_percent((0, 0), 500_000, SEC) == 50.0


def test_cpu_percent_counts_every_core():
    # 12 s of CPU in 1 s: twelve busy cores (was capped at 800 %)
    assert cpu_percent((0, 0), 12_000_000, SEC) == min(1200.0, CPU_PCT_MAX)


def test_cpu_percent_is_clamped_to_all_cores():
    assert cpu_percent((0, 0), 10**12, SEC) == CPU_PCT_MAX


def test_cpu_percent_ignores_counter_reset_and_clock_skew():
    assert cpu_percent((5_000_000, 0), 1_000, SEC) is None  # restarted
    assert cpu_percent((0, SEC), 1_000, SEC) is None  # same snapshot time


def test_uptime_only_for_a_running_process():
    for state in (RuntimeState.STARTING, RuntimeState.RUNNING, RuntimeState.UNHEALTHY):
        assert uptime_text(state, 10 * SEC, 75 * SEC) == "01:05"
    for state in (RuntimeState.STOPPED, RuntimeState.UNKNOWN):
        assert uptime_text(state, 10 * SEC, 75 * SEC) == "—"


def test_uptime_unknown_without_start_time():
    assert uptime_text(RuntimeState.RUNNING, 0, 75 * SEC) == "—"
