#!/bin/sh
# Run the recompiler with a hard wall-clock bound.
#
# The runtime installs interrupt handlers that can leave SIGALRM undelivered, so
# `perl -e 'alarm ...'` and `timeout(1)` are not always enough: a hung guest can
# outlive them. This wrapper always kills with SIGKILL.
#
# Usage:
#   tools/nwii_run.sh <seconds> <log-file> <command> [args...]
#
# Exit codes:
#   0    the guest exited on its own and the runtime reported no error counters
#   3    NWII_STRICT=1 and the runtime discarded work (see [DIAG] lines)
#   124  the wall-clock bound was hit (the log still has [STAT]/[DIAG] output)
#
# Logs are scanned afterwards and any [DIAG] error-class line is echoed to
# stderr, so a scripted run cannot quietly produce a broken frame.

set -u

if [ $# -lt 3 ]; then
    echo "usage: $0 <seconds> <log-file> <command> [args...]" >&2
    exit 2
fi

secs=$1
log=$2
shift 2

"$@" >"$log" 2>&1 &
pid=$!

(
    sleep "$secs"
    kill -9 "$pid" 2>/dev/null
) &
killer=$!

wait "$pid"
rc=$?
kill -9 "$killer" 2>/dev/null

if grep -q "NWII_STRICT=1: exiting non-zero" "$log"; then
    rc=3
elif [ "$rc" -eq 137 ] || [ "$rc" -eq 9 ]; then
    rc=124
fi

grep -E "^\[DIAG\] (\*\*\*|[a-z].*x[0-9]+:)" "$log" | head -20 >&2
grep -E "^\[DIAG\] stat:" "$log" | tail -1 >&2

if [ "$rc" -eq 124 ]; then
    echo "[nwii_run] hit ${secs}s bound; log: $log" >&2
fi

exit "$rc"
