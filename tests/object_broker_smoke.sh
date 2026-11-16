#!/bin/sh
# SPDX-License-Identifier: MIT
# Launch one real broker and independent exec clients, then verify that a
# configured but unavailable broker never falls back to local named objects.
set -eu
broker=$(realpath "$1")
client=$(realpath "$2")
root=$(mktemp -d /tmp/nt-object-broker.XXXXXX)
pid=""
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    rm -rf -- "$root"
}
trap cleanup EXIT HUP INT TERM
chmod 0700 "$root"
socket="$root/broker.sock"
export NT_NATIVE_BROKER_SOCKET="$socket"
"$broker" --socket "$socket" > "$root/broker.log" 2>&1 &
pid=$!
ready=0
i=0
while [ "$i" -lt 100 ]; do
    if grep -q '^READY nt-object-broker' "$root/broker.log"; then
        ready=1
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 0.05
    i=$((i+1))
done
if [ "$ready" -ne 1 ]; then
    cat "$root/broker.log" >&2
    echo "FAIL broker did not start" >&2
    exit 1
fi
timeout 30s "$client"
kill "$pid"
wait "$pid"
pid=""
timeout 10s "$client" --unavailable
test ! -e "$socket"
echo "PASS NT object broker: authentication, exec IPC, cleanup, fail-closed unavailable socket"
