# nvidia-smi GPU Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace pynvml/NVML with an nvidia-smi CLI backend so GPU columns work without installing any Python GPU package.

**Architecture:** Keep `GpuSampler` public API and GUI join logic. Introduce a new backend protocol (`list_process_vram` / `list_process_util`), implement `NvidiaSmiBackend` via subprocess + parsers, rewrite aggregation in `sample()`, remove all pynvml code and dependency.

**Tech Stack:** Python 3 stdlib (`subprocess`, `shutil`), PyQt6 (unchanged GUI), pytest

**Spec:** `docs/superpowers/specs/2026-08-14-nvidia-smi-gpu-backend-design.md`

## Global Constraints

- No required `pynvml` / `nvidia-ml-py` dependency
- Data source: local **nvidia-smi** only (`shell=False`, timeout ~3s per call)
- VRAM from compute-apps CSV; util best-effort from `pmon -c 1 -s u`
- Multi-GPU: max per `(gpu_id, pid)`, then **sum** across GPUs
- Missing util → `util_pct=None` (UI shows `"—"` for GPU %)
- GUI columns / PID join / toolbar behavior preserved
- No ZMQ / health_structs changes
- Commits optional (git may be unavailable)

---

## File Structure

| File | Action |
|------|--------|
| `gpu_sampler.py` | **Rewrite.** Drop pynvml; add parsers + `NvidiaSmiBackend` + new aggregation |
| `tests/test_gpu_sampler.py` | **Rewrite.** FakeBackend on new protocol + aggregation tests |
| `tests/test_nvidia_smi_parse.py` | **Create.** Pure parser tests from string fixtures |
| `requirements.txt` | **Modify.** Remove `pynvml` |
| `process_monitor_gui.py` | **Tiny.** Tooltip / fallback strings: nvidia-smi not NVML |
| `scripts/check_gpu_sampler.py` | **Create.** Diagnostic; delete or replace `check_pynvml.py` |
| `README_ProcessManagerGUI.md` | **Modify.** Document nvidia-smi, no pynvml |

---

### Task 1: Parser pure functions + unit tests (TDD)

**Files:**
- Modify: `gpu_sampler.py` (add parse helpers only first, or full module later — this task owns parse functions)
- Create: `tests/test_nvidia_smi_parse.py`

**Interfaces:**
- Produces:
  - `parse_compute_apps_csv(text: str) -> list[tuple[int, int, str]]`  
    → `(pid, vram_bytes, gpu_id)`  
    Memory field is MiB (nounits); convert with `* 1024 * 1024`.  
    `gpu_id` is the uuid string (or empty string if missing).
  - `parse_pmon_output(text: str) -> list[tuple[int, float, str]]`  
    → `(pid, util_pct, gpu_id)`  
    Use SM util column when present; skip header / non-numeric / pid `-` / `[Not Found]`.

- [ ] **Step 1: Write failing parser tests**

Create `tests/test_nvidia_smi_parse.py`:

```python
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
```

- [ ] **Step 2: Run tests — expect FAIL**

```bash
python -m pytest tests/test_nvidia_smi_parse.py -v
```

Expected: FAIL import / function not found.

- [ ] **Step 3: Implement parsers in `gpu_sampler.py`**

Add (module-level, no class yet if preferred):

```python
def parse_compute_apps_csv(text: str) -> List[Tuple[int, int, str]]:
    """
    Parse: nvidia-smi --query-compute-apps=pid,used_gpu_memory,gpu_uuid
                     --format=csv,noheader,nounits
    Memory is MiB → bytes.
    """
    out: List[Tuple[int, int, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 2:
            continue
        try:
            pid = int(parts[0])
            mib = float(parts[1])
        except ValueError:
            continue
        if pid <= 0:
            continue
        gpu_id = parts[2] if len(parts) >= 3 else ""
        bytes_ = int(mib * 1024 * 1024)
        out.append((pid, bytes_, gpu_id))
    return out


def parse_pmon_output(text: str) -> List[Tuple[int, float, str]]:
    """
    Parse: nvidia-smi pmon -c 1 -s u
    Columns are whitespace-separated; skip headers and non-numeric pid/sm.
    gpu_id = first column (device index as str).
    """
    out: List[Tuple[int, float, str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        gpu_id = parts[0]
        try:
            pid = int(parts[1])
        except ValueError:
            continue
        if pid <= 0:
            continue
        # type is often parts[2]; sm util often parts[3]
        sm_token = parts[3]
        if sm_token in ("-", "N/A"):
            continue
        try:
            sm = float(sm_token)
        except ValueError:
            continue
        out.append((pid, sm, gpu_id))
    return out
```

