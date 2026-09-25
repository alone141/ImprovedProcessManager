# Per-Service GPU Usage (GUI Local NVML Sampling)

**Date:** 2026-08-11  
**Status:** Approved for implementation planning  
**Scope:** ProcessManager GUI only — no ZMQ protocol or C++ health struct changes

## Problem

Operators can already see CPU and memory per managed service from health reports. They cannot see **per-service GPU utilization and VRAM**. GPU load is critical for vision/ML-style services, and the current table has no GPU columns.

## Goals

1. Show **GPU utilization %** and **VRAM used** for each service in the process table.
2. Source metrics **locally on the GUI host** via **NVIDIA NVML**, joined by **PID** from existing health reports.
3. Keep the GUI usable when NVML or a GPU is unavailable (no crash; clear status).
4. Leave the ZMQ wire protocol and `DetailedHealthReport` layout **unchanged**.

## Non-goals (v1)

- Broadcasting GPU from the process manager / extending binary health structs
- Non-NVIDIA vendors (AMD, Intel)
- Per-GPU breakdown UI, charts, or historical series
- Restart control button or unrelated GUI polish
- Changing mock publisher health payloads for fake GPU (mock has no real GPU PIDs)

## Decisions

| Topic | Decision |
|-------|----------|
| Data source | GUI samples host GPU; join by PID |
| Metrics | Utilization % + VRAM used (bytes, displayed human-readable) |
| Stack | NVIDIA NVML only (`pynvml` or equivalent thin binding) |
| Multi-GPU | Sum util and VRAM across all GPUs for the same PID |
| Missing data | Show `"—"` in cells; toolbar warning if NVML unavailable |
| Architecture | Dedicated `GpuSampler` + ~1s timer; not inside ZMQ receive path |

## Architecture

```
┌─────────────────────┐     health reports      ┌──────────────────────────┐
│  Process Manager    │ ──────────────────────▶ │  ProcessMonitorWindow    │
│  (unchanged)        │     (PID, CPU, …)       │  table + toolbar         │
└─────────────────────┘                         │         ▲                │
                                                │         │ join by PID    │
┌─────────────────────┐     sample() ~1s        │         │                │
│  GpuSampler         │ ──────────────────────▶ │  _gpu_by_pid cache       │
│  (NVML, no Qt/ZMQ)  │  pid → util, vram       └──────────────────────────┘
└─────────────────────┘
```

- **ZmqWorker** remains responsible only for SUB health and DEALER commands.
- **GpuSampler** is a pure Python module: init, sample, shutdown; optional availability.
- **ProcessMonitorWindow** owns a `QTimer` (~1s), calls `sample()`, stores the map, and uses it when rebuilding table rows.

## Components

### 1. `gpu_sampler.py` (new)

**Responsibility:** NVML access and PID aggregation only.

**Surface:**

- `GpuProcessUsage` — `util_pct: float | None`, `vram_bytes: int`
  - `util_pct` is `None` when VRAM is known but per-process utilization is not exposed by the driver.
  - `vram_bytes` is total used GPU memory for that PID (summed across GPUs).
- `GpuSampler.init() -> bool` — `nvmlInit`; return False if unavailable
- `GpuSampler.shutdown()` — `nvmlShutdown` if initialized
- `GpuSampler.available() -> bool`
- `GpuSampler.last_error() -> str | None`
- `GpuSampler.sample() -> dict[int, GpuProcessUsage]` — OS PID → aggregated usage

**Sampling rules:**

1. If not available, return `{}` and leave `last_error` set.
2. For each device index, query running processes (compute and graphics APIs where exposed).
3. For each process entry with a PID, add used GPU memory into that PID’s totals (multi-GPU sum). Merge PIDs that appear on both compute and graphics lists without double-counting VRAM on the same device when the same PID is listed twice with the same memory.
4. For utilization: use NVML per-process utilization APIs when available (e.g. process utilization samples). Sum util across GPUs for the same PID. If util cannot be obtained for a PID that has VRAM, set `util_pct=None` (UI shows `"—"` for GPU % and still shows VRAM). Never invent util from device-wide counters attributed to one process.
5. PIDs with no GPU activity are omitted (UI shows `"—"` for both columns).
6. Init/sample failures must not raise into the UI thread; set `last_error` and return `{}` (clear prior cache at the GUI layer after a failed sample).

