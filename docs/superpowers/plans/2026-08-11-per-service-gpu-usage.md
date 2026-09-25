# Per-Service GPU Usage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show per-service GPU utilization % and VRAM in the Process Manager GUI by sampling NVIDIA NVML locally and joining by PID from health reports.

**Architecture:** Add a pure-Python `GpuSampler` that talks to NVML (or an injectable fake backend), returns `dict[pid → GpuProcessUsage]`. The main window samples on the existing 1s timer, caches `_gpu_by_pid`, and fills two new table columns. ZMQ protocol and `health_structs.py` stay unchanged.

**Tech Stack:** Python 3, PyQt6, pyzmq, pynvml, pytest (tests only)

**Spec:** `docs/superpowers/specs/2026-08-11-per-service-gpu-usage-design.md`

## Global Constraints

- No changes to ZMQ wire format, `DetailedHealthReport` size, or command messages
- NVIDIA NVML only; multi-GPU metrics are **summed per PID**
- Missing / no GPU: cell text is the Unicode em dash `"—"` (U+2014)
- On failed sample: clear `_gpu_by_pid` to `{}` (no stale multi-minute values)
- Never invent per-process util from device-wide counters
- Commits optional (project may not use git)

---

## File Structure

| File | Responsibility |
|------|----------------|
| `gpu_sampler.py` | **Create.** NVML wrapper + aggregation; injectable backend for tests |
| `tests/test_gpu_sampler.py` | **Create.** Unit tests with fake backend (no GPU required) |
| `requirements.txt` | **Create.** `PyQt6`, `pyzmq`, `pynvml` |
| `process_monitor_gui.py` | **Modify.** Columns, toolbar GPU label, timer sample + join by PID |
| `health_structs.py` | Unchanged |
| `mock_publisher.py` | Unchanged |

---

### Task 1: Dependencies

**Files:**
- Create: `requirements.txt`

**Interfaces:**
- Consumes: nothing
- Produces: installable deps for runtime (`pynvml` for Task 2+)

- [ ] **Step 1: Create `requirements.txt`**

```text
PyQt6>=6.4
pyzmq>=25.0
pynvml>=11.5
```

- [ ] **Step 2: Install (dev machine)**

Run: `pip install -r requirements.txt pytest`

Expected: packages install without error. (If no NVIDIA driver, `pynvml` still installs; init will fail at runtime, which is handled later.)

---

### Task 2: `GpuSampler` data types and fake-backend aggregation (TDD)

**Files:**
- Create: `gpu_sampler.py`
- Create: `tests/test_gpu_sampler.py`

**Interfaces:**
- Consumes: nothing external in pure unit tests
- Produces:
  - `@dataclass GpuProcessUsage` with `util_pct: float | None`, `vram_bytes: int`
  - `class NvmlBackend` protocol / duck type with methods used by `GpuSampler`
  - `class GpuSampler` with:
    - `__init__(self, backend: NvmlBackend | None = None)`
    - `init(self) -> bool`
    - `shutdown(self) -> None`
    - `available(self) -> bool`
    - `last_error(self) -> str | None`
    - `sample(self) -> dict[int, GpuProcessUsage]`

**Fake backend contract** (implement in tests and call from `GpuSampler` when injected):

```python
# Methods GpuSampler calls on backend (real or fake):
# backend.init() -> None   # may raise
# backend.shutdown() -> None
# backend.device_count() -> int
# backend.compute_procs(device_index: int) -> list[tuple[int, int]]
#     # (pid, used_gpu_memory_bytes)
# backend.graphics_procs(device_index: int) -> list[tuple[int, int]]
# backend.process_utilization(device_index: int) -> list[tuple[int, float]]
#     # (pid, util_pct); empty list if not supported
```

- [ ] **Step 1: Write failing tests**

Create `tests/test_gpu_sampler.py`:

