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
# host in one. Either tell it to when you launch it in the guest:
#
#   KELP_ADVERTISE_HOST=<this host> KELP_DEBUG_LINK=1 /usr/bin/harbour-kelp
#
# — or let this script do it with --relaunch, which is the same thing without
# the shell dance: it stops whatever app is running in the guest, starts it
# again advertising this host, and only reads the log written from that point on
# (so a link from an earlier run can never be mistaken for a current one). Use
# --relaunch after the app has been started normally (from the emulator's own
# launcher, say), and every time it is restarted by hand: the guest's port is
# ephemeral, and a link is only reachable while an app advertising this host is
# serving it.
#
# The forward is left running in the background; the script prints how to stop it.
#
# Usage:
#   src/tests/run-emu-share.sh [--host ADDRESS] [--relaunch] [--redact]
#                              [--qr DIR] [--fetch]
#
#   --host ADDRESS  an address recipients can reach THIS machine on. Defaults to
#                   the Wi-Fi LAN's address when the host has one — a phone is
#                   usually there, often on its own hotspot — else to the
#                   default route's. It prints which one it picked when the host
#                   has more than one; --host overrides. It has to match the
#                   KELP_ADVERTISE_HOST the app was launched with.
#   --relaunch      stop the app in the guest and start it again with this host
#                   as the address it advertises, then carry on as below. Also
#                   clears any forward left over from a previous run, whose port
#                   the restart has just invalidated.
#   --redact        print links with the key fragment removed, for logs or
#                   screenshots. That is `#k=` (the decryption key itself) and
#                   also `#p=` (the key wrapped under a passphrase) — the second
#                   is half a capability and must not be pasted around either.
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
RELAUNCH=0
while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST="${2:?--host needs an address}"; shift 2 ;;
        --relaunch) RELAUNCH=1; shift ;;
        --redact) REDACT=1; shift ;;
        --qr) QR_DIR="${2:?--qr needs a directory}"; shift 2 ;;
        --fetch) FETCH=1; shift ;;
        -h|--help) sed -n '2,55p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
     -p $SSH_PORT -i $SSH_KEY defaultuser@127.0.0.1"

if [ -z "$HOST" ]; then
    DEFAULT="$(ip -4 route get 1.1.1.1 2>/dev/null \
            | sed -n 's/.* src \([0-9][0-9.]*\).*/\1/p' | head -1)"

    # The default route is the wrong guess on a machine that also carries a
    # phone's network: a recipient phone is almost always on Wi-Fi — often its
    # own hotspot — while the default route usually leaves by wire, so a link
    # that names the wired address is dead for the phone while the forward is
    # perfectly healthy. Prefer the Wi-Fi LAN when there is one; a host whose
    # default route *is* its Wi-Fi gets the same answer either way.
    WIFI=""
    for marker in /sys/class/net/*/wireless; do
        [ -e "$marker" ] || continue
        NAME="$(basename "$(dirname "$marker")")"
        ADDR="$(ip -4 -o addr show dev "$NAME" 2>/dev/null \
                | sed -n 's/.* inet \([0-9][0-9.]*\)\/.*/\1/p' | head -1)"
        if [ -n "$ADDR" ]; then
            WIFI="$ADDR"
            break
        fi
    done
    HOST="${WIFI:-$DEFAULT}"
    [ -n "$HOST" ] || { echo "cannot work out this host's address; pass --host" >&2; exit 1; }

    if [ -n "$WIFI" ] && [ "$WIFI" != "$DEFAULT" ]; then
        echo "note: advertising $WIFI, the Wi-Fi LAN — a phone is usually there;" >&2
        echo "      the default route is $DEFAULT." >&2
    fi

    # Whatever was picked is still only a guess about where the recipient is.
    # A recipient can be on another of this host's networks, and a forward
    # bound to the wrong one answers nothing — so name the rest rather than
    # leave the link looking broken. Bridges and the docker/vbox nets are not
    # candidates for a recipient.
    OTHERS="$(ip -4 -o addr show up 2>/dev/null \
              | awk -v h="$HOST" '$2 !~ /^(docker|br-|vbox|virbr|lo)/ {
                    a = $4; sub(/\/.*/, "", a); if (a != h) print a }' \
              | tr '\n' ' ')"
    if [ -n "$OTHERS" ]; then
        echo "note: this host also answers on ${OTHERS% }" >&2
        echo "      advertising $HOST; if the recipient is on one of the others," >&2
        echo "      pass --host <address>." >&2
    fi
