#!/bin/sh
#
# Build and run every Kelp test suite: libkelp, the app layer, and the relay
# harness. Run it inside the Sailfish build engine, where the target's Qt and
# OpenSSL live:
#
#   sfdk -c target=SailfishOS-5.1.0.11-i486 \
#       build-shell sh /path/to/harbour-kelp/src/tests/run-all-tests.sh
#
# The dev-extras bundle is a build-time switch (META-INF/BUILD.md), and two
# suites check a default on both sides of it, so run this twice — once as it is,
# once with KELP_DEV_EXTRAS=1 in front of it.
#
# One line per suite. A failing suite prints its own last 20 lines here, and the
# script exits non-zero if any suite failed. It does not stop at the first
# failure: a full picture in one run is worth more than stopping early.
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LOG="$(mktemp)"

fail=0

run() {
    name="$1"
    shift
    if "$@" >"$LOG" 2>&1; then
        printf '%-16s %s\n' "$name" "$(tail -1 "$LOG")"
    else
        fail=1
        printf '%-16s FAILED\n' "$name"
        tail -20 "$LOG" | sed 's/^/    /'
    fi
}

if [ -n "$KELP_DEV_EXTRAS" ] && [ "$KELP_DEV_EXTRAS" != 0 ]; then
    printf 'dev extras: on (KELP_DEV_EXTRAS=%s)\n' "$KELP_DEV_EXTRAS"
else
    echo "dev extras: off — release build"
fi

run libkelp sh "$ROOT/libkelp/tests/run-tests.sh"
for suite in kelpfetch kelpsettings kelpshares kelphandoff kelpqr kelpbundles kelppreview kelpclient; do
    run "tst_$suite" sh "$HERE/run-$suite-test.sh"
done
run relay sh "$ROOT/relay/test/run-relay-test.sh"

rm -f "$LOG"

if [ "$fail" = 0 ]; then
    echo "all suites passed"
else
    echo "FAILURES — see above"
fi
exit "$fail"