```python
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import pytest

from gpu_sampler import GpuProcessUsage, GpuSampler


@dataclass
class FakeBackend:
    """Injectable NVML stand-in for unit tests."""
    devices: int = 0
    # device_index -> list of (pid, vram_bytes)
    compute: Dict[int, List[Tuple[int, int]]] = field(default_factory=dict)
    graphics: Dict[int, List[Tuple[int, int]]] = field(default_factory=dict)
    # device_index -> list of (pid, util_pct)
    util: Dict[int, List[Tuple[int, float]]] = field(default_factory=dict)
    fail_init: bool = False
    fail_sample: bool = False
    init_called: bool = False
    shutdown_called: bool = False

    def init(self) -> None:
        self.init_called = True
        if self.fail_init:
            raise RuntimeError("nvml fake init failed")

    def shutdown(self) -> None:
        self.shutdown_called = True

    def device_count(self) -> int:
        if self.fail_sample:
            raise RuntimeError("nvml fake sample failed")
        return self.devices

    def compute_procs(self, device_index: int) -> List[Tuple[int, int]]:
        return list(self.compute.get(device_index, []))

    def graphics_procs(self, device_index: int) -> List[Tuple[int, int]]:
        return list(self.graphics.get(device_index, []))

    def process_utilization(self, device_index: int) -> List[Tuple[int, float]]:
        return list(self.util.get(device_index, []))


def test_init_failure_sets_unavailable():
    backend = FakeBackend(fail_init=True)
    s = GpuSampler(backend=backend)
    assert s.init() is False
    assert s.available() is False
    assert s.last_error() is not None
    assert "fail" in (s.last_error() or "").lower() or "nvml" in (s.last_error() or "").lower()
    assert s.sample() == {}


def test_empty_devices_returns_empty_map():
    backend = FakeBackend(devices=0)
    s = GpuSampler(backend=backend)
    assert s.init() is True
    assert s.available() is True
    assert s.sample() == {}


def test_multi_gpu_sums_vram_and_util_for_same_pid():
    backend = FakeBackend(
        devices=2,
        compute={
            0: [(1001, 1_000_000)],
            1: [(1001, 2_000_000)],
        },
        util={
            0: [(1001, 10.0)],
            1: [(1001, 20.0)],
        },
    )
    s = GpuSampler(backend=backend)
    assert s.init() is True
    result = s.sample()
    assert 1001 in result
    assert result[1001].vram_bytes == 3_000_000
    assert result[1001].util_pct == pytest.approx(30.0)


def test_two_pids_independent():
    backend = FakeBackend(
        devices=1,
        compute={0: [(10, 100), (20, 200)]},
        util={0: [(10, 5.0), (20, 15.0)]},
    )
    s = GpuSampler(backend=backend)
    s.init()
    result = s.sample()
    assert result[10].vram_bytes == 100
    assert result[10].util_pct == pytest.approx(5.0)
    assert result[20].vram_bytes == 200
    assert result[20].util_pct == pytest.approx(15.0)


def test_same_pid_on_compute_and_graphics_same_device_no_double_vram():
    """Same PID listed on compute + graphics with same memory → count once per device."""
    backend = FakeBackend(
        devices=1,
        compute={0: [(42, 5000)]},
        graphics={0: [(42, 5000)]},
        util={0: [(42, 12.0)]},
    )
    s = GpuSampler(backend=backend)
    s.init()
    result = s.sample()
    assert result[42].vram_bytes == 5000
    assert result[42].util_pct == pytest.approx(12.0)


def test_vram_without_util_sets_util_none():
    backend = FakeBackend(
        devices=1,
        compute={0: [(7, 4096)]},
        util={},  # no util API data
    )
    s = GpuSampler(backend=backend)
    s.init()
    result = s.sample()
    assert result[7].vram_bytes == 4096
    assert result[7].util_pct is None


def test_sample_failure_returns_empty_and_sets_error():
    backend = FakeBackend(devices=1, fail_sample=True)
    s = GpuSampler(backend=backend)
    s.init()
    assert s.sample() == {}
    assert s.last_error() is not None


def test_shutdown_calls_backend():
    backend = FakeBackend(devices=0)
    s = GpuSampler(backend=backend)
    s.init()
    s.shutdown()
    assert backend.shutdown_called is True
    assert s.available() is False
```

