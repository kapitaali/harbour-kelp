#!/bin/sh
#
# Build the fixture writer and run it. This half runs *inside* the Sailfish build
# engine, where the target's Qt lives — see run-gateway-test.sh, which is the one
# to run (it needs both this and node).
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/.build"
OUT="$WORK/fixture.json"

mkdir -p "$WORK"
( cd "$WORK" && qmake "$HERE/makefixture.pro" >/dev/null && make -s clean && make -s )

"$WORK/makefixture" "$OUT"
