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
- META-INF/PROGRESS-20261016.md — the newest session record: the three bugs with root
  causes, the exact evidence for what was verified, build/deploy/launch recipes, and a
  traps list where every entry cost the previous agent time. Read this first.
- META-INF/kelp-design.md — the design and the milestone list (§3.1 records the deliberate
  LAN deviations, §12 the open questions).
- META-INF/kelp-swarm-wire-spec.md — the wire format; §5 the link, §7.2 direct fetch,
  §16 pinned vectors.
- README.md — current state, screens, how to debug.
- META-INF/ipfs-fileshare-PLAN.md — parked, separate name. Leave it as-is.
- The repo is NOT a git repository: no history, no commits, changes are plain file edits.
  Keep the dated PROGRESS-<date>.md notes up to date as you go; they are the history.

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
  CBOR manifests, AES-256-GCM, key in the URL fragment. Identity hashed, location and key
  not: bundle_id = SHA-256 of the root manifest, addresses as l=<host:port> / r=<relay> in
  the query. HKDF-SHA256, salt = 32 zero bytes, info strings kelp/v1/content,
  kelp/v1/swarm, kelp/v1/relay.
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
- libkelp: 304 checks, 0 failures. Relay harness: 4/4 ALL PASS.
- Phone RPM with all three fixes: RPMS/SailfishOS-5.1.0.11-aarch64/harbour-kelp-0.1.0-1.aarch64.rpm.
  Emulator RPM: RPMS/SailfishOS-5.1.0.11-i486/harbour-kelp-0.1.0-1.i486.rpm. There is no
  git commit for any of it.

Next Move (in this order)
1. Regression test for the LAN request header. src/kelpfetch.cpp's LanSource puts the
   swarm token in Authorization: Bearer <token>, and the sender base64url-decodes that
   header and compares bytes. It was handed raw bytes for a while, which the sender
   silently answered 401 ("the sender did not recognise this link"). Nothing tests it.
   Pin the format with a test that asserts the header equals base64url(swarmToken(secret)).
2. Settings -> "Include links in the log", persisted, off by default. Today the link is
   only written to kelp.log when the app is launched with KELP_DEBUG_LINK=1, which makes
   the emulator (no usable clipboard) need a shell launch to get a link at all.
3. Make a share survive reality: shared rows are not persisted, so a restart silently
   drops the share; and the link's address is captured at share time, so a network change
   makes it stale with no indication to the user.
4. Store management: nothing ever deletes objects and the store now also grows from what
   others send. Show its size; give bundles a Remove (needs the object graph walked
   backwards) and/or an LRU. See kelp-design.md §12.7.
5. Only then C4 (browser viewer + gateway) or C7 (swarm).
Also worth doing opportunistically: replace the remaining printf/fprintf(stderr) debug
lines with KelpLog calls, so everything lands in the log file.

Relevant files
- src/kelpfetch.{h,cpp} — KelpSource, LanSource, RelaySource. Bug 1 lives here (create()).
- src/kelpclient.{h,cpp} — the fetch loop: walk, ask, fetch, repeat; writes the tree.
- src/kelpserver.{h,cpp} — the LAN listener the client talks to (endpoints, auth, guards).
- src/kelpbundles.{h,cpp} — seal()/unshare()/storeInfo()/linkInfo(); the link logging gate.
- src/kelplog.{h,cpp} — dataDirectory(): the one place the data path is decided.
- src/kelpstore.h — KelpObjectStore (sharded ab/<64-hex>, temp+rename, re-hash on get).
- src/kelptunnel.{h,cpp}, src/kelptls.{h,cpp} — the publisher's outbound tunnel + TLS 1.3.
- libkelp/kelp_bundle.{h,cpp} — seal/open/missingObjects (the walker).
- libkelp/kelp_link.h, kelp_object.h, kelp_manifest.h, kelp_store.h, kelp_wire.{h,cpp}.
- libkelp/tests/tst_kelp.cpp — CHECK/CHECK_EQ style and the harness patterns to mirror.
- relay/ — kelp-relay, a separate binary; relay/test/run-relay-test.sh is its harness.
- qml/ — harbour-kelp.qml (page stack, addBundle) and pages/ (Bundles, Add, Bundle,
  OpenLink, Settings, About).

Important context — build, deploy, debug
- Build: sfdk -c target=SailfishOS-5.1.0.11-i486 build (emulator) or
  target=SailfishOS-5.1.0.11-aarch64 (phone). It compiles in place, so switching target
  needs a clean first: rm -f harbour-kelp *.o moc_*.cpp Makefile .qmake.stash. The
  libcrypto.so.1.1 vs .3 linker warning is benign and explained in kelp-design.md.
- Deploy to the emulator: scp the RPM with workdir set to the project dir, then
  ssh ... 'sudo pkcon install-local -y ~/<rpm>'.
- Launch on the emulator needs the guest environment (XDG_RUNTIME_DIR=/run/user/100000,
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket,
  WAYLAND_DISPLAY=../../display/wayland-0, QT_QPA_PLATFORM=wayland) or the app exits at
  once. Prefix with sailjail -p harbour-kelp to reproduce sandbox behaviour.
- Log: ~/.local/share/harbour.kelp/harbour-kelp/kelp.log — <OrganizationName>/<ApplicationName>,
  i.e. exactly what Sailjail provisions, and the same path sandboxed or not. Rotates at
  512 KB. Read a link back out with: grep -o 'kelp://[^ ]*' <log> | tail -1
- KELP_DEBUG_LINK=1 writes the link to the log when a bundle is shared.
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
  and logs and really fetch over the LAN.
- Verify like this: object count in the store, the transfer lines in the log, sha256sum of
  the written file against the original, and diff -r for a tree.

Known gaps carried forward
- No TLS on the LAN path (deliberate). Shares are not persisted; the address is captured
  at share time; the port is ephemeral. Serving stops when the app exits. No object GC. No
  swarm, no relay caching or stewards, no browser gateway. Sealing is synchronous on the UI
  thread. open() holds the whole tree in memory. Symlink skips are only logged. Some
  diagnostic printf lines remain.

Open design questions are in kelp-design.md §12; they are still open.
