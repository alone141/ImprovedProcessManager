from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Tuple

import pytest

from gpu_sampler import GpuProcessUsage, GpuSampler


@dataclass
class FakeBackend:
    """Injectable stand-in matching GpuBackend protocol."""
    vram_rows: List[Tuple[int, int, str]] = field(default_factory=list)
    util_rows: List[Tuple[int, float, str]] = field(default_factory=list)
    fail_init: bool = False
    fail_sample: bool = False
    init_called: bool = False
    shutdown_called: bool = False

    def init(self) -> None:
        self.init_called = True
        if self.fail_init:
            raise RuntimeError("fake init failed")

    def shutdown(self) -> None:
        self.shutdown_called = True

    def list_process_vram(self) -> List[Tuple[int, int, str]]:
        if self.fail_sample:
            raise RuntimeError("fake sample failed")
        return list(self.vram_rows)

    def list_process_util(self) -> List[Tuple[int, float, str]]:
        if self.fail_sample:
            raise RuntimeError("fake sample failed")
        return list(self.util_rows)


def test_init_failure_sets_unavailable():
    s = GpuSampler(backend=FakeBackend(fail_init=True))
    assert s.init() is False
    assert s.available() is False
    assert s.last_error() is not None
    assert s.sample() == {}


def test_empty_rows_returns_empty_map():
    s = GpuSampler(backend=FakeBackend())
    assert s.init() is True
    assert s.sample() == {}


def test_multi_gpu_sums_vram_and_util():
    s = GpuSampler(
        backend=FakeBackend(
            vram_rows=[
                (1001, 1_000_000, "GPU0"),
                (1001, 2_000_000, "GPU1"),
            ],
            util_rows=[
                (1001, 10.0, "GPU0"),
                (1001, 20.0, "GPU1"),
            ],
        )
    )
    s.init()
    r = s.sample()
    assert r[1001].vram_bytes == 3_000_000
    assert r[1001].util_pct == pytest.approx(30.0)


def test_within_gpu_vram_max_not_sum():
    s = GpuSampler(
        backend=FakeBackend(
            vram_rows=[
                (42, 5000, "GPU0"),
                (42, 5000, "GPU0"),
                (42, 8000, "GPU0"),
            ],
        )
    )
    s.init()
    r = s.sample()
    assert r[42].vram_bytes == 8000
    assert r[42].util_pct is None


def test_within_gpu_util_max_then_sum_across():
    s = GpuSampler(
        backend=FakeBackend(
            vram_rows=[(7, 100, "0"), (7, 100, "1")],
            util_rows=[
                (7, 10.0, "0"),
                (7, 40.0, "0"),  # max on gpu0 = 40
                (7, 20.0, "1"),
            ],
        )
    )
    s.init()
    r = s.sample()
    assert r[7].util_pct == pytest.approx(60.0)  # 40 + 20


def test_vram_without_util_sets_util_none():
    s = GpuSampler(backend=FakeBackend(vram_rows=[(9, 4096, "GPU0")]))
    s.init()
    r = s.sample()
    assert r[9].vram_bytes == 4096
    assert r[9].util_pct is None


def test_sample_failure_returns_empty_and_sets_error():
    s = GpuSampler(backend=FakeBackend(fail_sample=True))
    s.init()
    assert s.sample() == {}
    assert s.last_error() is not None


def test_shutdown_calls_backend():
    b = FakeBackend()
    s = GpuSampler(backend=b)
    s.init()
    s.shutdown()
    assert b.shutdown_called is True
    assert s.available() is False


def test_two_pids_independent():
    s = GpuSampler(
        backend=FakeBackend(
            vram_rows=[(10, 100, "0"), (20, 200, "0")],
            util_rows=[(10, 5.0, "0"), (20, 15.0, "0")],
        )
    )
    s.init()
    r = s.sample()
    assert r[10].vram_bytes == 100 and r[10].util_pct == pytest.approx(5.0)
    assert r[20].vram_bytes == 200 and r[20].util_pct == pytest.approx(15.0)


def test_unreported_vram_stays_unknown():
    s = GpuSampler(backend=FakeBackend(vram_rows=[(5, None, "GPU0")], util_rows=[(5, 20.0, "GPU0")]))
    s.init()
    r = s.sample()
    assert r[5].vram_bytes is None
    assert r[5].util_pct == pytest.approx(20.0)


@pytest.mark.parametrize("rows", [
    [(5, None, "GPU0"), (5, 1000, "GPU1")],
    [(5, 1000, "GPU1"), (5, None, "GPU0")],
    [(5, None, "GPU0"), (5, 1000, "GPU0")],
])
def test_reported_vram_wins_over_unreported(rows):
    s = GpuSampler(backend=FakeBackend(vram_rows=rows))
    s.init()
    assert s.sample()[5].vram_bytes == 1000