Adjust column index if real `pmon` layout on Windows differs — fixtures define the contract; document in comments.

- [ ] **Step 4: Run parser tests — expect PASS**

```bash
python -m pytest tests/test_nvidia_smi_parse.py -v
```

Expected: all PASS.

---

### Task 2: Rewrite `GpuSampler` + FakeBackend aggregation tests (TDD)

**Files:**
- Modify: `gpu_sampler.py`
- Modify: `tests/test_gpu_sampler.py` (full rewrite of FakeBackend + tests)

**Interfaces:**
- Produces:
  - `class GpuBackend(Protocol)`:
    - `init(self) -> None`
    - `shutdown(self) -> None`
    - `list_process_vram(self) -> list[tuple[int, int, str]]`
    - `list_process_util(self) -> list[tuple[int, float, str]]`
  - `GpuSampler(backend: GpuBackend | None = None)`
  - `init() -> bool`, `shutdown()`, `available()`, `last_error()`, `sample() -> dict[int, GpuProcessUsage]`
  - Aggregation only in `sample()`:
    1. VRAM: max per `(gpu_id, pid)`, then sum across gpu_id
    2. Util: max per `(gpu_id, pid)`, then sum across gpu_id
    3. VRAM without util → `util_pct=None`
  - Default backend construction may still be stubbed until Task 3; for Task 2, **tests always inject FakeBackend**. Temporary: if `backend is None`, try construct placeholder that fails init with clear message OR implement full NvidiaSmiBackend in Task 3. **Preferred:** Task 2 implements aggregation + protocol; default backend = `NvidiaSmiBackend` only if Task 3 is combined — keep Task 2 default as `NvidiaSmiBackend` class stub that raises `NotImplementedError` on init **OR** implement Task 2+3 in sequence same day.

  **Plan choice:** Task 2 rewrites sampler aggregation + FakeBackend tests. Default backend remains missing until Task 3 adds `NvidiaSmiBackend`; until then:

```python
if self._backend is None:
    from ... # will be NvidiaSmiBackend after Task 3
```

  For Task 2 only: if backend is None, set error `"no backend"` and return False — Task 3 wires default.

- [ ] **Step 1: Rewrite `tests/test_gpu_sampler.py` for new protocol**

```python
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Tuple

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
```

- [ ] **Step 2: Run tests — expect FAIL** (old FakeBackend / API)

```bash
python -m pytest tests/test_gpu_sampler.py -v
```

- [ ] **Step 3: Rewrite `GpuSampler` aggregation + protocol in `gpu_sampler.py`**

Remove `PynvmlBackend`, old `NvmlBackend` protocol methods (`device_count`, `compute_procs`, …).

Keep parsers from Task 1. Implement:

