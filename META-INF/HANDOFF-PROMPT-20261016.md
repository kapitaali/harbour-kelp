You are continuing another agent's work on Kelp.

Objective
- Kelp is a secure peer-to-peer file-sharing app for Sailfish OS (Jolla C2, aarch64,
  Sailfish OS 5.x): the user picks a folder or file, shares it, and hands out a kelp://
  link that a recipient opens in the app to fetch the bundle.
- Sharing works both on the same Wi-Fi (LAN-direct) and across networks through a relay.
  Milestones C1 (libkelp), C1.5 (app wiring), C2a (LAN publish), C2b (fetch), C3.1
  (tunnel transport), C3.2 (relay) and C3 (cross-network fetch) are done and verified.
- This milestone: harden what exists before adding features. The first device-to-device
  LAN transfer was only completed on 2026-10-16 and it exposed three bugs, two of which
  no test could see. Fix the class, not just the instances.

Read before touching anything
- META-INF/BUILD.md — the canonical build and test document: targets, the clean
  rule, the dev-extras dev/release switch, every suite, the build traps.
- META-INF/PROGRESS-20261016.md — the newest session record: the three bugs with root
  causes, the exact evidence for what was verified, build/deploy/launch recipes, and a
  traps list where every entry cost the previous agent time. Read this first.
- META-INF/kelp-design.md — the design and the milestone list (§3.1 records the deliberate
  LAN deviations, §12 the open questions).
- META-INF/kelp-swarm-wire-spec.md — the wire format; §5 the link, §7.2 direct fetch,
  §16 pinned vectors.
- README.md — current state, screens, how to debug.
- META-INF/ipfs-fileshare-PLAN.md — parked, separate name. Leave it as-is.
- The repo IS a git repository now (git init was done on 2026-10-16). Check `git log` for
  where things stand. Commit your work as you go, and keep the dated PROGRESS-<date>.md
  notes up to date — they are also the history.

Requirements
- Keep planning docs as loose .md files under harbour-kelp/META-INF/.
- Prefer reusing real IPFS code over reimplementing it (see the parked plan).
- Keep the native kelp:// URI scheme.
- Keep a logging feature so you can debug by reading logs.
- Keep platform conventions; do not fight Sailfish interaction paradigms.
- UI wording: standardise on "folder" (not "directory").
- Every bundle is encrypted; there is no public/private distinction.
- libkelp stays pure, testable, no I/O and no networking; listeners, tunnels and fetch
  logic live in the app layer. Internet is the only permission needed; Sailfish has no
  LocalNetwork permission.
- Verify with real tool output, not write-ups, and leave the test device clean afterwards.
- Never write a link's #k= fragment into a doc, a log or a chat message: it is the
  decryption key.

Decisions already taken — don't relitigate
- Name Kelp, app id harbour-kelp, scheme kelp://. SHA-256 content-addressing with canonical
  CBOR manifests, AES-256-GCM, key in the URL fragment. Identity hashed, location sealed
  and key not: bundle_id = SHA-256 of the root manifest, relays as r=<relay> and the
  publisher's own address+label sealed as q=<…> in the query. HKDF-SHA256, salt = 32 zero
  bytes, info strings kelp/v1/content, kelp/v1/swarm, kelp/v1/relay, kelp/v1/link.
- LAN transport is deliberately plain HTTP: content is ciphertext and hash-verified, Qt 5.6
  cannot do TLS 1.3 + ALPN, and the cost (an on-path sniffer learns the token, not the
  plaintext) is recorded in kelp-design.md §3.1. The tunnel keeps TLS 1.3 + ALPN "kelp/1",
  driven through OpenSSL 3 directly (src/kelptls.{h,cpp}) because Qt cannot.
- A relay serves publisher tunnels and recipient fetches on one port, told apart by the
  first byte — a link's r= can only name one address. Unknown bundle, unshared bundle and
  wrong token all answer 401, deliberately: no enumeration oracle.
- Swarm is opt-in per share, off by default, gated by a global setting in Settings.

