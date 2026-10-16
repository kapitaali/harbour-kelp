#!/bin/sh
#
# Build the relay and its harness and run the end-to-end check.
#
# Run inside the Sailfish build engine, where the target's Qt and OpenSSL live:
#
#   sfdk -c target=SailfishOS-5.1.0.11-i486 \
#       build-shell sh /path/to/harbour-kelp/relay/test/run-relay-test.sh
#
# It starts the relay on fixed ports, runs the harness (which plays both the
# publisher and the recipient), prints PASS/FAIL per case, and tears everything
# down. Exits non-zero if any case fails.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
RELAY="$(cd "$HERE/.." && pwd)"

PORT=9643

echo "building relay and harness"
mkdir -p "$RELAY/.build"
( cd "$RELAY/.build" && qmake "$RELAY/kelp-relay.pro" >/dev/null && make -s )
mkdir -p "$HERE/.build"
( cd "$HERE/.build" && qmake "$HERE/relaytest.pro" >/dev/null && make -s )

mkdir -p "$HERE/certs"
if [ ! -f "$HERE/certs/cert.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$HERE/certs/key.pem" -out "$HERE/certs/cert.pem" \
        -days 1 -subj "/CN=kelp-relay" >/dev/null 2>&1
fi

# Never `pkill -f relay`: that pattern matches this very command line.
pkill -x kelp-relay 2>/dev/null || true
sleep 1

# QT_LOGGING_TO_CONSOLE so the relay's qInfo/qWarning are actually visible here.
env QT_LOGGING_TO_CONSOLE=1 "$RELAY/.build/kelp-relay" \
    --port "$PORT" \
    --cert "$HERE/certs/cert.pem" --key "$HERE/certs/key.pem" \
    >"$HERE/.build/relay.log" 2>&1 &
RELAY_PID=$!

# Give it a moment to bind.
sleep 2

set +e
"$HERE/.build/relaytest" "$PORT"
STATUS=$?
set -e

kill "$RELAY_PID" 2>/dev/null || true
wait "$RELAY_PID" 2>/dev/null || true

exit $STATUS