```python
class GpuBackend(Protocol):
    def init(self) -> None: ...
    def shutdown(self) -> None: ...
    def list_process_vram(self) -> List[Tuple[int, int, str]]: ...
    def list_process_util(self) -> List[Tuple[int, float, str]]: ...


class GpuSampler:
    def __init__(self, backend: Optional[GpuBackend] = None) -> None:
        self._backend = backend
        self._available = False
        self._error: Optional[str] = None
        self._inited = False

    def init(self) -> bool:
        if self._backend is None:
            # Task 3 sets: self._backend = NvidiaSmiBackend()
            # Temporary until Task 3:
            try:
                self._backend = NvidiaSmiBackend()  # may not exist yet — Task 3
            except NameError:
                self._error = "NvidiaSmiBackend not implemented"
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

    def sample(self) -> Dict[int, GpuProcessUsage]:
        if not self._available or self._backend is None:
            return {}
        try:
            vram_rows = self._backend.list_process_vram()
            util_rows = self._backend.list_process_util()

            vram_max: Dict[Tuple[str, int], int] = {}
            for pid, mem, gpu_id in vram_rows:
                if pid <= 0:
                    continue
                key = (gpu_id, pid)
                prev = vram_max.get(key)
                if prev is None or mem > prev:
                    vram_max[key] = mem
            vram: Dict[int, int] = {}
            for (gpu_id, pid), mem in vram_max.items():
                vram[pid] = vram.get(pid, 0) + mem

            util_max: Dict[Tuple[str, int], float] = {}
            for pid, u, gpu_id in util_rows:
                if pid <= 0:
                    continue
                key = (gpu_id, pid)
                prev = util_max.get(key)
                if prev is None or u > prev:
                    util_max[key] = float(u)
            util: Dict[int, float] = {}
            util_seen: Dict[int, bool] = {}
            for (gpu_id, pid), u in util_max.items():
                util[pid] = util.get(pid, 0.0) + u
                util_seen[pid] = True

            out: Dict[int, GpuProcessUsage] = {}
            for pid, mem in vram.items():
                out[pid] = GpuProcessUsage(
                    util_pct=util.get(pid) if util_seen.get(pid) else None,
                    vram_bytes=mem,
                )
            for pid, u in util.items():
                if pid not in out:
                    out[pid] = GpuProcessUsage(util_pct=u, vram_bytes=0)
            self._error = None
            return out
        except Exception as e:
            self._error = str(e)
            return {}
```

If Task 3 not yet done, define a minimal stub:

```python
class NvidiaSmiBackend:
    def init(self) -> None:
        raise RuntimeError("nvidia-smi backend not wired")
    def shutdown(self) -> None: ...
    def list_process_vram(self): return []
    def list_process_util(self): return []
```

Better: **implement full NvidiaSmiBackend in Task 3** and for Task 2 only inject FakeBackend in tests; default `None` path:

```python
if self._backend is None:
    self._backend = NvidiaSmiBackend()  # implemented in Task 3 same file
```

**Implement Task 2 and Task 3 back-to-back** so default path is never broken mid-stream.

- [ ] **Step 4: Run aggregation tests**

```bash
python -m pytest tests/test_gpu_sampler.py tests/test_nvidia_smi_parse.py -v
```

Expected: all PASS (parser + aggregation).

---

### Task 3: `NvidiaSmiBackend` (subprocess)

**Files:**
- Modify: `gpu_sampler.py` (complete `NvidiaSmiBackend`)

**Interfaces:**
- Produces: `NvidiaSmiBackend` implementing `GpuBackend`
- Constants: `SUBPROCESS_TIMEOUT_SEC = 3.0`
- Commands:
  - Probe/init: `nvidia-smi -L`
  - VRAM:  
    `nvidia-smi --query-compute-apps=pid,used_gpu_memory,gpu_uuid --format=csv,noheader,nounits`
  - Util:  
    `nvidia-smi pmon -c 1 -s u`  
    On failure of pmon only: return `[]` for util (do not fail whole sample if VRAM succeeded — **spec:** sample failure returns empty. Prefer: VRAM failure → raise/empty whole sample; util failure → empty util list only so VRAM still works.)

**Spec clarification for implementer (binding):**

- If **VRAM query** fails after successful init → `list_process_vram` raises → `sample()` returns `{}` and sets error.
- If **util/pmon** fails → return `[]` from `list_process_util` (no raise); VRAM still shown.

- [ ] **Step 1: Implement `NvidiaSmiBackend`**

```python
import shutil
import subprocess

SUBPROCESS_TIMEOUT_SEC = 3.0


class NvidiaSmiBackend:
    def __init__(self) -> None:
        self._exe: Optional[str] = None

    def init(self) -> None:
        exe = shutil.which("nvidia-smi")
        if not exe:
            raise RuntimeError("nvidia-smi not found on PATH")
        self._exe = exe
        self._run(["-L"])  # probe; raises on failure

    def shutdown(self) -> None:
        self._exe = None

    def _run(self, args: List[str]) -> str:
        if not self._exe:
            raise RuntimeError("nvidia-smi backend not initialized")
        try:
            cp = subprocess.run(
                [self._exe, *args],
                capture_output=True,
                text=True,
                timeout=SUBPROCESS_TIMEOUT_SEC,
                shell=False,
                check=False,
            )
        except subprocess.TimeoutExpired as e:
            raise RuntimeError(f"nvidia-smi timed out: {args}") from e
        if cp.returncode != 0:
            err = (cp.stderr or cp.stdout or "").strip()
            raise RuntimeError(
                f"nvidia-smi failed ({cp.returncode}): {err or args}"
            )
        return cp.stdout or ""

    def list_process_vram(self) -> List[Tuple[int, int, str]]:
        out = self._run([
            "--query-compute-apps=pid,used_gpu_memory,gpu_uuid",
            "--format=csv,noheader,nounits",
        ])
        return parse_compute_apps_csv(out)

    def list_process_util(self) -> List[Tuple[int, float, str]]:
        try:
            out = self._run(["pmon", "-c", "1", "-s", "u"])
        except RuntimeError:
            return []
        return parse_pmon_output(out)
```

