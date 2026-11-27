#!/usr/bin/env bash
# Soak test: runs an engine binary for a long time and fails on what a short
# smoke run cannot see (issue #39, "nightly soak").
#
#   script/soak.sh <binary-dir> <binary> <seconds> [max-rss-growth-mb]
#
# Fails when the process exits before <seconds>, when it logs a validation
# error, a sanitizer report or an assert, when it never enabled validation (a
# run without layers proves nothing, see AGENTS.md), or when its resident
# memory grows by more than [max-rss-growth-mb] (default 256) between the end
# of the first minute and the end of the run: a frame that leaks a little is
# invisible in ten seconds and fatal in an hour.
#
# Needs a display (DISPLAY or Xvfb) and a Vulkan ICD; the caller sets
# VK_ICD_FILENAMES, UHE_VALIDATION_FATAL and sanitizer options.
set -u

DIR=${1:?binary dir}
BIN=${2:?binary name}
SECONDS_TO_RUN=${3:?seconds}
MAX_GROWTH_MB=${4:-256}
WARMUP=${SOAK_WARMUP:-60}

cd "$DIR" || { echo "soak: no directory $DIR"; exit 1; }
LOG="$PWD/soak-$BIN.log"
RSS="$PWD/soak-$BIN.rss"
: >"$RSS"

stdbuf -oL -eL "./$BIN" >"$LOG" 2>&1 &
PID=$!
echo "soak: $BIN pid $PID for ${SECONDS_TO_RUN}s, log $LOG"

rss_mb() { awk '/VmRSS/ {print int($2 / 1024)}' "/proc/$PID/status" 2>/dev/null; }

alive() {
    local s
    s=$(ps -o stat= -p "$PID" 2>/dev/null)
    [ -n "$s" ] && [ "${s:0:1}" != "Z" ]
}

fail() {
    echo "soak: FAIL: $1"
    echo "---------------- $BIN output (last 80 lines) ----------------"
    tail -n 80 "$LOG"
    kill "$PID" 2>/dev/null || true
    exit 1
}

baseline=""
elapsed=0
while [ "$elapsed" -lt "$SECONDS_TO_RUN" ]; do
    sleep 10
    elapsed=$((elapsed + 10))
    if ! alive; then
        wait "$PID"
        fail "$BIN exited with $? after ${elapsed}s"
    fi
    now=$(rss_mb)
    echo "$elapsed $now" >>"$RSS"
    if [ -z "$baseline" ] && [ "$elapsed" -ge "$WARMUP" ]; then
        baseline=$now
        echo "soak: baseline RSS ${baseline} MB at ${elapsed}s"
    fi
    if grep -qE '\[error\].*Validation layer|UHE_VALIDATION_FATAL|ERROR: [A-Za-z]+Sanitizer|runtime error:|Assertion' "$LOG"; then
        fail "error in the log after ${elapsed}s: $(grep -m1 -E '\[error\].*Validation layer|UHE_VALIDATION_FATAL|ERROR: [A-Za-z]+Sanitizer|runtime error:|Assertion' "$LOG")"
    fi
done

grep -qF "[VALIDATION] enabled" "$LOG" || fail "validation layers were never enabled; the run proves nothing"

final=$(rss_mb)
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
if [ -n "$baseline" ] && [ -n "$final" ]; then
    growth=$((final - baseline))
    echo "soak: RSS ${baseline} MB -> ${final} MB (${growth} MB, limit ${MAX_GROWTH_MB})"
    [ "$growth" -le "$MAX_GROWTH_MB" ] || fail "resident memory grew ${growth} MB after warm-up"
fi
echo "soak: PASS: $BIN ran ${SECONDS_TO_RUN}s"