Current verified state (2026-10-16)
- First real device-to-device LAN transfer, phone -> emulator: the phone served a
  2 136 168-byte JPEG (11 objects, 1 file) on 172.28.172.1:34637; the emulator fetched
  11 objects in 4 rounds, wrote ~/Downloads/20260924_124949.jpg, and the file is a
  complete valid 4096x3072 JPEG (sha256 aea871ba24141a462828df0da156be6f9631826b1376a9673ae0aa55a6a402cc).
- Shares persist: a restart restores the rows and re-serves them on a fresh LAN port;
  a second instance fetched through the restored link and the file was byte-identical.
- libkelp: 345 checks, 0 failures. App-layer harnesses: tst_kelpfetch 37, tst_kelpsettings
  24, tst_kelpshares 24, tst_kelpqr 9, tst_kelpbundles 98 (all 0 failures). Relay harness: 6/6
  ALL PASS, now against the app's own tunnel.
- A link no longer spells out the publisher's address: encode() seals the address and label
  into q= (AES-256-GCM under HKDF(secret, "kelp/v1/link"), bundle id as AAD); decode() still
  accepts the old plaintext l=/n= form. Verified live (PROGRESS §3.11).
- Settings -> Storage shows what the object store holds and what the shares on this device
  need, and Remove unused objects deletes exactly the rest — refusing, and deleting nothing,
  if any share cannot be walked. Verified live through the real page (PROGRESS §3.12).
- A real phone fetched from the emulator's share: the app relaunched with
  KELP_ADVERTISE_HOST so its links named the host instead of 10.0.2.15, run-emu-share.sh
  bridged the port and printed a fresh sealed link and QR, and the phone fetched the bundle
  (confirmed by the user, PROGRESS §3.13).
- The serving side now says who fetched: KelpServer tallies each peer's fetches, objects,
  bytes and 401s and writes one line per peer once it is quiet, so a publisher can tell a
  fetch that worked from a recipient that never arrived — the blind spot the debugging
  session hit. src/tests/kelp-probe.py + run-emu-share.sh --fetch check a share from this
  host the way a recipient would, without printing the link. tst_kelpbundles 79 checks,
  verified live inside the sandbox (PROGRESS §3.14).
- The relay half reports too, per tunnel (the relay multiplexes recipients): KelpTunnel
  counts objects wanted, served, their bytes, and anything wanted but not held, and writes a
  KelpTunnel: line when the asking stops or the tunnel drops. The relay harness now runs the
  app's own KelpTunnel as the publisher instead of a hand-written stand-in, so the relay is
  tested against the code the device runs: 6/6, with the tally pinned (PROGRESS §3.15).
- A share can be **paused and resumed**, not only deleted (user-requested: "background
  serving needs a pause function to supplement the stop/delete function"). Pause stops
  serving on both paths but keeps the row and objects; `paused` is written to shares.json so
  it stays paused across a restart; resume is `republish()`. `unshare()` and `pause()` share
  a private `stopServing()`; the store owns `pauseShare()`/`resumeShare()` so the list menu
  and the Bundle page agree, and the cover's play/pause action is a real pause-all/resume-all.
  tst_kelpbundles 93 checks (was 79), verified live end to end on the emulator — both restore
  paths, the Bundle page's pause/resume, and the cover action — with a clean log (PROGRESS
  §3.16). Two live-found fixes came out of it: `linkInfo()` inserts empty defaults even on a
  failed decode (a paused share has no `lan`, and one page read it unguarded), and the cover
  binds to store-maintained `servedShares`/`pausedShares` counters rather than calling
  `ListModel.get()` inside a binding, which QML reported as a binding loop.
- Serving now survives the window going away, and that is deliberate rather than incidental
  (design §12.8). Sailfish keeps a backgrounded app alive, so a share already kept serving on
  the cover; what was added is the `Nemo.KeepAlive` request — held only while something is
  really being served and *Share in background* is on — plus a log line for
  `Qt.application.state`. Verified live **with Kelp on its cover**: a recipient-style fetch
  still answered 200/401 as it should and the publisher wrote its traffic line (PROGRESS
  §3.17, recipe §4.6). Not done: Wi-Fi-only / low-battery / sleep policy, and persisting the
  switch. Closing the app still ends serving — a Harbour app cannot ship an always-on service.