**Dependency:** `pynvml` declared as a project dependency. Runtime: if import or init fails, sampler stays unavailable.

**Testability:** Accept an optional injectable backend (or module-level seam) so unit tests can fake device/process lists without real hardware.

### 2. GUI changes (`process_monitor_gui.py`)

**Table columns** (insert after CPU % or Memory — exact order: after `CPU %`):

| Column | Content |
|--------|---------|
| GPU % | `{util:.1f}` if `util_pct` is not None; otherwise `"—"` |
| VRAM | `format_bytes(vram_bytes)` if PID in map; otherwise `"—"` |

Column order in `COLUMNS`: after `CPU %`, before `Uptime`.

**Lifecycle:**

- On window start: construct `GpuSampler`, call `init()`, set toolbar GPU status.
- Reuse the existing 1s UI timer (or a dedicated 1s timer): call `sample()`, assign `_gpu_by_pid`, then call `_rebuild_table()` if `_current` is non-empty so GPU cells update even when health is quiet.
- Health-driven rebuilds also read `_gpu_by_pid` so GPU columns stay filled between sample ticks.
- On close: stop timers, `shutdown()` sampler.

**Join rules:**

- Use `r["pid"]` from the health dict.
- If `pid` is missing, zero, or not in `_gpu_by_pid` → both GPU columns `"—"`.
- If sampler not available → all GPU cells `"—"`.
- If PID in map with `util_pct is None` → GPU % is `"—"`, VRAM still formatted.

**Toolbar / status:**

- Available: short label `GPU: OK` next to connection status.
- Unavailable: `GPU: unavailable` with tooltip set to `last_error`.

**Stale data:** On failed `sample()`, set `_gpu_by_pid = {}` and rebuild so cells become `"—"`. Successful empty map means no processes currently using GPU (also `"—"`), which is correct.

### 3. Unchanged

- `health_structs.py` layouts and command messages
- ZMQ endpoints, identity `PMC`, frames `BPM` + `CommandMessage`
- `mock_publisher.py` health broadcast behavior (optional later: document that GPU columns stay `"—"` under mock unless real local GPU PIDs match)

## Data flow

1. Health report arrives → update `_current[name]`, recompute CPU %, rebuild table.
2. GPU timer fires → `_gpu_by_pid = sampler.sample()`.
3. For each table row, if `pid in _gpu_by_pid`, fill GPU % and VRAM; else `"—"`.
4. Health rebuilds must read the latest `_gpu_by_pid` so columns stay filled between GPU ticks.

## Error handling

| Situation | Behavior |
|-----------|----------|
| `pynvml` missing / `nvmlInit` fails | Toolbar: unavailable + reason; all GPU cells `"—"` |
| Sample fails mid-run | Empty/clear map; status bar log once; cells `"—"` |
| Process stopped / PID 0 | `"—"` |
| Process on GPU | Show summed util + VRAM |

## Testing

1. **Unit tests** for aggregation: multi-GPU sum for one PID, two PIDs, empty devices, unavailable init — using a fake NVML backend.
2. **Manual:** GUI with NVIDIA driver; process under GPU load matches PID column; without driver, no crash and toolbar warning.
3. **Regression:** ZMQ health parse and Start/Stop commands still work with mock publisher.

## Success criteria

1. Each running service whose PID uses GPU shows live GPU % and VRAM.
2. Services without GPU activity show `"—"`.
3. GUI remains usable when NVML is absent.
4. Wire protocol and command path unchanged.

## Implementation notes (for planning)

- Use `pynvml` for NVML. VRAM from running-process info; util from per-process utilization APIs when present (see sampling rules).
- Column resize: `ResizeToContents` for GPU % and VRAM (same pattern as other metric columns).
- Document dependency alongside existing ones: `pip install PyQt6 pyzmq pynvml`.
- Add a minimal `requirements.txt` if the repo still has none, listing those three packages.

## Open items resolved during brainstorm

- Source: local GUI sampling (B)
- Metrics: util + VRAM (B)
- Stack: NVML (A)
- Multi-GPU: sum (A)
- Missing: em dash + toolbar warning
- Architecture: dedicated sampler + timer (Approach 1)