- [ ] **Step 2: Run tests — expect FAIL**

Run from project root:

```bash
pytest tests/test_gpu_sampler.py -v
```

Expected: FAIL with `ModuleNotFoundError: No module named 'gpu_sampler'` (or import errors for missing symbols).

- [ ] **Step 3: Implement `gpu_sampler.py`**

Create `gpu_sampler.py`:

```python
"""Local NVIDIA GPU sampling by PID (NVML). No Qt / no ZMQ."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Protocol, Tuple


@dataclass
class GpuProcessUsage:
    util_pct: Optional[float]  # None if util not available for this PID
    vram_bytes: int


class NvmlBackend(Protocol):
    def init(self) -> None: ...
    def shutdown(self) -> None: ...
    def device_count(self) -> int: ...
    def compute_procs(self, device_index: int) -> List[Tuple[int, int]]: ...
    def graphics_procs(self, device_index: int) -> List[Tuple[int, int]]: ...
    def process_utilization(self, device_index: int) -> List[Tuple[int, float]]: ...


class PynvmlBackend:
    """Real NVML via pynvml. Used when GpuSampler(backend=None)."""

    def __init__(self) -> None:
        self._nvml = None

    def init(self) -> None:
        import pynvml  # type: ignore

        self._nvml = pynvml
        pynvml.nvmlInit()

    def shutdown(self) -> None:
        if self._nvml is not None:
            try:
                self._nvml.nvmlShutdown()
            except Exception:
                pass
            self._nvml = None

    def device_count(self) -> int:
        assert self._nvml is not None
        return int(self._nvml.nvmlDeviceGetCount())

    def _handle(self, index: int):
        assert self._nvml is not None
        return self._nvml.nvmlDeviceGetHandleByIndex(index)

    def compute_procs(self, device_index: int) -> List[Tuple[int, int]]:
        assert self._nvml is not None
        h = self._handle(device_index)
        try:
            procs = self._nvml.nvmlDeviceGetComputeRunningProcesses(h)
        except self._nvml.NVMLError:
            return []
        out: List[Tuple[int, int]] = []
        for p in procs:
            pid = int(p.pid)
            mem = int(getattr(p, "usedGpuMemory", 0) or 0)
            if mem < 0:  # some drivers use max-uint as unknown
                mem = 0
            out.append((pid, mem))
        return out

    def graphics_procs(self, device_index: int) -> List[Tuple[int, int]]:
        assert self._nvml is not None
        h = self._handle(device_index)
        try:
            procs = self._nvml.nvmlDeviceGetGraphicsRunningProcesses(h)
        except self._nvml.NVMLError:
            return []
        out: List[Tuple[int, int]] = []
        for p in procs:
            pid = int(p.pid)
            mem = int(getattr(p, "usedGpuMemory", 0) or 0)
            if mem < 0:
                mem = 0
            out.append((pid, mem))
        return out

    def process_utilization(self, device_index: int) -> List[Tuple[int, float]]:
        """Best-effort per-process util. Empty if unsupported."""
        assert self._nvml is not None
        h = self._handle(device_index)
        # lastSeenTimeUs=0: request recent samples (driver-dependent)
        try:
            samples = self._nvml.nvmlDeviceGetProcessUtilization(h, 0)
        except Exception:
            return []
        out: List[Tuple[int, float]] = []
        for s in samples or []:
            pid = int(s.pid)
            # smUtil is streaming multiprocessor util % when present
            sm = getattr(s, "smUtil", None)
            if sm is None:
                continue
            out.append((pid, float(sm)))
        return out


class GpuSampler:
    def __init__(self, backend: Optional[NvmlBackend] = None) -> None:
        self._backend: Optional[NvmlBackend] = backend
        self._owns_backend = backend is None
        self._available = False
        self._error: Optional[str] = None
        self._inited = False

    def init(self) -> bool:
        if self._backend is None:
            try:
                self._backend = PynvmlBackend()
            except Exception as e:
                self._error = f"pynvml import failed: {e}"
                self._available = False
                return False
        try:
            self._backend.init()
            self._available = True
            self._inited = True
            self._error = None
            return True
        except Exception as e:
            self._error = str(e)
            self._available = False
            self._inited = False
            return False

    def shutdown(self) -> None:
        if self._backend is not None and self._inited:
            try:
                self._backend.shutdown()
            except Exception:
                pass
        self._available = False
        self._inited = False

    def available(self) -> bool:
        return self._available

    def last_error(self) -> Optional[str]:
        return self._error

    def sample(self) -> Dict[int, GpuProcessUsage]:
        if not self._available or self._backend is None:
            return {}
        try:
            n = self._backend.device_count()
            # per pid: vram sum; util sum; track (device, pid) seen for vram dedupe
            vram: Dict[int, int] = {}
            util: Dict[int, float] = {}
            util_seen: Dict[int, bool] = {}
            seen_vram_keys: set[tuple[int, int]] = set()  # (device, pid)

            for dev in range(n):
                mem_by_pid: Dict[int, int] = {}
                for pid, mem in self._backend.compute_procs(dev):
                    if pid <= 0:
                        continue
                    prev = mem_by_pid.get(pid, 0)
                    if mem > prev:
                        mem_by_pid[pid] = mem
                for pid, mem in self._backend.graphics_procs(dev):
                    if pid <= 0:
                        continue
                    prev = mem_by_pid.get(pid, 0)
                    if mem > prev:
                        mem_by_pid[pid] = mem
                for pid, mem in mem_by_pid.items():
                    key = (dev, pid)
                    if key in seen_vram_keys:
                        continue
                    seen_vram_keys.add(key)
                    vram[pid] = vram.get(pid, 0) + mem

                for pid, u in self._backend.process_utilization(dev):
                    if pid <= 0:
                        continue
                    util[pid] = util.get(pid, 0.0) + float(u)
                    util_seen[pid] = True

            out: Dict[int, GpuProcessUsage] = {}
            for pid, mem in vram.items():
                u = util.get(pid) if util_seen.get(pid) else None
                # util-only without vram: still include if we only saw util
                out[pid] = GpuProcessUsage(util_pct=u, vram_bytes=mem)
            for pid, u in util.items():
                if pid not in out:
                    out[pid] = GpuProcessUsage(util_pct=u, vram_bytes=0)
            self._error = None
            return out
        except Exception as e:
            self._error = str(e)
            return {}
```

