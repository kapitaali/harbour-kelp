# Building Kelp

How to go from this tree to a running app: the two build targets, the
dev-extras bundle that separates a development build from a release, the tests,
what comes out and what has actually gone wrong. This is the canonical build
document — keep it current when the build changes, and put session-specific
evidence in `PROGRESS-<date>.md` instead.

Recipes for reaching an emulator share from the LAN, and for backgrounding the
app without its gesture, stay in `PROGRESS-<date>.md` §4 (they are about the
emulator, not about building).

## Short version

```sh
cd /home/theb/Jolla/harbour-kelp

# emulator
sfdk -c target=SailfishOS-5.1.0.11-i486 build
# phone (Jolla C2)
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build

# every test suite, in the build engine
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-all-tests.sh"
```

The RPM lands in `RPMS/SailfishOS-5.1.0.11-<arch>/harbour-kelp-0.1.0-1.<arch>.rpm`
(`RPMS/` is git-ignored, so never commit it and never expect a fresh clone to
have one). To run it, see *Deploy and run* below.

**Switching target needs a clean first** — see *The clean rule*.

## What a build needs

- **sfdk** (the Sailfish SDK) with the two targets above already provisioned:
  `SailfishOS-5.1.0.11-i486` for the emulator, `SailfishOS-5.1.0.11-aarch64` for
  the phone. `sfdk -c target=... build` is self-contained: it does not need the
  emulator running, and a build inside the engine uses the target's own Qt 5.6
  and OpenSSL 3.
- Nothing else from the host — no host Qt, no host OpenSSL. The tests too are
  compiled inside the engine.
- The engine has **no `git`** (`sh: git: command not found`). That matters for
  the Build ID below, and it means nothing in the build reads the repository.

## Dev build and release build — the dev-extras bundle

Some behaviour is for building and testing rather than for shipping, and the
user asked for exactly that split rather than a user-facing default. It lives in
**`src/kelpbuild.h`** (`KelpBuild::devExtras()`), and it is off unless the build
asks for it:

| | dev build | release build |
|---|---|---|
| How | `KELP_DEV_EXTRAS=1` at build time → `DEFINES += KELP_DEV_BUILD` (`src/kelpbuild.pri`) | anything else, including unset |
| Spec | `%define kelp_dev_extras 1` (currently set) | `%define kelp_dev_extras 0` |
| Link logging | *Include links in the log* defaults **on**, the switch is offered | never: no default, no switch, and `KelpBundles` refuses to write a link whatever the preference or `KELP_DEBUG_LINK` say |
| Says so | `Kelp: build mode — dev extras on` | `Kelp: build mode — dev extras off` |

The spec's `%define` is the switch that matters for a packaged build; a bare
`qmake` falls back to a release, so the safe answer is what you get when nobody
said anything and nothing can ship by a forgotten switch. QML reads the bundle
as the `kelpFeatures` context property (`kelpFeatures.devExtras`), so the next
dev-only feature needs no new context property.

Making a release is therefore: set `%define kelp_dev_extras 0` in
`rpm/harbour-kelp.spec`, rebuild, and check. What to check:

```sh
grep -c KELP_DEV_BUILD Makefile        # 0 — the Makefile that compiled has no bundle
grep -c 'dev extras off' <log>         # the launch agrees
grep -c 'kelp://' <log>                # 0 links, even with a share restored
```

## The clean rule

`sfdk build` compiles **in place** (the spec runs `%qmake5` in the source tree),
so the generated Makefile, `moc_*.cpp`/`*.moc` and object files for one
architecture sit in the tree until something removes them. Building the other
target on top of them either fails to link or, worse, links the wrong objects.
So switching target — and any build whose defines you have changed — starts with:

```sh
rm -f harbour-kelp *.o moc_*.cpp *.moc Makefile .qmake.stash
```

`sfdk -c target=... build-shell make distclean` does the same from inside the
engine when a richer clean is wanted. Nothing in the tree is precious: the app
builds in place by design and every generated file is reproducible.

Related, and load-bearing: **`make` cannot see a `-D` change.** The dev-extras
bundle is a define, so `harbour-kelp.pro` touches the files whose compilation
depends on one (`_version_touch`) on every qmake run, and the switch also has to
be flipped with a clean. See *Traps*.

## What comes out

- **`RPMS/SailfishOS-5.1.0.11-<arch>/harbour-kelp-0.1.0-1.<arch>.rpm`** — the
  package: the binary, `qml/`, the desktop file, the QR library's license notice.
  The version comes from the spec (0.1.0), not from git.
