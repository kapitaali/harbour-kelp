#!/bin/sh
#
# run-qml-smoke.sh — compile every page the way PageStack would, on the emulator.
#
# A page is compiled only when it is pushed. So a page that assigns a property
# its type does not have — or that drops a brace — ships green through every C++
# suite, starts the app fine, and only shows **Could not load page** when a
# person taps the menu item that opens it. That is exactly how §3.30 shipped:
# Sailfish's TextField has no `accepted` signal, and OpenLinkPage assigned
# `onAccepted` on it, so the document could not be created at all.
#
# This closes that hole without a human tapping anything:
#
#   * takes the tree's qml/harbour-kelp.qml and appends a temporary Timer that
#     calls Qt.createComponent() on every page under qml/ (the list comes from
#     the tree, so a new page is covered without editing this script),
#   * logs one `QML-SMOKE <file> <status>` line per page, where 1 is
#     Component.Ready, plus `QML-SMOKE done <n>`,
#   * installs that file over the guest's copy, restarts the app plainly,
#     reads the lines back, and puts the installed file back,
#   * proves the installed file is the tree's again before it exits (md5).
#
# The hook is never committed and never left behind. What is checked is the
# *installed* qml tree, so install the build you mean to test first.
#
# It restarts the app in the guest, so a share advertised by run-emu-share.sh
# stops answering: re-run that (it mints a new port anyway) when you need links.
#
# Usage: src/tests/run-qml-smoke.sh
#
# Exit 0 = every page Ready; 1 = a page failed (the file and its error print).
set -eu

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SSH_PORT="${KELP_SSH_PORT:-2223}"
SSH_KEY="${KELP_SSH_KEY:-$HOME/SailfishOS/vmshare/ssh/private_keys/sdk}"
QML_DIR=/usr/share/harbour-kelp/qml
DATA_DIR="${KELP_EMU_DATAHOME:-/home/defaultuser/.local/share/harbour.kelp/harbour-kelp}"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
     -p $SSH_PORT -i $SSH_KEY defaultuser@127.0.0.1"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/kelp-qml-smoke.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PAGES=$(cd "$REPO/qml" && find . -name '*.qml' ! -name 'harbour-kelp.qml' \
        | sed 's|^\./||' | LC_ALL=C sort)
[ -n "$PAGES" ] || { echo "no pages found under $REPO/qml" >&2; exit 1; }
PAGE_COUNT=$(printf '%s\n' "$PAGES" | wc -l | tr -d ' ')

QML_LIST=$(printf '%s\n' "$PAGES" \
           | awk 'BEGIN { first = 1 }
                  { if (!first) printf ", "; printf "\"%s\"", $0; first = 0 }')

{
    head -n -1 "$REPO/qml/harbour-kelp.qml"
    cat <<EOF

    // TEMPORARY smoke hook (src/tests/run-qml-smoke.sh) — never committed.
    Timer {
        interval: 3000
        repeat: false
        running: true
        onTriggered: {
            var names = [$QML_LIST]
            for (var i = 0; i < names.length; ++i) {
                var c = Qt.createComponent(names[i])
                kelpLog.info("QML-SMOKE " + names[i] + " " + c.status
                             + (c.status === Component.Error ? " " + c.errorString() : ""))
            }
            kelpLog.info("QML-SMOKE done " + names.length)
        }
    }
EOF
    echo "}"
} > "$TMP/harbour-kelp.qml"

# Only replace a file we know: if the guest has something else installed, the
# pages it would smoke are not the ones in this tree, and a pass would lie.
TREE_MD5=$(md5sum "$REPO/qml/harbour-kelp.qml" | awk '{print $1}')
INSTALLED_MD5=$($SSH "md5sum $QML_DIR/harbour-kelp.qml" | awk '{print $1}')
if [ "$TREE_MD5" != "$INSTALLED_MD5" ]; then
    echo "the guest's harbour-kelp.qml ($INSTALLED_MD5) is not the tree's ($TREE_MD5)." >&2
    echo "install the build you want to test first; refusing to overwrite it." >&2
    exit 1
fi

scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    -P "$SSH_PORT" -i "$SSH_KEY" "$TMP/harbour-kelp.qml" defaultuser@127.0.0.1:/tmp/kelp-qml-smoke.qml
$SSH "sudo cp /tmp/kelp-qml-smoke.qml $QML_DIR/harbour-kelp.qml && rm -f /tmp/kelp-qml-smoke.qml"

# Restart the app by name via /proc: the ssh command line itself matches any
# `pkill -f` pattern, and `pkill -x` has silently missed it on this device.
$SSH 'for f in /proc/[0-9]*/comm; do
          [ "$(cat "$f" 2>/dev/null)" = harbour-kelp ] &&
              kill "$(basename "$(dirname "$f")")" 2>/dev/null
      done; true'
i=0
while [ "$i" -lt 10 ]; do
    [ -z "$($SSH 'cat /proc/[0-9]*/comm 2>/dev/null | grep -x harbour-kelp' || true)" ] && break
    sleep 1
    i=$((i + 1))
done

BEFORE=$($SSH "wc -l < '$DATA_DIR/kelp.log'" 2>/dev/null | tr -d ' ' || echo 0)
case "$BEFORE" in ''|*[!0-9]*) BEFORE=0 ;; esac