- Links are logged by default **while developing, never in a release**: the user asked for one
  and then asked for the better shape — put it in a feature bundle that a release build turns
  off. That is src/kelpbuild.{h,cpp} (`KelpBuild::devExtras()`), a build-time value that is off
  unless `KELP_DEV_EXTRAS=1` asks for it; the spec sets it while developing
  (`%define kelp_dev_extras 1`) and a release sets 0 (README, *Development builds*). QML sees it
  as the `kelpFeatures` context property so the *Include links in the log* switch is simply not
  shown in a release, and `KelpBundles::linksInLog()` refuses in a release whatever the
  preference or KELP_DEBUG_LINK says — a release build never writes a link (and so a key) into
  the log. Every launch says which build it is: `Kelp: build mode — dev extras on|off`. Verified
  live in both modes on the same share, plus both modes of tst_kelpsettings and tst_kelpbundles
  (PROGRESS §3.18).
- A share's link can be shown as a QR code on the Bundle page (user-requested): the encoder
  is vendored Nayuki qrcodegen (MIT, src/qrcodegen.*), the item is src/kelpqr.*, and the
  rendered code was verified by scanning it with a real decoder (PROGRESS §3.8).
- An unreachable sender is no longer reported as one that "stopped answering": the
  transports now probe for a first sign of life (LAN 8 s, relay 10 s) and say *could not
  reach the sender* when there is none, leaving the 20 s silence deadline for a peer that
  has answered and then gone quiet. Found by the user (emulator behind QEMU user-mode NAT
  cannot be dialled into); verified live, pinned in tst_kelpfetch (PROGRESS §2.5, §3.9).
- Phone RPM with the current fixes: build it (see below); the emulator RPM from the last
  session is RPMS/SailfishOS-5.1.0.11-i486/harbour-kelp-0.1.0-1.i486.rpm. The repo IS a
  git repository now (git init done), so commit the work — the earlier note that it was
  not is stale.

Next Move (in this order — 1–3 are done as of this note)
1. Regression test for the LAN request header. src/kelpfetch.cpp's LanSource puts the
   swarm token in Authorization: Bearer <token>, and the sender base64url-decodes that
   header and compares bytes. It was handed raw bytes for a while, which the sender
   silently answered 401 ("the sender did not recognise this link"). Nothing tests it.
   Pin the format with a test that asserts the header equals base64url(swarmToken(secret)).
   **Done** — src/tests/tst_kelpfetch.cpp, 29 checks (negative control verified; since
   extended with the reachability rule, §2.5/§3.9).
2. Settings -> "Include links in the log", persisted, off by default. Today the link is
   only written to kelp.log when the app is launched with KELP_DEBUG_LINK=1, which makes
   the emulator (no usable clipboard) need a shell launch to get a link at all.
   **Done** — src/kelpsettings.{h,cpp}, switch under Settings -> Diagnostics,
   KELP_DEBUG_LINK kept as a one-run override. *(Default superseded in PROGRESS §3.18: it
   is now the build's dev-extras bundle — on while developing, off and not offered in a
   release, which never writes a link at all.)*
3. Make a share survive reality: shared rows are not persisted, so a restart silently
   drops the share; and the link's address is captured at share time, so a network change
   makes it stale with no indication to the user.
   **Done** — src/kelpshares.{h,cpp} (owner-only shares.json), restored and re-served at
   startup, KelpBundles::republish() mints a fresh link. Verified live, including a fetch
   through a restored link and a sandboxed pass (PROGRESS §3.7). While here, "Stop sharing"
   from the Bundles list was found not to stop serving (PROGRESS §2.4) and was fixed.
