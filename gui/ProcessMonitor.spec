# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller build: the health monitor GUI, plus the mock publisher for
testing on a machine without Python.

    python -m PyInstaller --clean --noconfirm ProcessMonitor.spec

Output (one folder per program, copy the whole folder):
    dist/ProcessMonitor/ProcessMonitor[.exe]
    dist/MockPublisher/MockPublisher[.exe]

PM_ONEFILE=1 builds single-file executables instead. They unpack into a
temporary directory on every start, which needs an executable /tmp; the
folder build is the safer default on a hardened host.

PyInstaller does not cross-compile: build on the operating system family the
result will run on, and on Linux with a glibc no newer than the target's.
"""

import os

ONEFILE = os.environ.get("PM_ONEFILE", "").strip() not in ("", "0", "false", "no")

# Only QtCore / QtGui / QtWidgets are used. Naming the rest keeps the hooks
# from bundling Qt modules, plugins and translations nobody loads, and keeps
# large optional libraries out (pyzmq works without numpy).
EXCLUDES = [
    "PyQt6.QtBluetooth", "PyQt6.QtDBus", "PyQt6.QtDesigner", "PyQt6.QtHelp",
    "PyQt6.QtMultimedia", "PyQt6.QtMultimediaWidgets", "PyQt6.QtNetwork",
    "PyQt6.QtNfc", "PyQt6.QtOpenGL", "PyQt6.QtOpenGLWidgets", "PyQt6.QtPdf",
    "PyQt6.QtPdfWidgets", "PyQt6.QtPositioning", "PyQt6.QtPrintSupport",
    "PyQt6.QtQml", "PyQt6.QtQuick", "PyQt6.QtQuick3D", "PyQt6.QtQuickWidgets",
    "PyQt6.QtRemoteObjects", "PyQt6.QtSensors", "PyQt6.QtSerialPort",
    "PyQt6.QtSpatialAudio", "PyQt6.QtSql", "PyQt6.QtStateMachine", "PyQt6.QtSvg",
    "PyQt6.QtSvgWidgets", "PyQt6.QtTest", "PyQt6.QtTextToSpeech",
    "PyQt6.QtWebChannel", "PyQt6.QtWebSockets", "PyQt6.QtXml",
    "numpy", "pandas", "scipy", "matplotlib", "PIL", "tkinter", "unittest",
    "pydoc", "doctest", "IPython", "jinja2", "flask", "fastapi", "pytest",
]


def program(script, name, console, extra_excludes=()):
    analysis = Analysis(
        [script],
        pathex=[SPECPATH],
        binaries=[],
        datas=[],
        hiddenimports=[],
        hookspath=[],
        runtime_hooks=[],
        excludes=EXCLUDES + list(extra_excludes),
        noarchive=False,
    )
    pyz = PYZ(analysis.pure)
    if ONEFILE:
        return EXE(
            pyz,
            analysis.scripts,
            analysis.binaries,
            analysis.datas,
            [],
            name=name,
            console=console,
            debug=False,
            strip=False,
            upx=False,
        )
    exe = EXE(
        pyz,
        analysis.scripts,
        [],
        exclude_binaries=True,
        name=name,
        console=console,
        debug=False,
        strip=False,
        upx=False,
    )
    return COLLECT(
        exe,
        analysis.binaries,
        analysis.datas,
        strip=False,
        upx=False,
        name=name,
    )


# The GUI: no console window on Windows (no effect on Linux).
program("process_monitor_gui.py", "ProcessMonitor", console=False)
# The mock process manager: a console program, no Qt inside.
program("mock_publisher.py", "MockPublisher", console=True, extra_excludes=["PyQt6"])