**Aggregation rules (must match tests):**

1. Per device: take max VRAM for a PID across compute/graphics lists (same PID twice same mem → once).
2. Across devices: **sum** VRAM and **sum** util for the same PID.
3. If PID has VRAM but no util samples → `util_pct=None`.

- [ ] **Step 4: Run tests — expect PASS**

```bash
pytest tests/test_gpu_sampler.py -v
```

Expected: all tests PASS.

---

### Task 3: Wire GPU columns and sampler into the GUI

**Files:**
- Modify: `process_monitor_gui.py`

**Interfaces:**
- Consumes: `GpuSampler`, `GpuProcessUsage` from `gpu_sampler.py` (Task 2)
- Produces: table columns `GPU %`, `VRAM`; toolbar `lbl_gpu`; `_gpu_by_pid` cache; sampling in `_refresh_status`

- [ ] **Step 1: Update imports and COLUMNS**

Near the top of `process_monitor_gui.py`, add:

```python
from gpu_sampler import GpuSampler, GpuProcessUsage
```

Replace `COLUMNS` with:

```python
COLUMNS = [
    "Process", "PID", "State", "Memory", "CPU %",
    "GPU %", "VRAM",
    "Uptime", "Missed", "Restarts", "Last Seen", "Actions",
]
```

- [ ] **Step 2: Init sampler and cache in `ProcessMonitorWindow.__init__`**