$SSH "cd /home/defaultuser && XDG_RUNTIME_DIR=/run/user/100000 \
      DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket \
      WAYLAND_DISPLAY=../../display/wayland-0 QT_QPA_PLATFORM=wayland \
      setsid /usr/bin/harbour-kelp >/dev/null 2>&1 </dev/null &" || true

LINES=""
i=0
while [ "$i" -lt 30 ]; do
    LINES=$($SSH "tail -n +$((BEFORE + 1)) '$DATA_DIR/kelp.log' \
                 | grep -a 'QML-SMOKE '" 2>/dev/null || true)
    printf '%s\n' "$LINES" | grep -q 'QML-SMOKE done' && break
    sleep 1
    i=$((i + 1))
done

# Put the installed file back before saying anything, so a failed run cannot
# leave the hook in the package.
scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
    -P "$SSH_PORT" -i "$SSH_KEY" "$REPO/qml/harbour-kelp.qml" defaultuser@127.0.0.1:/tmp/kelp-qml-plain.qml
$SSH "sudo cp /tmp/kelp-qml-plain.qml $QML_DIR/harbour-kelp.qml && rm -f /tmp/kelp-qml-plain.qml"
RESTORED_MD5=$($SSH "md5sum $QML_DIR/harbour-kelp.qml" | awk '{print $1}')
if [ "$RESTORED_MD5" != "$TREE_MD5" ]; then
    echo "the guest's harbour-kelp.qml was NOT put back ($RESTORED_MD5 != $TREE_MD5)." >&2
    echo "reinstall the package before continuing." >&2
    exit 1
fi

if ! printf '%s\n' "$LINES" | grep -q 'QML-SMOKE done'; then
    echo "no QML-SMOKE lines came back — the app did not reach its timer." >&2
    echo "is it installed and able to start? (guest log: $DATA_DIR/kelp.log)" >&2
    exit 1
fi

# One result per page plus the `done` marker. A status that is not 1 carries
# its errorString after it, so the last field is only `1` on a ready page.
SEEN=$(printf '%s\n' "$LINES" | grep -c 'QML-SMOKE ' || true)
DONE=$(printf '%s\n' "$LINES" | grep -c 'QML-SMOKE done' || true)
BAD=$(printf '%s\n' "$LINES" | grep -a 'QML-SMOKE ' | grep -v 'QML-SMOKE done' \
      | awk '{ if ($(NF) != "1") print }' || true)

if [ "$DONE" -ne 1 ] || [ "$SEEN" -ne $((PAGE_COUNT + 1)) ] || [ -n "$BAD" ]; then
    echo "QML smoke: FAILED"
    [ -n "$BAD" ] && printf '%s\n' "$BAD" \
        | sed -E 's/^.*QML-SMOKE ([^ ]+) ([0-9]+) ?(.*)$/  not ready: \1 (status \2) \3/'
    [ "$SEEN" -ne $((PAGE_COUNT + 1)) ] &&
        echo "  saw $((SEEN - 1)) page result(s), expected $PAGE_COUNT"
    exit 1
fi

echo "QML smoke: $PAGE_COUNT/$PAGE_COUNT pages Component.Ready"
echo "installed harbour-kelp.qml restored to the tree ($RESTORED_MD5)"
