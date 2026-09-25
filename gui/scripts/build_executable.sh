#!/usr/bin/env bash
# Build standalone Linux executables (no Python needed on the target).
#
#   scripts/build_executable.sh                 # folder build (default)
#   PM_ONEFILE=1 scripts/build_executable.sh    # single-file executables
#
# Run this on a Linux machine with network access (or a wheel cache, see
# WHEELS below) whose glibc is no newer than the target's: PyInstaller
# bundles this machine's Python and Qt, and the result runs on any x86_64
# Linux with an equal or newer glibc and a desktop session. The safest
# choice is the same distribution and release as the air-gapped host.
#
# The build machine needs python3 (3.8 or newer), curl and network. pip and
# venv are bootstrapped without root when the distribution ships python3
# without them (Debian/Ubuntu without python3-pip / python3-venv).
#
# Offline build machine: on a networked machine run, with the build machine's
# own Python version (python3 --version there; Ubuntu 20.04 has 3.8):
#   pip download -r requirements.txt pyinstaller -d wheels --only-binary=:all: \
#       --python-version 3.8 --platform manylinux2014_x86_64 \
#       --platform manylinux_2_28_x86_64
# copy the wheels/ folder over and set WHEELS to its path (relative paths are
# taken from where the script is started); that machine then needs a working
# pip and venv of its own.
set -euo pipefail

# Resolve WHEELS before the cd below changes what a relative path means.
if [ -n "${WHEELS:-}" ]; then
    WHEELS="$(cd "$WHEELS" && pwd)"
fi
cd "$(dirname "$0")/.."
VENV=".build-venv"
PY="${PYTHON:-python3}"

# Behind a TLS-inspecting proxy the company root certificate sits in the
# system store, which curl uses, but not in pip's bundled certificates.
# Pointing pip at the system bundle makes both behave the same.
if [ -z "${PIP_CERT:-}" ]; then
    for bundle in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt \
                  /etc/ssl/ca-bundle.pem /etc/ssl/cert.pem; do
        if [ -f "$bundle" ]; then
            export PIP_CERT="$bundle"
            break
        fi
    done
fi

have_pip() { "$1" -m pip --version >/dev/null 2>&1; }

bootstrap_pip() {
    # get-pip.py installs pip for the user without apt or root.
    local ver url
    ver="$("$PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
    url="https://bootstrap.pypa.io/pip/${ver}/get-pip.py"
    echo "python3 has no pip: bootstrapping it from ${url}"
    if ! curl -fsSL "$url" -o /tmp/get-pip.py; then
        curl -fsSL https://bootstrap.pypa.io/get-pip.py -o /tmp/get-pip.py
    fi
    "$PY" /tmp/get-pip.py --user --quiet
}

make_venv() {
    if "$PY" -m venv "$VENV" 2>/dev/null && [ -x "$VENV/bin/pip" ]; then
        return
    fi
    # venv without ensurepip (Debian/Ubuntu): use virtualenv, which brings pip.
    rm -rf "$VENV"
    have_pip "$PY" || bootstrap_pip
    "$PY" -m pip install --user --quiet virtualenv
    "$PY" -m virtualenv --quiet "$VENV"
}

[ -x "$VENV/bin/python" ] || make_venv
# shellcheck disable=SC1091
source "$VENV/bin/activate"

if [ -n "${WHEELS:-}" ]; then
    pip install --no-index --find-links "$WHEELS" -r requirements.txt pyinstaller
else
    pip install --quiet --upgrade pip
    pip install --quiet -r requirements.txt pyinstaller
fi

# WSL1 reports kernel 4.4 while Qt 6's libQt6Core carries a "needs Linux 4.11"
# ABI note, so the loader refuses it there. Dropping the note lets the build
# (and its output) run on WSL1; it changes nothing on a real Linux kernel.
if ! python -c 'import PyQt6.QtCore' >/dev/null 2>&1 && grep -qi microsoft /proc/version 2>/dev/null; then
    core="$(python -c 'import PyQt6, os; print(os.path.join(os.path.dirname(PyQt6.__file__), "Qt6", "lib", "libQt6Core.so.6"))')"
    if [ -f "$core" ] && command -v strip >/dev/null; then
        echo "WSL1: removing the kernel ABI note from $(basename "$core") so Qt loads here"
        strip --remove-section=.note.ABI-tag "$core"
    fi
fi
python -c 'import PyQt6.QtCore as c, zmq, PyInstaller; print("PyQt6", c.PYQT_VERSION_STR, "Qt", c.QT_VERSION_STR, "| pyzmq", zmq.__version__, "| PyInstaller", PyInstaller.__version__)'

rm -rf build dist
python -m PyInstaller --clean --noconfirm ProcessMonitor.spec

ARCH="$(uname -m)"
# awk reads all of ldd's output, so ldd never dies of SIGPIPE under pipefail.
GLIBC="$(ldd --version 2>/dev/null | awk 'NR == 1 { print $NF }')"
GLIBC="${GLIBC:-unknown}"
if [ -n "${PM_ONEFILE:-}" ] && [ "${PM_ONEFILE}" != "0" ]; then
    OUT="dist/ProcessMonitor-linux-${ARCH}-glibc${GLIBC}-onefile.tar.gz"
else
    OUT="dist/ProcessMonitor-linux-${ARCH}-glibc${GLIBC}.tar.gz"
fi
tar -C dist -czf "$OUT" ProcessMonitor MockPublisher

echo
echo "Built with glibc ${GLIBC} on ${ARCH}; the target needs glibc >= ${GLIBC}."
echo "Archive: $OUT"
echo "On the target:  tar xzf $(basename "$OUT")  &&  ./ProcessMonitor/ProcessMonitor --sub tcp://HOST:6667 --dealer tcp://HOST:5557"