After `self._current: Dict[str, dict] = {}` add:

```python
self._gpu_by_pid: Dict[int, GpuProcessUsage] = {}
self._gpu_sampler = GpuSampler()
self._gpu_sampler.init()
```

Keep the existing 1s timer connected to `_refresh_status`.

- [ ] **Step 3: Add toolbar GPU status label**

In `_build_ui`, after `toolbar.addWidget(self.lbl_status)`, add:

```python
toolbar.addSeparator()
self.lbl_gpu = QLabel("GPU: …")
self.lbl_gpu.setStyleSheet("color: #aaa; font-weight: bold; padding: 0 12px;")
toolbar.addWidget(self.lbl_gpu)
self._update_gpu_status_label()
```

Add method:

```python
def _update_gpu_status_label(self) -> None:
    if self._gpu_sampler.available():
        self.lbl_gpu.setText("GPU: OK")
        self.lbl_gpu.setStyleSheet(
            "color: #4caf50; font-weight: bold; padding: 0 12px;"
        )
        self.lbl_gpu.setToolTip("NVIDIA NVML sampling active")
    else:
        err = self._gpu_sampler.last_error() or "NVML unavailable"
        self.lbl_gpu.setText("GPU: unavailable")
        self.lbl_gpu.setStyleSheet(
            "color: #f44336; font-weight: bold; padding: 0 12px;"
        )
        self.lbl_gpu.setToolTip(err)
```

- [ ] **Step 4: Implement sampling in `_refresh_status`**

Replace the empty `_refresh_status` body:

```python
def _refresh_status(self) -> None:
    if not self._gpu_sampler.available():
        # Keep map empty; still refresh label in case state changed
        self._gpu_by_pid = {}
        self._update_gpu_status_label()
        if self._current:
            self._rebuild_table()
        return

    result = self._gpu_sampler.sample()
    if self._gpu_sampler.last_error() and not result:
        # failed sample → clear (stale policy)
        self._gpu_by_pid = {}
        self.statusBar().showMessage(
            f"GPU sample error: {self._gpu_sampler.last_error()}", 3000
        )
    else:
        self._gpu_by_pid = result
    self._update_gpu_status_label()
    if self._current:
        self._rebuild_table()
```

- [ ] **Step 5: Join by PID in `_rebuild_table`**

In the row loop, after computing `cpu_pct` item list, insert GPU % and VRAM items **after** the CPU % item and **before** Uptime.

Replace the `items = [...]` block with logic equivalent to:

```python
pid = r["pid"]
gpu_usage = self._gpu_by_pid.get(pid) if pid else None
if gpu_usage is None:
    gpu_pct_text = "—"
    vram_text = "—"
else:
    if gpu_usage.util_pct is None:
        gpu_pct_text = "—"
    else:
        gpu_pct_text = f"{gpu_usage.util_pct:.1f}"
    vram_text = format_bytes(gpu_usage.vram_bytes)

items = [
    QTableWidgetItem(name),
    QTableWidgetItem(str(r["pid"]) if r["pid"] else "—"),
    QTableWidgetItem(state.name),
    QTableWidgetItem(format_bytes(r["memoryUsageInBytes"])),
    QTableWidgetItem(f"{r['_cpu_pct']:.1f}"),
    QTableWidgetItem(gpu_pct_text),
    QTableWidgetItem(vram_text),
    QTableWidgetItem(format_duration_ns(uptime_ns)),
    QTableWidgetItem(str(r["missedBeats"])),
    QTableWidgetItem(str(r["restartCount"])),
    QTableWidgetItem(format_duration_ns(last_seen_ns) + " ago"),
]
```

