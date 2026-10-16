#!/bin/sh
#
# Build and run the kelpfetch unit test (the app layer's wire agreement).
#
# Run this inside the Sailfish build engine, where the target's Qt and OpenSSL
# live:
#
#   sfdk -c target=SailfishOS-5.1.0.11-i486 \
#       build-shell sh /path/to/harbour-kelp/src/tests/run-kelpfetch-test.sh
#
# The test binary prints every check that fails and exits non-zero if any do.
set -e

cd "$(dirname "$0")"

QMAKE="${QMAKE:-qmake}"

# Out of source, and cleaned, so that switching build targets (i486 <-> aarch64)
# cannot leave foreign object files behind.
mkdir -p .build
cd .build
$QMAKE -o Makefile ../tst_kelpfetch.pro
make -s clean
make -s
exec ./tst_kelpfetch
