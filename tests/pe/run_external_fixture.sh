#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
runner="${1:-build/pe-isolated-run}"
fixture="${2:-build/win32-desktop.exe}"
# A caller-supplied environment flag cannot activate the internal worker.
status=0
NT_NATIVE_SANDBOXED=1 "$runner" --worker /game/win32-desktop.exe || status=$?
if [ "$status" -ne 125 ]; then
  echo "FAIL direct worker invocation was not rejected" >&2
  exit 1
fi
if [ ! -x /usr/bin/bwrap ]; then
  echo "SKIP external-PE sandbox test: bubblewrap is not installed"
  exit 0
fi
# Host policy may disable unprivileged user namespaces; don't bypass sandbox.
if ! bwrap --unshare-all --ro-bind / / -- /bin/true >/dev/null 2>&1; then
  echo "SKIP external-PE sandbox test: Bubblewrap could not prepare isolation"
  exit 0
fi
echo "Running external PE fixture with Linux bubblewrap and SDL offscreen"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  timeout 30s "$runner" "$fixture"
