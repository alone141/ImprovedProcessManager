# GPU Backend without pynvml (nvidia-smi)

**Date:** 2026-08-14  
**Status:** Approved for implementation planning  
**Supersedes (partially):** `docs/superpowers/specs/2026-08-11-per-service-gpu-usage-design.md` — only the **backend stack** decision. GUI columns, PID join, multi-GPU sum, and `"—"` behavior remain in force.

## Problem

The Process Manager GUI samples GPU usage locally and joins by PID. The first implementation used **pynvml**. On the target machine the operator **does not want to install** any Python GPU package. **nvidia-smi** is available (NVIDIA driver present). GPU columns must keep working without `pip install pynvml`.

## Goals

1. Obtain per-PID **VRAM** (always when processes appear in nvidia-smi) without any pip GPU dependency.
2. Obtain per-PID **GPU util %** when nvidia-smi exposes it (e.g. `pmon`); otherwise show `"—"` for util.
3. Keep the existing **`GpuSampler` public API** and GUI wiring (columns, toolbar, 1s timer, join by PID).
4. Remove **pynvml** from required dependencies (`requirements.txt`, README).

## Non-goals (v1)

- Shipping or documenting offline pynvml wheels as the primary path
- Dual backend (pynvml if present, else nvidia-smi)
- ctypes binding to `nvml.dll` / `libnvidia-ml.so`
- Changing ZMQ protocol or health structs
- Non-NVIDIA vendors
- Historical charts or per-GPU breakdown UI

## Decisions

| Topic | Decision |
|-------|----------|
| Constraint | No pynvml install |
| Data source | Local **nvidia-smi** CLI |
| Metrics | VRAM always (when listed); GPU % when pmon (or equivalent) provides it |
| Multi-GPU | Sum VRAM and util across GPUs per PID (same as prior design) |
| Architecture | **Approach A:** `NvidiaSmiBackend` behind existing `GpuSampler` |
| GUI | Unchanged join/columns/toolbar semantics |
| Deps | Drop pynvml from `requirements.txt` |

## Architecture

```
┌─────────────────────┐  health (PID, …)   ┌──────────────────────────┐
│  Process Manager    │ ─────────────────▶ │  ProcessMonitorWindow    │
└─────────────────────┘                    │  _gpu_by_pid join        │
                                           └────────────▲─────────────┘
                                                        │ sample()
┌─────────────────────┐                    ┌────────────┴─────────────┐
│  nvidia-smi (OS)    │ ◀── subprocess ─── │  GpuSampler              │
│                     │                    │    NvidiaSmiBackend      │
└─────────────────────┘                    └──────────────────────────┘
```

- **`GpuSampler`**: same surface (`init`, `shutdown`, `available`, `last_error`, `sample` → `dict[int, GpuProcessUsage]`).
- **Default backend**: `NvidiaSmiBackend` (replaces `PynvmlBackend`).
- **Injectable backend** for unit tests remains (fake backend protocol unchanged enough that tests can drive aggregation without a real GPU).

## Components

### 1. `NvidiaSmiBackend` (in `gpu_sampler.py` or small helper module)

**Responsibility:** Locate and run `nvidia-smi`, parse process VRAM and optional util; no Qt.

**Init:**

1. Resolve executable: `shutil.which("nvidia-smi")` (and on Windows, common CUDA/driver paths only if which fails — keep minimal).
2. Run a cheap probe: `nvidia-smi -L` or `nvidia-smi --query-gpu=name --format=csv,noheader` with a short timeout (e.g. 3–5 s).
3. Success → `available=True`. Failure → `available=False`, set `last_error` (not found / timeout / non-zero exit).

**Sample — VRAM (required path):**

```text
nvidia-smi --query-compute-apps=pid,used_gpu_memory,gpu_uuid
            --format=csv,noheader,nounits
```

- Parse each row: PID, memory (MiB → convert to bytes: `* 1024 * 1024`), GPU identity for multi-GPU sum.
- Same PID on multiple GPUs: **sum** memory.
- Same PID twice on the same GPU: take **max** memory once (no double-count).
- **v1 scope:** use **compute-apps only** (CUDA/compute processes). Graphics-only processes that never appear in compute-apps are out of scope for v1; document that limitation in README.

**Sample — util (best-effort):**

```text
nvidia-smi pmon -c 1 -s u
```

(or equivalent one-shot sampling). Map PID → SM util %.

