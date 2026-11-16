#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fedora 45 NT144 QA preflight. Never silently install RPMs or elevate the
# runtime. Optional --install requires deliberate local user approval.
set -Eeuo pipefail
if [[ ${1:-} == --install ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "INCOMPLETE: cannot install dependencies without sudo." >&2
        exit 3
    fi
    echo "Installing QA build prerequisites and optional FluidSynth runtime..."
    sudo dnf install -y openssl-devel fuse3 fuse3-devel mingw64-gcc \
        fluidsynth-libs bubblewrap python3 pkgconf-pkg-config
    shift
fi
if [[ $# -ne 0 ]]; then
    echo "Usage: $0 [--install]" >&2
    exit 2
fi
missing=0
check() {
    if "$@" >/dev/null 2>&1; then
        echo "PASS preflight: $*"
    else
        echo "INCOMPLETE preflight: $*" >&2
        missing=1
    fi
}
for executable in cc make git python3 pkg-config x86_64-w64-mingw32-gcc timeout bwrap; do
    check command -v "$executable"
done
if command -v pkg-config >/dev/null 2>&1; then
    check pkg-config --exists libcrypto
    check pkg-config --exists fuse3
fi
if command -v python3 >/dev/null 2>&1; then
    # This checks the SONAME used by src/runtime/fluid.c. The installed
    # package name alone is not proof that dlopen() will succeed.
    check python3 -c 'import ctypes; ctypes.CDLL("libfluidsynth.so.3")'
fi
if command -v bwrap >/dev/null 2>&1; then
    # Never advise bypassing userns/sandbox policy to make QA green.
    check bwrap --unshare-all --ro-bind / / -- /bin/true
fi
if [[ -c /dev/fuse ]]; then
    echo "PASS preflight: /dev/fuse character device exists (mount still unverified)"
else
    echo "INCOMPLETE preflight: /dev/fuse not available for physical FUSE acceptance" >&2
    missing=1
fi
if ! command -v fusermount3 >/dev/null 2>&1; then
    echo "INCOMPLETE preflight: fusermount3 absent" >&2
    missing=1
fi
if [[ $missing -ne 0 ]]; then
    echo "Install Fedora prerequisites explicitly with: bash scripts/fedora-qa-preflight.sh --install" >&2
    echo "Do not label unexecuted tests PASS." >&2
    exit 3
fi
echo "PASS Fedora QA host dependencies; no claim of physical >64-CPU validation"
