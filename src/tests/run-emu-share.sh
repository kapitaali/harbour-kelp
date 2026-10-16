#!/bin/sh
#
# run-emu-share.sh — make the emulator's share reachable from the LAN.
#
# The Sailfish emulator runs behind QEMU's user-mode NAT: it can dial out, but
# nothing on the network can dial *in* to it. So a link the emulator mints names
# 10.0.2.15, which is useless to any other device. This bridges that gap for
# testing, without touching the app:
#
#   * reads the emulator's running KelpServer port from its log,
#   * opens an ssh -L forward from this host's LAN address into the guest, so
#     the host becomes the address that can actually be reached,
#   * prints each shared bundle as a link whose l= names this host, ready to be
#     pasted or scanned into a recipient's Kelp app.
#
# The emulator's own port is ephemeral, so run this again after the app
# restarts. The forward is left running in the background; the script prints how
# to stop it.
#
# Usage:
#   src/tests/run-emu-share.sh [--host ADDRESS] [--redact] [--qr DIR]
#
#   --host ADDRESS  an address recipients can reach THIS machine on. Defaults to
#                   the host's primary LAN IPv4. Use this when the phone is not
#                   on the same network as the default address.
#   --redact        print links with the #k= fragment removed, for logs or
#                   screenshots. The fragment is the decryption key.
#   --qr DIR        also write one PNG QR code per share into DIR. These contain
#                   the full link, key included, so they can be scanned straight
#                   into a phone — the easiest way to move a 140-character link
#                   without a clipboard bridge. Treat the files as secrets.
#
# The printed links carry each share's secret. Treat them like passwords: do not
# paste them into a bug report, a log, or a chat.
set -eu

SSH_PORT="${KELP_SSH_PORT:-2223}"
SSH_KEY="${KELP_SSH_KEY:-$HOME/SailfishOS/vmshare/ssh/private_keys/sdk}"
DATA_DIR="${KELP_EMU_DATAHOME:-/home/defaultuser/.local/share/harbour.kelp/harbour-kelp}"

HOST=""
REDACT=0
QR_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs an address}"; shift 2 ;;
        --redact) REDACT=1; shift ;;
        --qr) QR_DIR="${2:?--qr needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
     -p $SSH_PORT -i $SSH_KEY defaultuser@127.0.0.1"

if [ -z "$HOST" ]; then
    HOST="$(ip -4 route get 1.1.1.1 2>/dev/null \
            | sed -n 's/.* src \([0-9][0-9.]*\).*/\1/p' | head -1)"
    [ -n "$HOST" ] || { echo "cannot work out this host's address; pass --host" >&2; exit 1; }
fi

# The address the guest is serving on, e.g. 10.0.2.15:43071. Its host part is
# what the ssh forward must target (the guest binds its LAN interface, not
# loopback); its port is the one recipients will reach, on OUR address instead.
SERVING="$($SSH "grep -a 'KelpServer: serving on' '$DATA_DIR/kelp.log'" 2>/dev/null \
           | sed -n 's/.*serving on "\([^"]*\)".*/\1/p' | tail -1 || true)"
if [ -z "$SERVING" ]; then
    echo "no running share found in $DATA_DIR/kelp.log" >&2
    echo "start the app and share (or restore) a bundle first." >&2
    exit 1
fi

GUEST_HOST="${SERVING%:*}"
PORT="${SERVING##*:}"

# A share is one row: identity in bundleId/secret, a display name, and no
# address — the address is the ephemeral one above. So the link can be rebuilt
# exactly, just pointed at this host.
SHARES="$($SSH "cat '$DATA_DIR/shares.json'" 2>/dev/null || true)"
if [ -z "$SHARES" ] || [ "$SHARES" = "[]" ]; then
    echo "no shares in $DATA_DIR/shares.json" >&2
    exit 1
fi

# Reuse a forward if one is already up on this address:port; otherwise start it.
# KelpServer answers 401 to a request for a bundle it does not know, so any
# answer but 000 means the door is already open.
if [ "$(curl -s -m 2 -o /dev/null -w '%{http_code}' \
        "http://$HOST:$PORT/v1/bundle/x/info" 2>/dev/null || true)" = "000" ]; then
    echo "opening forward: $HOST:$PORT -> $GUEST_HOST:$PORT (guest)"
    # setsid + closed stdin: the forward must outlive this script, and must not
    # hold the pipe this script's output travels down.
    # shellcheck disable=SC2086
    setsid $SSH -g -N -o ExitOnForwardFailure=yes \
        -L "$HOST:$PORT:$GUEST_HOST:$PORT" </dev/null >/dev/null 2>&1 &
    FORWARD_PID="$!"
    sleep 2
    if ! kill -0 "$FORWARD_PID" 2>/dev/null; then
        echo "forward failed to start (is another one already bound to $HOST:$PORT?)" >&2
        exit 1
    fi
else
    echo "forward already open: $HOST:$PORT -> $GUEST_HOST:$PORT (guest)"
fi

echo
if [ -n "$QR_DIR" ]; then
    mkdir -p "$QR_DIR"
fi
echo "links (l= names $HOST; #k= is the key):"
printf '%s' "$SHARES" | python3 -c '
import json, os, sys, urllib.parse
host, port, redact, qr_dir = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]
for row in json.load(sys.stdin):
    name = row.get("name") or row.get("path") or ""
    query = "l=" + urllib.parse.quote(host + ":" + port)
    if name:
        query += "&n=" + urllib.parse.quote(name)
    bundle = row.get("bundleId", "")
    secret = row.get("secret", "")
    head = "kelp://%s?%s#k=" % (bundle, query)
    print(head + ("<redacted>" if redact else secret))
    if qr_dir:
        import qrcode
        path = os.path.join(qr_dir, bundle + ".png")
        qrcode.make(head + secret).save(path)   # the QR always carries the key
        print("  QR (contains the key): %s" % path)
' "$HOST" "$PORT" "$REDACT" "$QR_DIR"

echo

echo
if [ -n "${FORWARD_PID:-}" ]; then
    echo "forward running as pid $FORWARD_PID. Stop it with:"
    echo "  kill $FORWARD_PID"
fi
