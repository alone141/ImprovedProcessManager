#!/usr/bin/env python3
"""Diagnostic: nvidia-smi on PATH + GpuSampler."""
import shutil
import sys
from pathlib import Path

# Project root is parent of scripts/
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


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
