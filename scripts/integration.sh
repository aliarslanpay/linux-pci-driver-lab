#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
log="$root/build/guest.log"
if ! "$root/scripts/run-guest.sh" > "$log" 2>&1; then
    tail -n 60 "$log"
    echo 'Guest launch/timeout failed' >&2
    exit 1
fi
if ! grep -q '^EDU_LAB_INTEGRATION_PASS' "$log" ||
   grep -Eq 'EDU_LAB_FAIL|BUG:|WARNING:|Oops:|Kernel panic|EDU: clamping|out of bounds' "$log"; then
    tail -n 80 "$log"
    echo 'Guest integration failed' >&2
    exit 1
fi
grep -E '^(HOST_TEST|SELF_TEST|STRESS|CAPS|TIMEOUT|LIFECYCLE|PROBE_FAILURES|RELOAD|KERNEL_DIAGNOSTICS|EDU_LAB_INTEGRATION)' "$log"