- **Build ID.** The About page's "build id" prefers `git rev-parse --short HEAD`
  and falls back to a UTC timestamp — and since the engine has no git, every
  packaged build shows the fallback: `20261017-103244` is a *time*, not a
  revision. Identify a build by what it logs (`Kelp: build mode — …`, then
  `Kelp <version> (build …)`), not by that string; the host repo's `git log` is
  the history.
- A benign linker warning about `libcrypto.so.1.1` (wanted by `libQt5Network`)
  against `libcrypto.so.3`: both are on the device, and `EVP_*` resolves to 3.x.
  Explained in `kelp-design.md`.

## Tests

All of them run inside the build engine (target Qt/OpenSSL), out of source, each
in its own `.build` directory that is cleaned first — so switching target between
test runs cannot leave foreign objects behind.

```sh
# one suite
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-kelpsettings-test.sh"
# everything, one line per suite
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-all-tests.sh"
```

| Suite | What it pins | Checks |
|---|---|---|
| `libkelp/tests/run-tests.sh` | RFC 5869 + GCM + PBKDF2 vectors, wire-spec §16 manifest/object vectors, the whole seal→open round trip, corrupt-object refusal, the §5.3 page chooser, the serving policy's truth table (metered, battery, charging, at and below the threshold, unknown battery), and the passphrase link form (`#p=` wrapping, unlock, the AAD binding, the iteration cap, both fragments at once) | 497 |
| `src/tests/run-kelpfetch-test.sh` | the LAN request header, base64url swarm token, the reachability rule | 37 |
| `src/tests/run-kelpsettings-test.sh` | the persisted preferences — including the three serving switches and the dev-extras default, from both sides | 39 |
| `src/tests/run-kelpshares-test.sh` | the persisted share rows | 24 |
| `src/tests/run-kelpqr-test.sh` | QR encoding, against Python's `qrcode` | 9 |
| `src/tests/run-kelpbundles-test.sh` | seal, webpage shares (`page` in the row and `p` in the link), passphrase shares (`#p=` in the link, `passBlob` in the row, restores and the 8-character floor), the local-address gate, the link-log gate, storage management, pause/resume, the preview seam | 211 |
| `src/tests/run-kelppreview-test.sh` | the §5.3 page convention, path → objects, the URL space (`404` vs `503`), `Range`/`HEAD`, the security headers, an expiring session and `has()` for a caller that kept a URL, the 404 body that says the link can be opened again | 206 |
| `src/tests/run-kelpclient-test.sh` | receiving a link: a view writes nothing (the app store is byte-identical before and after), the page ceiling from `/info` and while storing, a link whose bundle has no page, the save path, a passphrase link refused without (and with the wrong) passphrase before the publisher is dialled, that a second view does not free the first session's store, that a swept session is re-minted rather than handed out dead, and that a finished fetch knows which link it belongs to | 152 |
| `relay/test/run-relay-test.sh` | the relay end to end, publisher and recipient both played | 6 cases |

**Run the suites in both bundle modes.** Two of them check a default that
follows the build, so a run only covers one side of it:

```sh
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh "$PWD/src/tests/run-all-tests.sh"
sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell env KELP_DEV_EXTRAS=1 \
    sh "$PWD/src/tests/run-all-tests.sh"
```

(That the mode really was what it claims can be checked, after the fact, with
`grep -c KELP_DEV_BUILD src/tests/.build/Makefile`.)

Two of the harnesses under `src/tests/` are tools rather than test suites:
`run-emu-share.sh` bridges an emulator share to the LAN and can fetch from it — and with
`--relaunch` it restarts the guest app advertising this host first, which is what makes an
emulator link reachable at all from another device — and `kelp-probe.py` fetches from a
link the way a recipient would without printing its key. Both are documented in
`PROGRESS-<date>.md` §4.5.

`run-qml-smoke.sh` covers what the suites cannot: a page is compiled only when
it is pushed, so a page that assigns a property its Silica type does not have —
Sailfish's `TextField` has no `accepted` signal, which is how §3.30 shipped —
starts the app fine and only shows *Could not load page* when a person taps the
menu. The script appends a temporary timer to the tree's `harbour-kelp.qml`,
installs it over the guest's copy, restarts the app, compiles every `.qml` under
`qml/` (the list comes from the tree), restores the installed file, and proves
its md5 is the tree's again:

```sh
src/tests/run-qml-smoke.sh
# QML smoke: 9/9 pages Component.Ready
# installed harbour-kelp.qml restored to the tree (62cd959a08dad869233d21400b03fc2a)
```

It restarts the app, so re-run `run-emu-share.sh --relaunch` afterwards when you
need a reachable link. It checks the *installed* pages, so install the build
first. Negative control run: re-adding `onAccepted` to the installed
`OpenLinkPage.qml` makes it exit 1 and name the file, line and error.