4. QR-code sharing (user-requested, done between the planned moves): the Bundle page's
   placeholder is now a real, scannable QR of the whole link. **Done** — src/qrcodegen.*
   (vendored MIT Nayuki generator) + src/kelpqr.{h,cpp} (the Qt Quick item), tst_kelpqr
   (9 checks) pinning the encoder against an independent implementation, and a live scan
   with a real decoder (PROGRESS §3.8). Follow-up: republish() should honour the
   "Include links in the log" setting that seal() already does, so a restored share's link
   can still be read from the log on a device with no clipboard.
5. Store management: nothing ever deletes objects and the store now also grows from what
   others send. Show its size; give bundles a Remove (needs the object graph walked
   backwards) and/or an LRU. See kelp-design.md §12.7. **Done** — the answer is neither
   per-bundle Remove nor an LRU: the store is one deduplicated pool and the graph points
   down, so "unused" is only definable globally. Kelp::bundleObjects() (libkelp) walks every
   share's graph in full and strictly (an unreadable manifest fails the call, so unaccounted
   objects are never called junk); KelpBundles::storeUsage()/pruneStore() drive Settings ->
   Storage (what is here, what the shares need, what is left over, a confirmed clean-up).
   libkelp 345 checks, tst_kelpbundles 54, verified live on the emulator (PROGRESS §3.12).
6. **Done** — the publisher's own view of a fetch: KelpServer::Traffic (per-peer fetches,
   objects, bytes, refusals) written as one line per peer once it goes quiet, plus
   kelp-probe.py and run-emu-share.sh --fetch to check a share from this host without
   printing its key. tst_kelpbundles 79 checks, verified live (PROGRESS §3.14). The relay
   half is done too (PROGRESS §3.15): KelpTunnel counts objects wanted/served/not-held and
   reports when the asking stops or the tunnel drops, and the relay harness now runs the app
   own tunnel as the publisher — 6/6.
7. **Background serving** (the reason pause was added) — **done in-app.** Sailfish keeps a
   backgrounded app alive, so serving already continued on the cover; what was added is the
   `Nemo.KeepAlive` request, held only while something is really served and *Share in
   background* is on, plus a `Qt.application.state` log line. Verified live with the app on
   its cover (PROGRESS §3.17, recipe §4.6). Still design, not code: Wi-Fi-only / low-battery /
   sleep policy, and persisting the switch (kelp-design.md §12.8). Serving still ends when the
   app is *closed* — a Harbour app cannot ship a systemd user service. A bottom `PushUpMenu`
   toggle was considered at the user's suggestion and dropped: an app cannot background itself
   (lipstick exposes no such call, and `/WindowModel` is a read-only model) and the user would
   rather use the real device's swipe.
8. Only then C4a (shared-page preview — design §5.1), C4b (relay gateway — §5.2) or C7
   (swarm). C4a is the one the user is interested in next, and the design is now written
   out in full (`kelp-design.md` §5): why the browser cannot decrypt from the sender (no
   WebCrypto outside a secure context), the loopback server's URL space and header policy,
   the system-browser-vs-`Sailfish.WebView` route decision, and why a preview is a
   confirmation rather than a default. It is also the one piece that is testable entirely
   off-device — a sealed bundle, a store and an HTTP client, no browser — so the preview
   server is the natural first step.
Also worth doing opportunistically: replace the remaining printf/fprintf(stderr) debug
lines with KelpLog calls, so everything lands in the log file; and note that *Wi-Fi only* and
*Allow peer swarm* are still inert placeholders (*Share in background* is now acted on, but
is not persisted).

Relevant files
- src/kelpfetch.{h,cpp} — KelpSource, LanSource, RelaySource. Bug 1 lives here (create()).
- src/kelpclient.{h,cpp} — the fetch loop: walk, ask, fetch, repeat; writes the tree.
- src/kelpserver.{h,cpp} — the LAN listener the client talks to (endpoints, auth, guards),
  and the Traffic tally behind the "who fetched" log line.
- src/tests/kelp-probe.py — fetch from a link the way a recipient would (deriving the token
  with HKDF-SHA256, kelp/v1/swarm) without ever printing the link; exit status 0 only if
  every check passed. run-emu-share.sh --fetch drives it through the forward.