- Per device: one value per PID (max if multiple lines).
- Across devices: **sum** for multi-GPU (same rule as prior design).
- If pmon fails, is missing, or returns no util column: leave `util_pct=None` for PIDs that only have VRAM.
- Never invent util from device-wide averages.

**Subprocess rules:**

- Timeout on every invoke (e.g. 3 s).
- Capture stdout/stderr; on failure return empty map from `sample()` and set `last_error` (GUI already clears/shows `"—"`).
- Do not spawn a shell (`shell=False`); argument list only.
- Prefer locating `nvidia-smi` once at init; reuse path each sample.

**Performance:** Two short CLI calls per ~1 s tick is acceptable for v1. If too heavy later, throttle util query to every N ticks — out of scope unless measured pain.

### 2. `GpuSampler` changes

- Remove `PynvmlBackend` and all `import pynvml` usage.
- Default backend = `NvidiaSmiBackend`.
**Backend interface (required):**

```text
backend.init() -> None                         # raises on failure
backend.shutdown() -> None
backend.list_process_vram() -> list[tuple[int, int, str]]
    # (pid, vram_bytes, gpu_id)
backend.list_process_util() -> list[tuple[int, float, str]]
    # (pid, util_pct, gpu_id); empty list if util unavailable
```

`GpuSampler.sample()` is the **only** place that aggregates:

1. Group VRAM by `(gpu_id, pid)` → max bytes; then sum bytes across `gpu_id` per `pid`.
2. Group util by `(gpu_id, pid)` → max util; then sum util across `gpu_id` per `pid`.
3. Build `dict[pid, GpuProcessUsage]`; if VRAM known and util missing → `util_pct=None`.

FakeBackend for tests implements the same four methods (init/shutdown/list_process_vram/list_process_util). Update unit tests to this protocol; keep coverage for multi-GPU sum, util None, init fail, sample fail, within-gpu max.

### 3. GUI (`process_monitor_gui.py`)

**No behavioral change required** if `GpuSampler` API is preserved:

- Toolbar still `GPU: OK` / `GPU: unavailable`
- Columns still join by PID; `"—"` rules unchanged

Optional: tooltip text may say `nvidia-smi` instead of `NVML` when available.

### 4. Dependencies & docs

- `requirements.txt`: remove `pynvml`.
- README: document that GPU columns use **nvidia-smi** (must be on PATH); no Python GPU package.
- `scripts/check_pynvml.py`: replace with `scripts/check_gpu_sampler.py` that checks `nvidia-smi` + `GpuSampler`, or update the existing script’s name and body.

## Error handling

| Situation | Behavior |
|-----------|----------|
| `nvidia-smi` not on PATH | init fails; toolbar unavailable; cells `"—"` |
| Command timeout / non-zero exit | sample → `{}`; set `last_error`; GUI clears cells |
| VRAM row for PID, no util | `util_pct=None` → GPU % `"—"`, VRAM formatted |
| Process not using GPU | PID absent from map → both `"—"` |

## Testing

1. **Unit tests** with fake backend (no real nvidia-smi): multi-GPU VRAM sum, util max-then-sum, missing util, init/sample failure.
2. **Parser unit tests** for sample CSV / pmon fixtures (string fixtures, no hardware):
   - compute-apps CSV lines → pid + MiB
   - pmon lines → pid + sm util
3. **Manual:** machine with nvidia-smi — run GUI; toolbar OK; process under GPU load shows VRAM; util if pmon works.
4. **Manual:** hide/rename PATH entry for nvidia-smi — GUI still runs, GPU unavailable.

## Success criteria

1. App has **no required pynvml dependency**.
2. With nvidia-smi present, per-service **VRAM** appears for GPU-using PIDs that match health reports.
3. **GPU %** filled when pmon (or chosen util query) works; else `"—"`.
4. Without nvidia-smi, GUI does not crash; toolbar + em dash behavior preserved.
5. Existing ZMQ health/command path unchanged.

## Migration from 2026-08-11 design

| Was | Becomes |
|-----|---------|
| Stack: NVIDIA NVML via pynvml | Stack: nvidia-smi CLI |
| PynvmlBackend | NvidiaSmiBackend |
| requirements include pynvml | requirements: PyQt6, pyzmq only |
| scripts/check_pynvml.py | check script for nvidia-smi |

GUI columns, join-by-PID, multi-GPU sum, em dash, and failed-sample clear policy: **unchanged**.

## Open items resolved in brainstorm

- No pynvml install (user choice)
- nvidia-smi available
- VRAM always; util when possible
- nvidia-smi only (no optional pynvml)
- Approach A: backend behind same GpuSampler