That reachability only holds for the app instance the script started. Start Kelp from the
guest's own app grid instead — or restart it, or install a package — and it goes back to
advertising the guest's `10.0.2.15`, so its links read `10.0.2.15:<port>` and nothing
outside the emulator can open them. If a link that worked a moment ago says `10.0.2.15`,
re-run `run-emu-share.sh --relaunch` and copy the link **again**: the address is sealed into
the link at mint time, so the old one stays dead (PROGRESS §2.5, met twice).

To try a *webpage* share end to end — the one thing that needs something to look at — the repo
carries `demo/`: a three-file page with no external reference at all, which is what a viewed page
has to be (the preview serves it offline, design §5.1/§12.11). Copy the folder in and point
*Share… → Share webpage* at it; it is also the honest test of relative sub-resources resolving
through a bundle (§3.24).

### The serving policy's two inputs

The rule itself is pure and covered by `libkelp/tests`; what a device can *prove* is the
plumbing, and an emulator has neither a cellular connection nor a battery that can be drained.
Two environment variables make those situations reachable there, read once at startup and
deliberately **not** behind the dev-extras bundle — they cannot make the app serve something it
would not otherwise have served, only stop it sooner:

| Variable | Effect |
|---|---|
| `KELP_FORCE_METERED=1` | the only usable connection counts as mobile data |
| `KELP_FORCE_BATTERY=<0..100>` | a discharging battery of that percentage (charging cannot be forced) |

Launch the guest app with either, the way the deploy recipe does but with the variable added:

```sh
ssh -p 2223 -i $KEY defaultuser@127.0.0.1 \
    'cd /home/defaultuser && XDG_RUNTIME_DIR=/run/user/100000 \
     DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket \
     WAYLAND_DISPLAY=../../display/wayland-0 QT_QPA_PLATFORM=wayland \
     KELP_ADVERTISE_HOST=<this host> KELP_DEBUG_LINK=1 KELP_FORCE_METERED=1 \
     setsid /usr/bin/harbour-kelp >/dev/null 2>&1 </dev/null &'
```

Then read the verdict off the log (`KelpDeviceState:`, then `KelpPolicy: serving stopped — …` or
`KelpPolicy: serving allowed`), and check the network half with
`run-emu-share.sh --host <this host> --redact --fetch`, which must report
`401 bad token` / `NOT SERVING` while blocked. The clearest observable is the link *behaviour*,
not the link text: flip the Wi-Fi-only switch off (a temporary QML hook is the honest way — a
restart would move the ephemeral port, and a move is the one thing this is testing) and fetch
through a link minted *before* the stop. It must still serve. Nothing about the switch changes
the same-app-in-front rule: it only decides whether serving happens at all.

## Checks before committing

1. Build the target you changed (both, if the change is shared).
2. `run-all-tests.sh` in both bundle modes.
3. If you touched QML or anything user-visible: deploy, launch, and confirm the
   log has no `WARN`/`ERROR` beyond Silica's own known `DialogHeader` warnings
   (`grep -aE "WARN |ERROR" <log>` — QML warnings only reach the log file, not
   the launch's stderr).
4. Never paste a link's `#k=` fragment into a doc, a log or a message: it is the
   decryption key. `run-emu-share.sh --redact` exists for output that might be
   surfaced.

## Deploy and run

Emulator (the full recipe, including the guest environment the app needs to
start at all, is `PROGRESS-<date>.md` §4.2–4.4):

```sh
cd /home/theb/Jolla/harbour-kelp          # scp needs this as the working directory
KEY=~/SailfishOS/vmshare/ssh/private_keys/sdk
scp -P 2223 -i $KEY RPMS/SailfishOS-5.1.0.11-i486/harbour-kelp-0.1.0-1.i486.rpm \
    defaultuser@127.0.0.1:~
ssh -p 2223 -i $KEY defaultuser@127.0.0.1 \
    'sudo pkcon install-local -y ~/harbour-kelp-0.1.0-1.i486.rpm; rm -f ~/harbour-kelp-0.1.0-1.i486.rpm'
```

Phone (Jolla C2, aarch64) — the same install on the device: copy
`RPMS/SailfishOS-5.1.0.11-aarch64/harbour-kelp-0.1.0-1.aarch64.rpm` over (USB or `scp`), then
`devel-su pkcon install-local -y ~/harbour-kelp-0.1.0-1.aarch64.rpm`, with **developer mode** on.
The log is `~/.local/share/harbour.kelp/harbour-kelp/kelp.log`, the same path as in the guest.
`PROGRESS-<date>.md` §4.7 is the recipe for trying the preview there.

Then launch it with the guest's Wayland/DBus environment (an app launched
without it exits immediately), and read the log to see which build it is and
whether it is clean:

