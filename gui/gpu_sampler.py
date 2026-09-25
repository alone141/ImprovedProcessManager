"""Local NVIDIA GPU sampling by PID. No Qt / no ZMQ."""

from __future__ import annotations

import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Protocol, Tuple

SUBPROCESS_TIMEOUT_SEC = 3.0


@dataclass
class GpuProcessUsage:
    util_pct: Optional[float]  # None if util not available for this PID
    vram_bytes: Optional[int]  # None when the driver reports [N/A] (Windows WDDM)


UNAVAILABLE_VALUES = ("[N/A]", "N/A", "[Not Supported]")


def parse_compute_apps_csv(text: str) -> List[Tuple[int, Optional[int], str]]:
    """
    Parse: nvidia-smi --query-compute-apps=pid,used_gpu_memory,gpu_uuid
                     --format=csv,noheader,nounits
    Memory is MiB → bytes, or None when the driver does not report it.
    """
    out: List[Tuple[int, Optional[int], str]] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 2:
            continue
        try:
            pid = int(parts[0])
            mib = None if parts[1] in UNAVAILABLE_VALUES else float(parts[1])
        except ValueError:
            continue
        if pid <= 0:
            continue
        gpu_id = parts[2] if len(parts) >= 3 else ""
        bytes_ = None if mib is None else int(mib * 1024 * 1024)
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


class GpuBackend(Protocol):
    def init(self) -> None: ...
    def shutdown(self) -> None: ...
    def list_process_vram(self) -> List[Tuple[int, int, str]]: ...
    def list_process_util(self) -> List[Tuple[int, float, str]]: ...


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
        kwargs = {}
        if sys.platform == "win32":
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            cp = subprocess.run(
                [self._exe, *args],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=SUBPROCESS_TIMEOUT_SEC,
                shell=False,
                check=False,
                **kwargs,
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


class GpuSampler:
    def __init__(self, backend: Optional[GpuBackend] = None) -> None:
        self._backend: Optional[GpuBackend] = backend
        self._available = False
        self._error: Optional[str] = None
        self._inited = False

    def init(self) -> bool:
        if self._backend is None:
            self._backend = NvidiaSmiBackend()
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
            vram_rows = self._backend.list_process_vram()
            util_rows = self._backend.list_process_util()

            # VRAM: max per (gpu_id, pid), then sum across gpu_id; None = not reported
            vram_max: Dict[Tuple[str, int], Optional[int]] = {}
            for pid, mem, gpu_id in vram_rows:
                if pid <= 0:
                    continue
                key = (gpu_id, pid)
                prev = vram_max.get(key)
                if key not in vram_max or (mem is not None and (prev is None or mem > prev)):
                    vram_max[key] = mem
            vram: Dict[int, Optional[int]] = {}
            for (_gpu_id, pid), mem in vram_max.items():
                if mem is None:
                    vram.setdefault(pid, None)
                else:
                    vram[pid] = (vram.get(pid) or 0) + mem

            # Util: max per (gpu_id, pid), then sum across gpu_id
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
            for (_gpu_id, pid), u in util_max.items():
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
