#!/bin/sh
#
# The gateway's JavaScript core, checked against bundles sealed by libkelp.
#
# Two halves, because they run in two places: the fixture is minted by C++ inside
# the Sailfish build engine (real seals, with libkelp's own answers as the
# expected values), and the test is JavaScript on this host. So this script runs
# *on the host*, and uses `sfdk` for the first half and `node` for the second:
#
#   sh relay/gateway/test/run-gateway-test.sh [target]
#
# The default target is the emulator's. The fixture is target-independent content
# crypto, so either target's build answers the same; i486 is the faster build.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-SailfishOS-5.1.0.11-i486}"
FIXTURE="$HERE/.build/fixture.json"

echo "sealing the fixture inside $TARGET"
sfdk -c "target=$TARGET" build-shell sh "$HERE/makefixture-build.sh"

echo
echo "checking the JavaScript core against it (node $(node --version))"
node "$HERE/core.test.cjs" "$FIXTURE"