Wire default in `GpuSampler.init`:

```python
if self._backend is None:
    self._backend = NvidiaSmiBackend()
```

- [ ] **Step 2: Optional integration smoke (skip if no GPU)**

```bash
python -c "from gpu_sampler import GpuSampler; s=GpuSampler(); print(s.init(), s.last_error()); print(s.sample()); s.shutdown()"
```

Expected: either `True` + dict, or `False` + clear error if no nvidia-smi.

- [ ] **Step 3: Full unit suite still green**

```bash
python -m pytest tests/ -v
```

Expected: all PASS (no real GPU required).

---

### Task 4: Deps, GUI strings, check script, README

**Files:**
- Modify: `requirements.txt` — remove `pynvml>=11.5`
- Modify: `process_monitor_gui.py` — tooltip / fallback text
- Create: `scripts/check_gpu_sampler.py`
- Delete or empty: `scripts/check_pynvml.py` (replace by new script)
- Modify: `README_ProcessManagerGUI.md`

- [ ] **Step 1: `requirements.txt`**

```text
PyQt6>=6.4
pyzmq>=25.0
```

- [ ] **Step 2: GUI tooltip strings**

In `process_monitor_gui.py` `_update_gpu_status_label`:

- Available tooltip: `"nvidia-smi sampling active"`
- Unavailable fallback: `"nvidia-smi unavailable"` (instead of `"NVML unavailable"`)

- [ ] **Step 3: `scripts/check_gpu_sampler.py`**

```python
#!/usr/bin/env python3
"""Diagnostic: nvidia-smi on PATH + GpuSampler."""
import shutil
import sys

def main() -> int:
    print("=== which nvidia-smi ===")
    exe = shutil.which("nvidia-smi")
    print(exe or "NOT FOUND")
    if not exe:
        return 1
    from gpu_sampler import GpuSampler
    s = GpuSampler()
    ok = s.init()
    print("init:", ok, s.last_error())
    if ok:
        print("sample:", s.sample())
    s.shutdown()
    return 0 if ok else 2

if __name__ == "__main__":
    sys.exit(main())
```

Remove `scripts/check_pynvml.py` after the new script works.

- [ ] **Step 4: README**

Update Dependencies and GPU sections:

- Packages: PyQt6, pyzmq only
- GPU columns use **local nvidia-smi** (must be on PATH); no Python GPU package
- VRAM from compute apps; GPU % when `pmon` provides it; else `"—"`
- Compute-apps only in v1 (CUDA processes)

- [ ] **Step 5: Final verification**

```bash
python -m pytest tests/ -v
python -c "import gpu_sampler; assert not hasattr(gpu_sampler, 'PynvmlBackend') or True"
rg -n "pynvml" --glob "!docs/**" .
```

Expected: tests PASS; no runtime `import pynvml` in project source (docs history may still mention it).

Confirm no `pynvml` in `requirements.txt`.

---

## Spec coverage checklist

| Spec requirement | Task |
|------------------|------|
| nvidia-smi only, no pynvml | 3, 4 |
| parse compute-apps CSV → VRAM bytes | 1, 3 |
| pmon util best-effort | 1, 3 |
| Multi-GPU max then sum | 2 |
| util_pct None when no util | 2 |
| GpuSampler API preserved | 2 |
| GUI join unchanged (tooltip only) | 4 |
| requirements drop pynvml | 4 |
| check script / README | 4 |
| Unit tests fake + parser fixtures | 1, 2 |
| shell=False, timeout | 3 |

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-14-nvidia-smi-gpu-backend.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks  
2. **Inline Execution** — implement in this session with checkpoints  

Which approach?