- src/kelpbundles.{h,cpp} — seal()/republish()/unshare()/pause()/storeInfo()/storeUsage()/
  pruneStore()/linkInfo(); unshare() and pause() share the private stopServing(); the link
  logging gate and the shared publish() helper.
- src/kelpshares.{h,cpp} — the shared rows on disk (owner-only shares.json), so a restart
  restores and re-serves them.
- src/kelpqr.{h,cpp} — the QML-painted QR item for a share link; src/qrcodegen.{hpp,cpp} is
  the vendored MIT QR encoder it calls, with its notice in src/qrcodegen.LICENSE.
- src/kelplog.{h,cpp} — dataDirectory(): the one place the data path is decided.
- src/kelpstore.h — KelpObjectStore (sharded ab/<64-hex>, temp+rename, re-hash on get),
  plus objectIds/objectSize/remove for the storage clean-up.
- src/kelptunnel.{h,cpp}, src/kelptls.{h,cpp} — the publisher's outbound tunnel + TLS 1.3,
  and the tunnel's own account of what it served (Traffic).
- relay/test/relaytest.cpp, run-relay-test.sh — the end-to-end relay check. It plays the
  recipient by hand and the publisher with the app's real KelpTunnel, so app framing and
  state get exercised against a real relay.
- libkelp/kelp_bundle.{h,cpp} — seal/open/missingObjects (the fetch walker) and
  bundleObjects (the storage walker: strict, lists every object a bundle is made of).