Ensure Actions column still uses `len(COLUMNS) - 1` for the cell widget (already does — remains correct after COLUMNS update).

- [ ] **Step 6: Shutdown sampler in `closeEvent`**

```python
def closeEvent(self, event):
    if hasattr(self, "_ui_timer") and self._ui_timer:
        self._ui_timer.stop()
    if hasattr(self, "_gpu_sampler") and self._gpu_sampler:
        self._gpu_sampler.shutdown()
    if hasattr(self, "worker") and self.worker:
        self.worker.stop()
    if hasattr(self, "worker_thread") and self.worker_thread:
        self.worker_thread.quit()
        self.worker_thread.wait(1500)
    super().closeEvent(event)
```

- [ ] **Step 7: Sanity-check imports**

Run:

```bash
python -c "from process_monitor_gui import COLUMNS; assert 'GPU %' in COLUMNS and 'VRAM' in COLUMNS; from gpu_sampler import GpuSampler; s=GpuSampler(); print('init', s.init(), s.last_error()); s.shutdown()"
```

Expected: prints `init True ...` if NVIDIA/NVML present, or `init False ...` with an error string if not. Either way, **no traceback**.

- [ ] **Step 8: Re-run unit tests**

```bash
pytest tests/test_gpu_sampler.py -v
```

Expected: all PASS.

---

### Task 4: Manual verification + brief README note

**Files:**
- Modify: `README_ProcessManagerGUI.md` (short dependency + GPU note only)

- [ ] **Step 1: Manual check without GPU / with mock**

Terminal 1:

```bash
python mock_publisher.py
```

Terminal 2:

```bash
python process_monitor_gui.py
```

Verify:

1. Table has **GPU %** and **VRAM** columns after **CPU %**.
2. With mock-only PIDs (not real GPU processes), both columns show `"—"`.
3. Toolbar shows `GPU: OK` (driver present) or `GPU: unavailable` (tooltip has reason).
4. Start/Stop still work against the mock.
5. GUI does not crash if `pynvml` fails.

- [ ] **Step 2: Manual check with real GPU process (optional)**

If a local process uses the GPU and its PID appears in the health table (manager must report that real PID), confirm GPU % and VRAM update ~every second.

- [ ] **Step 3: Update README dependencies**

In `README_ProcessManagerGUI.md`, replace the install blurb to match reality:

```markdown
## Dependencies

```bash
pip install -r requirements.txt
```

Packages: PyQt6, pyzmq, pynvml (NVIDIA GPU columns; optional at runtime — GUI works without a GPU).

## GPU columns

The GUI samples **local** NVIDIA GPU usage via NVML and joins by **PID** from health reports. Multi-GPU usage is summed per process. If NVML is unavailable, GPU columns show "—" and the toolbar shows `GPU: unavailable`.
```

Also fix the quick-start script name to `process_monitor_gui.py` if the README still says `zmq_process_manager_gui.py`.

---

## Spec coverage checklist

| Spec requirement | Task |
|------------------|------|
| Local NVML sampling, join by PID | 2, 3 |
| GPU % + VRAM columns after CPU % | 3 |
| Multi-GPU sum | 2 (`test_multi_gpu_sums_...`) |
| `"—"` when missing; toolbar warning | 3 |
| Failed sample clears cache | 3 (`_refresh_status`) |
| `util_pct=None` shows VRAM only | 2 + 3 |
| No wire protocol change | (no tasks touch health/command structs) |
| Unit tests with fake backend | 2 |
| `requirements.txt` / pynvml | 1, 4 |
| Mock publisher unchanged | — |

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-11-per-service-gpu-usage.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — implement tasks in this session with checkpoints  

Which approach?
