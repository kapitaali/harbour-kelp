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
#   * prints the links the app itself minted, which name this host.
#
# The app seals the address it puts in a link (q=), so only the app can name this
# host in one. Tell it to, when you launch it in the guest:
#
#   KELP_ADVERTISE_HOST=<this host> KELP_DEBUG_LINK=1 /usr/bin/harbour-kelp
#
# then run this with the same address. The forward is left running in the
# background; the script prints how to stop it.
#
# The emulator's own port is ephemeral, so run this again after the app restarts.
#
# Usage:
#   src/tests/run-emu-share.sh [--host ADDRESS] [--redact] [--qr DIR] [--fetch]
#
#   --host ADDRESS  an address recipients can reach THIS machine on. Defaults to
#                   the host's primary LAN IPv4. It has to match the
#                   KELP_ADVERTISE_HOST the app was launched with.
#   --redact        print links with the #k= fragment removed, for logs or
#                   screenshots. The fragment is the decryption key.
#   --qr DIR        also write one PNG QR code per share into DIR. These contain
#                   the full link, key included, so they can be scanned straight
#                   into a phone. Treat the files as secrets.
#   --fetch         go on to fetch from the share through the forward, the way a
#                   recipient would (kelp-probe.py): authenticated /info and root
#                   object, then two requests that must be refused. Prints status
#                   codes only, then the line the publisher's own log writes for
#                   that traffic. Use it to check a share is really reachable
#                   before handing the link to a phone.
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
FETCH=0
while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs an address}"; shift 2 ;;
        --redact) REDACT=1; shift ;;
        --qr) QR_DIR="${2:?--qr needs a directory}"; shift 2 ;;
        --fetch) FETCH=1; shift ;;
        -h|--help) sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

LOG="$($SSH "cat '$DATA_DIR/kelp.log'" 2>/dev/null || true)"

# The address the app put in its links, e.g. 10.0.2.15:43071 — or this host's
# address, when it was launched with KELP_ADVERTISE_HOST. Its host part must be
# this host for the forward to be the address that works; its port is the one
# recipients will reach, on OUR address instead.
SERVING="$(printf '%s\n' "$LOG" | grep -a 'KelpServer: serving on' \
           | sed -n 's/.*serving on "\([^"]*\)".*/\1/p' | tail -1)"
if [ -z "$SERVING" ]; then
    echo "no running share found in $DATA_DIR/kelp.log" >&2
    echo "start the app and share (or restore) a bundle first." >&2
    exit 1
fi

# What the app actually bound, when that differs from what it advertises. That is
# the address the forward must target: the guest binds its LAN interface, not
# loopback, and advertising does not move the socket.
BOUND="$(printf '%s\n' "$LOG" | grep -a 'KelpServer: bound to' \
         | sed -n 's/.*bound to "\([^"]*\)".*/\1/p' | tail -1)"

SERVING_HOST="${SERVING%:*}"
PORT="${SERVING##*:}"
GUEST_HOST="${BOUND%:*}"
[ -n "$GUEST_HOST" ] || GUEST_HOST="$SERVING_HOST"

if [ "$SERVING_HOST" != "$HOST" ]; then
    echo "the app is advertising $SERVING_HOST, so its links point there, not at $HOST." >&2
    echo "restart it in the guest with KELP_ADVERTISE_HOST=$HOST KELP_DEBUG_LINK=1," >&2
    echo "then run this again with --host $HOST." >&2
    exit 1
fi

# The shares the app still holds, so a link left in the log by an earlier run is
# not shown for a bundle that has since been removed.
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
echo "links (sealed by the app; #k= is the key):"
printf '%s' "$LOG" | python3 -c '
import json, os, re, sys
shares_raw, redact, qr_dir = sys.argv[1], sys.argv[2] == "1", sys.argv[3]
log = sys.stdin.read()

links = {}
for found in re.finditer(r"kelp://[^\"\s]+", log):
    link = found.group(0)
    bundle = link[len("kelp://"):].split("?")[0].split("#")[0]
    links[bundle] = link                 # the last link the app minted wins

for row in json.loads(shares_raw):
    bundle = row.get("bundleId", "")
    name = row.get("name") or row.get("path") or bundle
    link = links.get(bundle)
    if not link:
        print("%s\n  no link in the log — share it with links logging on" % name)
        continue
    shown = re.sub(r"#k=.*$", "#k=<redacted>", link) if redact else link
    print("%s\n  %s" % (name, shown))
    if qr_dir:
        import qrcode
        path = os.path.join(qr_dir, bundle + ".png")
        qrcode.make(link).save(path)     # the QR always carries the key
        print("  QR (contains the key): %s" % path)
' "$SHARES" "$REDACT" "$QR_DIR"

if [ "$FETCH" = "1" ]; then
    echo
    echo "fetching through the forward, the way a recipient would. The link is"
    echo "parsed here and never printed:"
    if ! printf '%s' "$LOG" | python3 "$(dirname "$0")/kelp-probe.py" \
            --base "http://$HOST:$PORT"; then
        echo "the share did not answer as it should (see above)" >&2
    fi

    echo
    echo "what the publisher logged for that traffic — one line per peer, written"
    echo "once the peer has been quiet for five seconds, so within about ten:"
    sleep 12
    $SSH "grep -a 'KelpServer' '$DATA_DIR/kelp.log' \
          | grep -av -e 'serving on' -e 'bound to' | tail -1"
fi

echo
if [ -n "${FORWARD_PID:-}" ]; then
    echo "forward running as pid $FORWARD_PID. Stop it with:"
    echo "  kill $FORWARD_PID"
fi