```sh
ssh -p 2223 -i $KEY defaultuser@127.0.0.1 \
    'cd /home/defaultuser && XDG_RUNTIME_DIR=/run/user/100000 \
     DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket \
     WAYLAND_DISPLAY=../../display/wayland-0 QT_QPA_PLATFORM=wayland \
     setsid /usr/bin/harbour-kelp >/dev/null 2>&1 </dev/null &'
grep -aE 'build mode|WARN |ERROR' <log>   # ~/.local/share/harbour.kelp/harbour-kelp/kelp.log
```

A local build made outside the spec (a test binary, a `qmake && make` in the
engine) is a **release build** unless `KELP_DEV_EXTRAS=1` is passed to qmake —
the safe default, and it is why a test run must say which mode it wants.

Starting the emulator is separate from building for it, and on this host
`sfdk emulator start` loops on `VERR_VMX_IN_VMX_ROOT_MODE` while `kvm_intel` is
loaded: the user unloads KVM first (`sudo modprobe -r kvm_intel kvm`) and reloads
it when done. The rest of the emulator's environment is in
`PROGRESS-<date>.md` §6 and in the `sailfish-os-dev` skill's
`environment-setup.md`.

## Traps

Each of these has cost time; they are the reason this file exists rather than a
line in the README.

- **In-source builds.** See *The clean rule*. If a link fails with duplicate
  `qt_static_metacall` symbols, the cause is a stale in-tree generated file
  shadowing a test's own — which is why the test projects deliberately keep the
  project root out of `INCLUDEPATH`.
- **A define is invisible to `make`.** Flipping the dev-extras bundle and
  rebuilding incrementally leaves the previous mode compiled in, silently.
  `harbour-kelp.pro` touches every file that depends on a define, but a clean is
  the habit.
- **Do not edit a build input while a build is running.** `sfdk build` re-runs
  qmake only at the start, but `%qmake5_install` is a `make install`, and a
  `make` that finds a stale Makefile regenerates it — through a qmake that has
  no `KELP_DEV_EXTRAS`, because the spec exports it only in `%build`. The
  symptom is the build printing the switch message twice, `on` then `off`. The
  mode of a build is decided by whichever Makefile compiled the objects, so when
  in doubt read `grep -c KELP_DEV_BUILD Makefile` or rebuild untouched.
- **No git in the engine**, so the Build ID is a timestamp (see above).
- **Adding a source file means adding it everywhere it is used**: the app's
  `harbour-kelp.pro`, and every test `.pro` that links the code it belongs to
  (`tst_kelpsettings.pro` and `tst_kelpbundles.pro` link `kelpsettings.cpp` and
  so need `kelpbuild.cpp` too). A missing one is an `undefined reference` at
  link, not a compile error.
- **A test that talks to its own listener must return to the event loop.** The
  LAN listener is a `QTcpServer` in the test's thread, so a helper that blocks in
  `waitForConnected`/`waitForReadyRead` never gets accepted; drive the socket
  with signals and a `QEventLoop` (`tst_kelpbundles`, `httpGet`).
- **Sailfish's Qt is 5.6**, not 5.15: no `QAbstractSocket::errorOccurred`, no
  `QOverload`, and nothing else from 5.7+. Check the header rather than the
  docs when an API "should" exist.
- **Stop the app with `killall harbour-kelp`, then check.** `pkill -f
  /usr/bin/harbour-kelp` over ssh matches the *remote shell's own command line*
  and kills the shell with the app (ssh exits 255 and prints nothing), while
  BusyBox's `pgrep -x`/`pkill -x` did not match the app here — so a
  "stopped" that was never checked left two app instances writing one log and one
  `shares.json`. `ps -o pid,comm | grep harbour` is the check (there must be one).
- **A QML delegate reads model roles by name, and an `Item` property wins.** In a
  `ListItem` delegate an unqualified `state` is `Item::state` (always `""`), not
  the model's `state` role — the share list said "Offline" for every row and a
  paused share could not be resumed until the role was renamed `shareState`. The
  same class of miss: a delegate binding to a role a *row* lacks throws
  `ReferenceError: <role> is not defined`, so every `ListModel.append()` must
  carry every key the delegate reads (`PROGRESS-<date>.md` §2.6).
- **Photograph a page from inside the app.** `pageStack.grabToImage()` (Qt 5.6
  `saveToFile` takes a plain path, not a `file://` URL) is the only way to see a
  page on this host — the emulator window is not capturable and `/dev/fb0` is not
  what the compositor draws. Such an image has no ambience wallpaper behind it, so
  theme-*primary* text (near-white) is invisible on white: read it for layout and
  wording, not for colour.
