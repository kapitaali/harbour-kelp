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
#
# Two of the cases are the *browser* side of the same port (design §5.2, C4b):
# the harness cannot reach those, because KelpTls::Client offers "kelp/1" only
# and a browser offers "http/1.1". So the harness holds its tunnel open while
# `openssl s_client -alpn http/1.1` asks the relay for the gateway page and for
# the token-gated objects underneath it, exactly as a browser would.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
RELAY="$(cd "$HERE/.." && pwd)"
WORK="$HERE/.build"

PORT=9643

# Out of source, and cleaned first, so that switching build targets
# (i486 <-> aarch64) cannot leave foreign object files behind — the same rule the
# C++ suite runners follow, and the reason a stale tree once failed to link here
# with "undefined reference to operator new(unsigned int)".
echo "building relay and harness"
mkdir -p "$RELAY/.build"
( cd "$RELAY/.build" && qmake "$RELAY/kelp-relay.pro" >/dev/null && make -s clean && make -s )
mkdir -p "$WORK"
( cd "$WORK" && qmake "$HERE/relaytest.pro" >/dev/null && make -s clean && make -s )

mkdir -p "$HERE/certs"
if [ ! -f "$HERE/certs/cert.pem" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$HERE/certs/key.pem" -out "$HERE/certs/cert.pem" \
        -days 1 -subj "/CN=kelp-relay" >/dev/null 2>&1
fi

# Never `pkill -f relay`: that pattern matches this very command line.
pkill -x kelp-relay 2>/dev/null || true
sleep 1

# QT_LOGGING_TO_CONSOLE so the relay's qInfo/qWarning are actually visible here —
# the traffic line included.
env QT_LOGGING_TO_CONSOLE=1 "$RELAY/.build/kelp-relay" \
    --port "$PORT" \
    --cert "$HERE/certs/cert.pem" --key "$HERE/certs/key.pem" \
    >"$WORK/relay.log" 2>&1 &
RELAY_PID=$!

# Give it a moment to bind.
sleep 2

ACCESS="$WORK/access.json"
rm -f "$ACCESS"

BROWSER_FAILURES=0

# One browser-shaped request, and one check of the answer. The relay never closes
# a connection it was asked to keep open, so every request here says HTTP/1.0
# (whose default is "close") and the answer ends the connection.
browser_raw() {
    # $1 the file the answer is read into
    timeout 6 openssl s_client -quiet -connect "127.0.0.1:$PORT" -alpn http/1.1 \
        -servername kelp-relay < "$WORK/request.txt" > "$1" 2>"$WORK/browser.err" || true
}

browser_get() {
    printf 'GET %s HTTP/1.0\r\nHost: relay\r\n\r\n' "$1" > "$WORK/request.txt"
    browser_raw "$WORK/browser.out"
}

# $1 a name, $2 the path, $3 the status a browser must see, $4 a byte string the
# answer must contain (empty for none).
browser_case() {
    browser_get "$2"
    ok=1
    head -1 "$WORK/browser.out" | grep -q "HTTP/1.1 $3 " || ok=0
    if [ -n "$4" ]; then
        grep -qF -- "$4" "$WORK/browser.out" || ok=0
    fi
    if [ "$ok" = 1 ]; then
        printf 'PASS browser: %s\n' "$1"
    else
        printf 'FAIL browser: %s\n' "$1"
        sed -n '1,8p' "$WORK/browser.out" | sed 's/^/       | /'
        BROWSER_FAILURES=$((BROWSER_FAILURES + 1))
    fi
}

# The harness, in the background: it runs its own cases, writes $ACCESS, and then
# holds its tunnel open for the browser-shaped cases below.
env QT_LOGGING_TO_CONSOLE=1 "$WORK/relaytest" "$PORT" \
    --access "$ACCESS" --hold-ms 120000 >"$WORK/relaytest.log" 2>&1 &
HARNESS_PID=$!

# Wait for the harness's own cases to finish (about ten seconds: it waits out a
# report interval as well).
waited=0
while [ ! -f "$ACCESS" ] && [ "$waited" -lt 40 ]; do
    sleep 1
    waited=$((waited + 1))
done

if [ ! -f "$ACCESS" ]; then
    printf 'FAIL browser: the harness never reached its hold (no %s)\n' "$ACCESS"
    BROWSER_FAILURES=$((BROWSER_FAILURES + 1))
else
    # The browser only needs the address it was given; the data cases need the
    # relay token the harness minted, which is what a link's #k= would derive.
    BUNDLE="$(sed -n 's/.*"bundle":"\([a-z2-7]*\)".*/\1/p' "$ACCESS")"
    TOKEN="$(sed -n 's/.*"token":"\([A-Za-z0-9_-]*\)".*/\1/p' "$ACCESS")"
    OBJECT="$(sed -n 's/.*"object":"\([a-z2-7]*\)".*/\1/p' "$ACCESS")"

    if [ -z "$BUNDLE" ] || [ -z "$TOKEN" ] || [ -z "$OBJECT" ]; then
        printf 'FAIL browser: cannot read %s\n' "$ACCESS"
        BROWSER_FAILURES=$((BROWSER_FAILURES + 1))
    else
        # The gateway page and its own files: public, no key, no bundle needed.
        browser_case "gateway redirects to the bundle's own prefix" \
            "/b/$BUNDLE" 301 "Location: /b/$BUNDLE/"
        browser_case "gateway serves the loader page" \
            "/b/$BUNDLE/" 200 "kelp/core.js"
        browser_case "gateway serves the core as JavaScript" \
            "/b/$BUNDLE/.kelp/core.js" 200 "KelpGateway"
        browser_case "gateway serves the worker with its scope" \
            "/b/$BUNDLE/.kelp/sw.js" 200 "Service-Worker-Allowed: /b/$BUNDLE/"
        browser_case "a malformed bundle id is not a bundle" \
            "/b/notabundle" 404 "no such bundle"
        browser_case "the relay never serves a bundle's own file" \
            "/b/$BUNDLE/index.html" 404 "only the gateway page"

        # The paths underneath, on the same connection shape: this is what the
        # loader asks for once it has derived the relay token from the link.
        browser_case "the loader's info path answers a browser" \
            "/v1/relay/$TOKEN/bundle/$BUNDLE/info" 200 '"objects":22'
        browser_case "the loader's object path answers a browser" \
            "/v1/relay/$TOKEN/bundle/$BUNDLE/object/$OBJECT" 200 "kelp relay test payload v1"
        browser_case "a wrong token is still refused" \
            "/v1/relay/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/bundle/$BUNDLE/info" 401 "bad token"

        # The negotiation itself, from the relay's own account of the connection.
        if grep -q 'ALPN "http/1.1"' "$WORK/relay.log"; then
            printf 'PASS browser: the relay negotiated ALPN http/1.1\n'
        else
            printf 'FAIL browser: the relay never negotiated ALPN http/1.1\n'
            BROWSER_FAILURES=$((BROWSER_FAILURES + 1))
        fi

        # A browser reuses one connection for both of a page's scripts. Two
        # HTTP/1.1 requests, the first asking to keep it open and the second
        # saying it is done: both must be answered, and the connection must end
        # after the second, which is why s_client exits by itself here.
        {
            printf 'GET /b/%s/ HTTP/1.1\r\nHost: relay\r\nConnection: keep-alive\r\n\r\n' "$BUNDLE"
            printf 'GET /b/%s/.kelp/core.js HTTP/1.1\r\nHost: relay\r\nConnection: close\r\n\r\n' "$BUNDLE"
        } > "$WORK/request.txt"
        browser_raw "$WORK/pipeline.out"
        answered="$(grep -c '^HTTP/1.1 200 ' "$WORK/pipeline.out" || true)"
        if [ "$answered" = 2 ] && grep -qF -- "KelpGateway" "$WORK/pipeline.out"; then
            printf 'PASS browser: two requests on one kept-alive connection\n'
        else
            printf 'FAIL browser: two requests on one kept-alive connection (answered %s)\n' \
                "$answered"
            sed -n '1,8p' "$WORK/pipeline.out" | sed 's/^/       | /'
            BROWSER_FAILURES=$((BROWSER_FAILURES + 1))
        fi
    fi

    rm -f "$ACCESS"    # the harness may finish now
fi

set +e
wait "$HARNESS_PID"
HARNESS_STATUS=$?
set -e

kill "$RELAY_PID" 2>/dev/null || true
wait "$RELAY_PID" 2>/dev/null || true

echo
echo "--- the harness's own cases ---"
cat "$WORK/relaytest.log"

echo
if [ "$HARNESS_STATUS" = 0 ] && [ "$BROWSER_FAILURES" = 0 ]; then
    echo "ALL PASS"
    exit 0
fi
echo "FAILURES"
exit 1
