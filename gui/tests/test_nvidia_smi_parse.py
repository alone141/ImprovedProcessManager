from __future__ import annotations

import pytest

from gpu_sampler import parse_compute_apps_csv, parse_pmon_output


def test_parse_compute_apps_basic():
    text = """
1234, 256, GPU-aaa
5678, 1024, GPU-bbb
""".strip()
    rows = parse_compute_apps_csv(text)
    assert (1234, 256 * 1024 * 1024, "GPU-aaa") in rows
    assert (5678, 1024 * 1024 * 1024, "GPU-bbb") in rows


def test_parse_compute_apps_skips_bad_lines():
    text = """
# comment
not,a,row
111, 10, GPU-x
""".strip()
    rows = parse_compute_apps_csv(text)
    assert len(rows) == 1
    assert rows[0][0] == 111
    assert rows[0][1] == 10 * 1024 * 1024


def test_parse_compute_apps_empty():
    assert parse_compute_apps_csv("") == []
    assert parse_compute_apps_csv("   \n") == []


def test_parse_pmon_basic():
    # Typical pmon columns: # gpu pid type sm mem enc dec command
    text = """
# gpu        pid  type    sm   mem   enc   dec   command
# Idx          #   C/G     %     %     %     %   name
    0       1234     C    45     3     0     0   python
    1       1234     C    10     1     0     0   python
    0       9999     C     5     2     0     0   other
""".strip()
    rows = parse_pmon_output(text)
    pids = {(p, u, g) for p, u, g in rows}
    assert (1234, 45.0, "0") in pids
    assert (1234, 10.0, "1") in pids
    assert (9999, 5.0, "0") in pids


def test_parse_pmon_skips_dash_pid_and_headers():
    text = """
# gpu pid type sm mem
  0   -   -   -  -
  0  42   C  12  1  myapp
""".strip()
    rows = parse_pmon_output(text)
    assert len(rows) == 1
    assert rows[0][0] == 42
    assert rows[0][1] == 12.0


def test_parse_compute_apps_keeps_rows_without_memory():
    # WDDM drivers on Windows report no per-process memory.
    text = "1234, [N/A], GPU-aaa\n5678, [Not Supported], GPU-bbb"
    assert parse_compute_apps_csv(text) == [(1234, None, "GPU-aaa"), (5678, None, "GPU-bbb")]
