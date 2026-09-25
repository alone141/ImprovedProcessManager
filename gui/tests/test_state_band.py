"""State band: runs of one state over recent time, from the usage samples."""

import usage_graphs as ug


def sample(t, state):
    return ug.UsageSample(t, None, None, None, None, state)


def test_runs_of_equal_state_merge_and_change_at_the_new_sample():
    samples = [sample(0, 2), sample(1, 2), sample(2, 2), sample(3, 4), sample(4, 4), sample(5, 3)]
    assert ug.state_segments(samples, since=0, now=6, gap_sec=5) == [(0, 3, 2), (3, 5, 4), (5, 6, 3)]


def test_a_gap_ends_the_run_at_the_last_sample():
    samples = [sample(0, 2), sample(1, 2), sample(20, 2)]
    assert ug.state_segments(samples, since=0, now=21, gap_sec=5) == [(0, 1, 2), (20, 21, 2)]


def test_a_stale_last_sample_does_not_reach_now():
    assert ug.state_segments([sample(0, 2)], since=0, now=30, gap_sec=5) == [(0, 0, 2)]


def test_samples_without_a_state_are_skipped_and_starts_clamp_to_since():
    samples = [sample(-10, 2), sample(1, None), sample(2, 2)]
    assert ug.state_segments(samples, since=0, now=3, gap_sec=20) == [(0, 3, 2)]


def test_no_samples_no_segments():
    assert ug.state_segments([], since=0, now=10) == []