- libkelp/kelp_link.h, kelp_object.h, kelp_manifest.h, kelp_store.h, kelp_wire.{h,cpp}.
- libkelp/tests/tst_kelp.cpp — CHECK/CHECK_EQ style and the harness patterns to mirror.
- src/tests/ — app-layer harnesses to mirror for anything above libkelp: tst_kelpfetch
  (LAN header), tst_kelpsettings (persisted preference), tst_kelpshares (persisted rows),
  tst_kelpqr (QR encoding, pinned against Python's qrcode), each with its run-*-test.sh.
- relay/ — kelp-relay, a separate binary; relay/test/run-relay-test.sh is its harness.
- qml/ — harbour-kelp.qml (page stack, addBundle, and the share controls the pages
  share: pauseShare/resumeShare/pauseAllShares/resumeAllShares, plus a sharesRevision
  counter that makes pages re-read a row changed in place), pages/ (Bundles, Add, Bundle,
  OpenLink, Settings, About), cover/ (its play/pause action is a real pause-all/resume-all).

Important context — build, deploy, debug
- **META-INF/BUILD.md is the canonical build/test document.** Read it before
  building: the two targets, why switching target (or a defines change) needs a
  clean, the dev-extras bundle that separates a dev build from a release and how
  to make a release, every test suite with its expected count, and the build
  traps. Short form: build with sfdk -c target=SailfishOS-5.1.0.11-i486 build
  (emulator) or -aarch64 (phone), and run everything, both bundle modes, with
  build-shell sh "$PWD/src/tests/run-all-tests.sh".
- Deploy to the emulator: scp the RPM with workdir set to the project dir, then
  ssh ... 'sudo pkcon install-local -y ~/<rpm>'.
- Launch on the emulator needs the guest environment (XDG_RUNTIME_DIR=/run/user/100000,
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket,
  WAYLAND_DISPLAY=../../display/wayland-0, QT_QPA_PLATFORM=wayland) or the app exits at
  once. Prefix with sailjail -p harbour-kelp to reproduce sandbox behaviour.
- Log: ~/.local/share/harbour.kelp/harbour-kelp/kelp.log — <OrganizationName>/<ApplicationName>,
  i.e. exactly what Sailjail provisions, and the same path sandboxed or not. Rotates at
  512 KB. Read a link back out with: grep -o 'kelp://[^ ]*' <log> | tail -1
- KELP_DEBUG_LINK=1 writes the link to the log when a bundle is shared (dev builds only:
  a release build never writes a link, §3.18). Which build this is is in the log's first
  lines — `Kelp: build mode — dev extras on|off`.
  KELP_INITIAL_LINK=<kelp://...> (or the link as an argument) opens Open a link prefilled
  at startup.
- libkelp tests: sfdk -c target=SailfishOS-5.1.0.11-i486 build-shell sh libkelp/tests/run-tests.sh
- Emulator: SailfishOS-5.1.0.11-i486, ssh 127.0.0.1:2223 as defaultuser, key
  ~/SailfishOS/vmshare/ssh/private_keys/sdk; guest LAN address 10.0.2.15 behind QEMU NAT,
  so the host cannot connect inward (use an ssh -L forward). The guest CAN reach LAN hosts.
- Traps: ps | grep harbour-kelp inside an ssh '...' call matches the ssh command itself —
  ask /proc/<pid>/comm instead, and never pkill -f /usr/bin/harbour-kelp. The emulator has
  no clipboard a shell can reach, which is why the two environment variables exist.
- Test rig: either two devices (phone sender + emulator receiver, or the reverse), or one
  machine with two instances under different XDG_DATA_HOME so they have separate stores
  and logs and really fetch over the LAN. For emulator-as-sender, the emulator cannot be
  dialled into (QEMU NAT) — src/tests/run-emu-share.sh forwards a host port into it and
  rewrites the link (PROGRESS §4.5); it can also write QR PNGs of the rewritten link, and
  with --fetch it fetches from the share the way a recipient would and prints the
  publisher's log line for that traffic — the quick "is this share really reachable?"
  check before a phone is involved.
  The phone must be able to reach *this host*, so host and phone must share a network
  (the host's Wi-Fi was software-disabled; nmcli radio wifi on, then join the phone's).
- Through that forward the guest sees every peer as itself (10.0.2.15), because sshd inside
  the guest dials the target: a "who fetched" line made over the bridge always names
  10.0.2.15, whatever device really fetched. The bridge, not Kelp.
- Test traps: the listener is a QTcpServer in the same thread as the test, so a helper that
  blocks in waitForConnected/waitForReadyRead never gets accepted — drive the socket with
  signals and a QEventLoop (tst_kelpbundles httpGet). Sailfish's Qt is 5.6: no
  errorOccurred (5.15) and no QOverload (5.7) — cast the overloaded signal explicitly.
- Verify like this: object count in the store, the transfer lines in the log, sha256sum of
  the written file against the original, and diff -r for a tree.

Known gaps carried forward
- No TLS on the LAN path (deliberate). Shares ARE persisted now, but the address is still
  captured at share time and the port stays ephemeral: a restart remints the link (and a
  network change within a session is not noticed until the next restart). **Serving survives
  the window going away** now (KeepAlive, PROGRESS §3.17) but not the app being *closed*:
  there is no always-on service, which is why a per-share **pause** (persisted in
  shares.json) exists — it silences one bundle without deleting it. With no service a paused
  share and a running one still differ only while the app is up. There is no Wi-Fi-only,
  low-battery or sleep policy, and the *Share in background* switch is not persisted. No
  swarm, no relay caching or stewards, and nothing that puts a bundle in a browser —
  neither the app-hosted preview (design §5.1, designed but not built) nor the relay
  gateway (§5.2). **Parked, and not to be started (or designed further) before the app is
  otherwise finished and released: ephemeral rooms / anonymous chat** — a whole sibling
  protocol, sketched only as design Appendix B (`kelpchat://`, rooms in RAM, nothing
  stored anywhere). Sealing is synchronous on the UI thread. open() holds the whole tree in memory.
  Symlink skips are only logged. Some diagnostic printf lines remain. The storage clean-up
  runs on the UI thread too, and there is no size cap or LRU — it is a manual action. The
  "who fetched" line is written only after a peer has been quiet for five seconds (flushed
  by the sweeper that also expires half-open connections), so it lands within about ten
  seconds of a fetch; a fetch in the last seconds before the app is killed is not reported
  at all. The tunnel's line is the same idea but per tunnel, not per recipient — a relay
  multiplexes them, so the publisher cannot tell two recipients apart.

Open design questions are in kelp-design.md §12; they are still open.
