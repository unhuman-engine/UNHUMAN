#!/usr/bin/env bash
#
# Runs the engine with Vulkan validation errors fatal, so a frame that is
# invalid fails here instead of looking fine.
#
#   script/check_validation.sh [seconds] [binary]
#
# Defaults: 10 seconds of the Sandbox. Exits 0 only when the engine rendered
# (its [SMOKE-READY] marker), survived, and reported no validation error; 1 when
# any of those is false, or when the build has no validation layers at all (a
# run that cannot report an error must not be read as a clean one); 2 when the
# binary is missing.
#
# This is the local form of the gate CI applies: CI runs the same thing through
# UHE_VALIDATION_FATAL=1 in .github/workflows/sanitizers.yml and
# test-renderer.yml. Run it before pushing, not after.
set -uo pipefail

RUN_SECONDS="${1:-10}"
BINARY="${2:-bin/Debug-linux-x64/Sandbox}"

if [ ! -x "$BINARY" ]; then
    echo "check_validation: no executable at '$BINARY'" >&2
    echo "  build first: ninja -C build Sandbox" >&2
    exit 2
fi

LOG="$(mktemp -t uhe-validation-XXXXXX.log)"
trap 'rm -f "$LOG"' EXIT

# The Sandbox logs [SMOKE-READY] once frame 1 has been submitted and presented;
# the editor has no such marker, so for it the evidence of a real run is that it
# stayed up for the whole window.
REQUIRE_READY=0
case "$(basename "$BINARY")" in
    Sandbox) REQUIRE_READY=1 ;;
esac

echo "check_validation: $BINARY for ${RUN_SECONDS}s, validation errors fatal"
# Line-buffered: the engine's stdout sink is otherwise fully buffered when piped.
UHE_VALIDATION_FATAL=1 stdbuf -oL -eL "$BINARY" >"$LOG" 2>&1 &
APP_PID=$!

alive() {
    local state
    state=$(ps -o stat= -p "$APP_PID" 2>/dev/null)
    [ -n "$state" ] && [ "${state:0:1}" != "Z" ]
}

# Wait for the engine's own readiness marker rather than a wall clock: a run
# that never rendered has nothing to say about validation.
READY=0
for _ in $(seq 1 "$RUN_SECONDS"); do
    if grep -qF "[SMOKE-READY]" "$LOG" 2>/dev/null; then
        READY=1
        break
    fi
    # An error stops the engine before the marker, so stop waiting for one.
    if grep -qE 'UHE_VALIDATION_FATAL|\[error\].*Validation layer' "$LOG" 2>/dev/null; then
        break
    fi
    alive || break
    sleep 1
done

# Let it keep running so a late error still lands in the log.
for _ in $(seq 1 3); do
    alive || break
    sleep 1
done

if alive; then
    kill "$APP_PID" 2>/dev/null || true
fi
wait "$APP_PID" 2>/dev/null
CODE=$?

if grep -qE '\[error\].*Validation layer|UHE_VALIDATION_FATAL' "$LOG"; then
    echo "check_validation: FAILED - validation reported an error"
    echo
    grep -m5 -E '\[error\].*Validation layer|UHE_VALIDATION_FATAL' "$LOG"
    exit 1
fi

# A run without layers reports no validation error either: it has nothing to
# report them with, and `enableValidationLayers` is off under NDEBUG. Passing on
# that would make this gate green on exactly the builds it cannot judge.
if ! grep -qF "[VALIDATION] enabled" "$LOG"; then
    echo "check_validation: FAILED - the run never enabled validation layers, so it proves nothing" >&2
    echo "  this build has them off (Release): point the script at a Debug build," >&2
    echo "  e.g. bin/Debug-linux-x64/Sandbox" >&2
    exit 1
fi

if [ "$REQUIRE_READY" -eq 1 ] && [ "$READY" -ne 1 ]; then
    echo "check_validation: FAILED - never reported [SMOKE-READY], so nothing was rendered" >&2
    tail -n 30 "$LOG" >&2
    exit 1
fi

# A non-zero code after a clean, rendered run means it died on its own: a
# segfault or an abort, which is a failure even with no validation message.
case "$CODE" in
    0|124|137|143)
        echo "check_validation: clean - rendered and survived ${RUN_SECONDS}s with no validation error"
        exit 0
        ;;
    *)
        echo "check_validation: FAILED - engine died with exit code $CODE after rendering" >&2
        tail -n 30 "$LOG" >&2
        exit 1
        ;;
esac
