#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

runtime=$(mktemp -d /tmp/ntnative-wayland.XXXXXX)
export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=ntnative-test
export SDL_VIDEODRIVER=wayland
export SDL_FRAMEBUFFER_ACCELERATION=1
export SDL_VIDEO_WAYLAND_PREFER_LIBDECOR=0
export LIBGL_ALWAYS_SOFTWARE=1

cleanup() {
    status=$?
    kill "$compositor" 2>/dev/null || :
    wait "$compositor" 2>/dev/null || :
    if [ "$status" -ne 0 ]; then
        cat "$runtime/weston.log" >&2 || :
    fi
    rm -rf -- "$runtime"
}

weston --backend=headless-backend.so --use-pixman --no-config --idle-time=0 \
    --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" &
compositor=$!
trap cleanup EXIT
for step in 1 2 3 4 5 6 7 8 9 10; do
    if [ -S "$runtime/$WAYLAND_DISPLAY" ]; then
        "$@"
        exit 0
    fi
    kill -0 "$compositor"
    sleep 1
done
exit 1
