# Standalone executable

**Date:** 2026-09-24
**Status:** Implemented

## Goal

Run the GUI on the air-gapped machine without installing Python or any
package there. The bundle carries its own interpreter, PyQt6 (Qt) and
pyzmq (libzmq); the operator unpacks one archive and runs one file.

## Decisions

| Topic | Choice |
|-------|--------|
| Tool | PyInstaller, driven by `ProcessMonitor.spec` (one spec, two programs: `ProcessMonitor` and `MockPublisher`) |
| Layout | Folder build by default (`dist/ProcessMonitor/`); `PM_ONEFILE=1` for single files, which need an executable temp directory |
| Excludes | Every PyQt6 module except QtCore / QtGui / QtWidgets, plus numpy and other large optional libraries, so hooks bundle nothing unused |
| Build hosts | `scripts/build_executable.sh` (Linux) and `scripts/build_executable.ps1` (Windows); no cross-compiling |
| Compatibility | Linux output runs where glibc ≥ the build machine's; the archive name records the version |
| Bare build machines | The Linux script bootstraps pip (`get-pip.py`) and a `virtualenv` without root, points pip at the system CA bundle behind a TLS proxy, and on WSL1 strips Qt's kernel ABI note so Qt loads there |
| Offline build machines | `WHEELS=<dir>` installs from a wheel cache made with `pip download` elsewhere |

## Verified

- Windows 10, Python 3.10, PyQt6 6.11: `dist/ProcessMonitor/ProcessMonitor.exe` (89 MB folder) starts against `MockPublisher.exe` and stays up; `MockPublisher.exe --help` works.
- Ubuntu 20.04 under WSL1, Python 3.8, PyQt6 6.7.1, glibc 2.31: `dist/ProcessMonitor-linux-x86_64-glibc2.31.tar.gz` (61 MB); both programs start offscreen and stay up.

## Target requirements

No Python. A desktop session and the usual X11/Wayland client libraries
(`libxcb`, `libxkbcommon`, `libGL`, fontconfig, freetype, `libxcb-cursor`
for Qt ≥ 6.5), which any host that runs the pip-installed GUI today has.
