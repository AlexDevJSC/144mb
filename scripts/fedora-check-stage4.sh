#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Real Fedora stage-4 acceptance. Never disguise SKIP as PASS and never
# disable Bubblewrap/Landlock to make an incomplete host appear compatible.
set -Eeuo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ $(uname -m) != x86_64 ]]; then
    echo "FAIL: stage 4 requires Linux x86_64." >&2
    exit 2
fi
if ! grep -Eq '^ID=fedora$|^ID="fedora"$' /etc/os-release; then
    echo "FAIL: final acceptance requires a native Fedora installation." >&2
    exit 2
fi

for tool in make cc git python3 x86_64-w64-mingw32-gcc timeout bwrap; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "INCOMPLETE: missing build or sandbox dependency: $tool" >&2
        exit 3
    fi
done

# Preserve evidence: overwritten temporary logs previously prevented
# tracing QA outcomes to exact tested source revisions.
state_dir=${NT144_QA_OUTPUT_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/nt144-qa/stage4}
mkdir -p -- "$state_dir"
evidence=$(mktemp -d "$state_dir/run-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
log="$evidence/commands.log"
report="$evidence/classification.json"
metadata="$evidence/metadata.txt"
touch "$log"

{
    printf 'UTC=%s\n' "$(date -u +%FT%TZ)"
    printf 'REPOSITORY=%s\n' "$root"
    printf 'GIT_HEAD=%s\n' "$(git rev-parse HEAD)"
    printf 'GIT_STATUS=\n'
    git status --porcelain
    printf 'KERNEL='
    uname -a
    printf 'FEDORA='
    cat /etc/fedora-release
    printf 'SELINUX='
    if command -v getenforce >/dev/null 2>&1; then getenforce; else echo unknown; fi
    printf 'CPU_ALLOWED='
    grep '^Cpus_allowed_list:' /proc/self/status || :
    printf 'CPU_LOGICAL='
    getconf _NPROCESSORS_ONLN || :
    printf 'CC='
    cc --version | head -n 1
    printf 'MINGW='
    x86_64-w64-mingw32-gcc --version | head -n 1
    printf 'BWRAP='
    bwrap --version
    printf 'PACKAGES=\n'
    if command -v rpm >/dev/null 2>&1; then
        rpm -q openssl-devel fuse3 fuse3-devel mingw64-gcc fluidsynth-libs python3 2>&1 || :
    fi
} >"$metadata"

echo "NT144 QA evidence: $evidence"
cat "$metadata"

run_logged() {
    printf '\nCOMMAND:' | tee -a "$log"
    printf ' %q' "$@" | tee -a "$log"
    printf '\n' | tee -a "$log"
    if ! "$@" 2>&1 | tee -a "$log"; then
        printf 'FAIL: command did not complete: %s\n' "$1" | tee -a "$log" >&2
        python3 scripts/qa_classify.py "$log" --json-out "$report" || :
        echo "Evidence: $evidence" >&2
        exit 1
    fi
}

echo "[1/4] Linux runtime and real pthread/handle lifetime"
run_logged make -j2 check

# Positive scheduler tests require capability to restore *exactly* the
# original nice of this disposable process. Never elevate the runtime itself
# or touch any other host PID/TID. The operator must explicitly opt in.
if [[ ${NT144_QA_POSITIVE_NICE:-0} == 1 ]]; then
    if command -v sudo >/dev/null 2>&1 &&
       command -v prlimit >/dev/null 2>&1 &&
       sudo -n true >/dev/null 2>&1; then
        echo "[stage4] Positive reversible nice transactions (disposable test processes)"
        run_logged sudo -n prlimit --nice=40:40 -- env \
            NT_REQUIRE_POSITIVE_MULTITHREAD_PRIORITY=1 \
            ./build/process-priority-multithread-test
        run_logged sudo -n prlimit --nice=40:40 -- env \
            NT_NATIVE_EXPERIMENTAL_BOOST=1 NT_REQUIRE_POSITIVE_BOOST=1 \
            ./build/dynamic-priority-boost-test
    else
        echo "SKIP positive scheduler acceptance: explicit opt-in cannot acquire reversible nice privilege" | tee -a "$log"
    fi
else
    echo "SKIP positive scheduler acceptance: set NT144_QA_POSITIVE_NICE=1 for scoped, reversible privileged tests" | tee -a "$log"
fi

echo "[2/4] Native Windows PE32+ AMD64 APC/TLS/process plus lstrcmpW IAT"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 run_logged make -j2 \
  check-pe-lstrcmp check-pe-core-objects check-pe-apc check-pe-suspended-thread \
  check-pe-thread-groups check-pe-priority-class check-pe-priority-boost \
  check-pe-process-affinity check-pe-native-core check-pe-teb-tls check-pe-nt-io

echo "[3/4] UBSan: process/thread and guest PE boundaries"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 run_logged make -j2 \
  BUILD_DIR=build-stage4-ubsan \
  CFLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
  check check-pe-lstrcmp check-pe-core-objects check-pe-apc check-pe-suspended-thread \
  check-pe-thread-groups check-pe-priority-class check-pe-priority-boost \
  check-pe-process-affinity check-pe-native-core check-pe-teb-tls

echo "[4/4] Real Bubblewrap child and commandline, no physical acceptance SKIP"
SDL_VIDEODRIVER=offscreen SDL_FRAMEBUFFER_ACCELERATION=0 \
  run_logged make check-pe-isolated

python3 scripts/qa_classify.py "$log" --json-out "$report" \
  --require 'PASS PE child launched inside the same Linux sandbox and returned its full exit code' \
  --require 'PASS PE launcher preserves Windows arguments, escaping and Unicode' \
  --resolved-by 'SKIP positive boost::PASS dynamic priority: real Linux nice wake boost' \
  --resolved-by 'SKIP positive multithread priority transition::PASS privileged multithread priority transition'
echo "PASS: stage4 tests completed without mandatory SKIP."
echo "Evidence: $evidence"
echo "LIMITATION: this is not proof of universal Windows NT compatibility."