fi

# --relaunch: the two halves of the recipe above, done here. The app is stopped
# and started again advertising this host, and the log is read from that point on
# only — an earlier run's `serving on` line names a port that has just died, and
# reading it would hand out a link to nothing.
FRESH_FROM=""
if [ "$RELAUNCH" = "1" ]; then
    FRESH_FROM="$($SSH "wc -l < '$DATA_DIR/kelp.log'" 2>/dev/null | tr -d ' ' || true)"
    case "$FRESH_FROM" in
        ''|*[!0-9]*) FRESH_FROM=0 ;;
    esac

    # A forward from an earlier run holds a port the restart has thrown away; it
    # can only keep a listening socket nobody will use. Matched on this host's
    # address so no other forward is touched.
    pkill -f "ssh .*-L $HOST:" 2>/dev/null || true

    echo "stopping the app in the guest"
    $SSH 'killall harbour-kelp 2>/dev/null' >/dev/null 2>&1 || true
    i=0
    while [ "$i" -lt 10 ]; do
        if [ -z "$($SSH 'ps -o comm | grep harbour-kelp' 2>/dev/null || true)" ]; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done
    if [ -n "$($SSH 'ps -o comm | grep harbour-kelp' 2>/dev/null || true)" ]; then
        echo "the app is still running in the guest — not relaunching" >&2
        exit 1
    fi

    echo "starting it again, advertising $HOST"
    $SSH "cd /home/defaultuser && XDG_RUNTIME_DIR=/run/user/100000 \
          DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket \
          WAYLAND_DISPLAY=../../display/wayland-0 QT_QPA_PLATFORM=wayland \
          KELP_ADVERTISE_HOST=$HOST KELP_DEBUG_LINK=1 \
          setsid /usr/bin/harbour-kelp >/dev/null 2>&1 </dev/null &" || true

    printf 'waiting for it to serve on %s' "$HOST"
    SERVING_LINE=""
    i=0
    while [ "$i" -lt 30 ]; do
        SERVING_LINE="$($SSH "tail -n +$((FRESH_FROM + 1)) '$DATA_DIR/kelp.log' \
                              | grep -a 'KelpServer: serving on' | tail -1" || true)"
        if [ -n "$SERVING_LINE" ]; then
            break
        fi
        printf '.'
        sleep 1
        i=$((i + 1))
    done
    echo
    if [ -z "$SERVING_LINE" ]; then
        echo "the app is up but serving nothing: it holds no share, or every share" >&2
        echo "is paused. Its log since the restart:" >&2
        $SSH "tail -n +$((FRESH_FROM + 1)) '$DATA_DIR/kelp.log'" 2>/dev/null \
            | sed -E 's/#[kp]=[^ ]*/#<redacted>/g' | tail -12 >&2
        exit 1
    fi
    # Address and port only; the log is still read through the same redaction.
    printf '%s\n' "$SERVING_LINE" | sed -E 's/#[kp]=[^ ]*/#<redacted>/g'
fi

if [ -n "$FRESH_FROM" ]; then
    LOG="$($SSH "tail -n +$((FRESH_FROM + 1)) '$DATA_DIR/kelp.log'" 2>/dev/null || true)"
else
    LOG="$($SSH "cat '$DATA_DIR/kelp.log'" 2>/dev/null || true)"
fi

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
    echo "run this again with --relaunch to start it over advertising $HOST (or restart it" >&2
    echo "in the guest with KELP_ADVERTISE_HOST=$HOST KELP_DEBUG_LINK=1), then run it" >&2
    echo "with --host $HOST." >&2
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
echo "links (sealed by the app; #k= is the key, #p= the key under a passphrase):"
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
    # Both fragments are capability material: `#k=` is the key, `#p=` is the key
    # wrapped under a passphrase. --redact takes them both off.
    shown = re.sub(r"#[kp]=.*$", "#<redacted>", link) if redact else link
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
