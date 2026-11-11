#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
runner="${1:-build/pe-isolated-run}"
fixture="${2:-build/win32-desktop.exe}"
profile_fixture="${3:-build/profile-fixture.exe}"
folder_fixture="${4:-build/folder-fixture.exe}"
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
profile_state=$(mktemp -d)
trap 'rm -rf -- "$profile_state"' EXIT HUP INT TERM
expected=P
for attempt in 1 2; do
  XDG_STATE_HOME="$profile_state" SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
    timeout 30s "$runner" "$profile_fixture"
  saved="$profile_state/nt-native-linux/drive-c/Users/guest/AppData/Roaming/nt-native-profile-fixture.dat"
  test "$(cat "$saved")" = "$expected"
  expected="${expected}P"
done
echo "PASS PE preferences persist across two isolated processes"
native_folder="$profile_state/Native documents"
mkdir -- "$native_folder"
printf L > "$native_folder/shared.txt"
XDG_STATE_HOME="$profile_state" SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  timeout 30s "$runner" --share "Documents=$native_folder" "$folder_fixture"
test "$(cat "$native_folder/shared.txt")" = LW
echo "PASS PE known folder resolves to the original Linux file"
