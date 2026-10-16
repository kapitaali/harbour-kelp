# Kelp

**Secure, minimal peer-to-peer file sharing for Sailfish OS.**

<p align="center"><img src="docs/icon-512.png" width="160" alt="Kelp icon"></p>

Share a folder or file from the phone as a `kelp://` link. A recipient pastes the
link into Kelp and it fetches the bundle. Content-addressed and encrypted by default,
with the key travelling in the link fragment — the relay and any peers only ever
see ciphertext.

Current state: **a bundle can be shared from the phone and opened on another
device — on the same Wi-Fi, or across networks through a relay.** `libkelp`
chunks, manifests, seals, hashes, verifies and reassembles bundles, with unit
tests and pinned vectors; the app uses it for real in both directions. Pressing
*Start sharing* seals every selected entry into on-device objects, starts a
listener on this device's LAN address and produces a `kelp://` link carrying that
address; pasting that link into *Open a link* on another device fetches the
objects, checks each one against its address, decrypts the bundle and writes the
files into *Downloads*. If a relay is configured, the same press also opens an
outbound TLS tunnel to it and puts its address in the link as `r=`, so a
recipient on a *different* network can fetch through it — the phone never has to
accept an inbound connection. Still missing: peers helping each other serve
(the swarm), relay caching/stewards, and a browser gateway.
See [`META-INF/kelp-design.md`](META-INF/kelp-design.md) for the design and
[`META-INF/kelp-swarm-wire-spec.md`](META-INF/kelp-swarm-wire-spec.md) for the
swarm wire format.

## Screens

- **Bundles** — list of shared bundles, each showing whether it is being served and
  the address it is served on. The list is remembered: sealed shares are restored
  and served again at the next launch. Pull down for *Add files or folder*,
  *Open a link*, *Settings*, *About*; long-press a row for *Open*, *Copy link*,
  *Stop sharing*.
- **Bundle** — details (files, size, location, object count, the address it is
  served on, encryption), the real `kelp://` link laid out in its three parts
  (identity · where · key), *Show QR code* — the whole link, key and all, as a
  scannable code for the other device — a note saying who can reach it, and
  *Stop sharing* — which stops serving, not just listing.
- **Add** — pick real files (each tap adds to the selection) or a whole folder.
  Sizes and file counts are read from disk; long-press a picked row to remove it.
  Nothing is sealed until *Start sharing*. Also carries the per-share *Let peers
  help serve this bundle* choice.
- **Open a link** — paste a `kelp://` link and download the bundle. The link is
  explained before anything happens (name, content address, where it will be
  fetched from), you pick the folder to save into, and the download reports
  progress and then where the files went.
- **Settings** — background seeding, Wi-Fi only, *Allow peer swarm* (a global
  permission), relay address, *Include my local address in the link*, *Include
  links in the log*, and a *Clear log* action. The first three switches are the
  shape of the intended behaviour and are deliberately not acted on yet — only
  the LAN path is wired up, and now in both directions. Two switches are real and
  persisted: *Include my local address in the link* decides whether a link carries
  this device's address at all (and so whether the bundle is served locally), and
  *Include links in the log* decides whether a share's `kelp://` link (and so its
  key) is written to the log. When the address *is* carried it is sealed, not
  spelled out — see *Serving on the LAN* below.
- **About** — version, build id, the log file path, the object store's path,
  object count and disk usage, and the address bundles are served on.
- **Cover** — bundle count and how many are actually being served; the play/pause
  action toggles *background seeding*, which is one of the placeholders and does
  nothing yet.

## Serving and privacy

There is no "public" vs "private" bundle, and no encryption toggle: **every bundle
is AES-256-GCM encrypted**, with the key in the link fragment, so a bundle is a
*capability* — readable only by whoever holds the link. Relays and peers see only
ciphertext.

What varies per bundle is **how it is served**:

- **Relay** — for anything wider than one network. The publisher dials out to a
  relay (if one is configured) and the recipient fetches through it, so peers never
  learn your IP address. This works today; see *Across networks* below.
- **Swarm** (opt-in per share) — not implemented yet. Other downloaders trade pieces directly with each
  other, offloading your connection for popular shares at the cost of exposing your
  IP to those peers. Available only when *Allow peer swarm* is on in Settings.

A swarm peer can help distribute a bundle it cannot decrypt, which is exactly why
swarm is a distribution choice rather than a visibility one. See
[`META-INF/kelp-design.md`](META-INF/kelp-design.md) §1.4 and §3.7.

## Serving on the LAN

Sharing a bundle starts a small HTTP listener on this device's LAN address — not on
every interface, so mobile data never opens a port by accident — and that address
goes into the link, **sealed** rather than spelled out: the link's `q=` block is the
`host:port` and the share's label under AES-256-GCM, keyed by `HKDF(secret,
"kelp/v1/link")`. Anyone who holds the link holds the fragment and so can open it;
anything that sees the link without the fragment — a redacted paste, a chat preview,
a log — learns neither. Two devices on one Wi-Fi are therefore the first thing that
works:

```
GET /v1/bundle/<bundle_id>/object/<object_id>    → the object's bytes
GET /v1/bundle/<bundle_id>/info                  → { objects, files, bytes }
Authorization: Bearer <swarm_token>
```

The bearer token is *derived* from the link's secret (HKDF), not carried in the
link: holding the link **is** the credential. Objects are served only while their
bundle is being shared, and a bundle that is not shared answers exactly like a
wrong token, so the endpoint cannot be used to enumerate what a device is
offering. What comes back is ciphertext, and the receiver checks every object
against its address, so a server can withhold but cannot substitute.

There is deliberately no TLS on this path yet. Confidentiality does not depend on
the transport, and a `curl`-able endpoint is what makes bring-up possible; the
cost is that a sniffer on the same Wi-Fi learns the token — and so the sender's
address and availability — but not the plaintext, because the content key and the
swarm token are separate HKDF outputs of the same secret. The swarm transport,
where peer addresses are handed around, keeps TLS. See wire spec §7.2.

Because a link is handed to whoever it is sent to, the address in it reaches all of
them — normally a private `192.168.x.x`, but a cellular bearer is a real IP. Sealing
keeps it out of the link *text*, but the recipient must still learn it to connect, so
for a share that must not disclose where it is served from at all there is the switch:
*Settings → Include my local address in the link* turns the whole path off — nothing
is served locally, the link carries no address, and only a relay can carry the bundle.

Sharing is remembered. Each row — name, size, object counts, the bundle id and the
secret — is written to a small owner-only file (`shares.json`, mode `0600`) beside
the log, and the next launch serves those bundles again instead of silently
forgetting them. The *address* is deliberately not stored: the LAN port is
ephemeral, so a fresh one is chosen at startup and the link is minted again, which
is why the app always shows the current link rather than one captured earlier.
Storing the secret asks for no more trust than the link already does — it is the
same key — but the file is kept owner-only and is never logged.

## Opening a link

*Open a link* is the other half of the LAN path: it takes a `kelp://` link, asks
the address inside it for what the bundle needs, and writes the files out.

The link is explained before anything is fetched — the name, the content address
and the address it will be fetched from all come out of the link itself, so the
preview works with no network at all. Downloading then goes in rounds, and the
rounds are the interesting part:

1. ask the store what is still missing for this bundle — to begin with that is
   only the root manifest, because everything else is *inside* it;
2. fetch those objects, four at a time, and keep each one only if it hashes to
   the address it was asked for;
3. go back to step 1, which can now see one level deeper.

When nothing is missing any more the bundle is opened and written into the chosen
folder — **Downloads** by default, or wherever the *Save to → Choose folder…*
picker on the page was last pointed, a choice that is remembered across
restarts. A bundle holding a single file becomes that file; anything else
becomes a folder named after the link. Every name is checked again on the way
out, so a hostile manifest cannot write outside that folder, and nothing existing
is ever overwritten — a second copy becomes "(2)".

Both halves of that walk live in `libkelp` (`missingObjects` and `storeReceived`),
where they are unit-tested; the app side is only the network and the bookkeeping
the UI binds to. What a peer cannot do is substitute content: an object is
verified against its address before it is stored, and a link whose key is not the
sender's is reported as such instead of being retried forever.

A fetch that cannot get through says which wall it hit. An address that accepts
the connection but never answers is reported as *could not reach the sender*
after a few seconds; only a sender that has answered *and then* gone quiet is
called out as *stopped answering*. The two are different faults, and the first
is usually a network path that is not there at all — for instance an address
behind a NAT with no way in.

When the link also names a relay (`r=`), the relay is used instead — see below —
which is what makes a link work off the local network.

Not there yet: peers helping serve the same bundle to each other (the swarm),
relay-side caching so a link outlives the phone, and any streaming — the whole
tree is held in memory before it is written, so a bundle far larger than the
device's memory would not fit.

## Across networks: the relay

Two devices on one Wi-Fi need nothing but each other. Anything wider needs a third
party, because a phone behind carrier NAT cannot accept a connection — so Kelp
inverts it: the phone *dials out* to a relay and keeps that tunnel open, and a
recipient fetches from the relay.

The relay is untrusted by construction. It is given a credential of its own
(`relay_token` — a third derivation from the link secret, not the content key and
not the swarm token) and it carries the same ciphertext a LAN peer would, so it
can withhold an object but never substitute one: the recipient checks every object
against its address.

Run one with the small `kelp-relay` program (`relay/`, built separately from the
app):

```sh
kelp-relay --port 9443 --cert cert.pem --key key.pem
```

One port, TLS 1.3 with ALPN `"kelp/1"`, serving both publisher tunnels and
recipient fetches. Point the app's Settings at `host:port` and every new share
publishes there and carries it in the link as `r=`. A relay is a few hundred lines
and self-hosted; nothing about a link requires any particular one.

## The content layer (`libkelp`)

`libkelp/` is the content layer: canonical CBOR (RFC 8949 deterministic encoding),
AES-256-GCM content objects, the file and directory manifest schemas, and the
bundle build/open path. It is pure computation — no I/O, no UI, no network — and
depends only on `QtCore` and OpenSSL's `libcrypto`, both of which are on every
Sailfish device.

```
libkelp/
  kelp_cbor.{h,cpp}      canonical CBOR, strict decode (rejects non-canonical)
  kelp_crypto.{h,cpp}    SHA-256, HKDF-SHA256, AES-256-GCM (libcrypto)
  kelp_object.{h,cpp}    the object envelope and its content address
  kelp_manifest.{h,cpp}  file/directory manifests, segmentation, NFC names
  kelp_store.{h,cpp}     the object-store interface, plus an in-memory store
  kelp_bundle.{h,cpp}    seal a tree, open a tree, plan a fetch, store what
                         arrives, derive the keys
  kelp_link.{h,cpp}      encode/decode a kelp:// link, base32 and base64url
  tests/                 unit tests with pinned vectors
```

`libkelp.pri` is the include point for the app. The app code on top of it:

```
src/
  kelpstore.{h,cpp}      KelpObjectStore: objects on disk, sharded by id, written
                         atomically; KelpFileSource: reads files for the sealer
  kelpbundles.{h,cpp}    KelpBundles, the QML-facing seal()/storeInfo(), and the
                         publisher's relay tunnels
  kelpserver.{h,cpp}     KelpServer, the LAN listener: serves objects to whoever
                         holds the link (wire spec §7.2)
  kelpshares.{h,cpp}     KelpShares: the shared-bundle rows on disk, so a restart
                         restores and re-serves them (owner-only JSON)
  kelpfetch.{h,cpp}      KelpSource and its two implementations: the LAN fast path
                         and the relay tunnel, behind one interface
  kelpclient.{h,cpp}     KelpClient, the other end: fetches a link, verifies what
                         arrives, writes the files into Downloads
  kelptunnel.{h,cpp}     KelpTunnel, the publisher's outbound tunnel to a relay
  kelptls.{h,cpp}        KelpTls, the TLS 1.3 + ALPN "kelp/1" session the tunnel
                         is built on (OpenSSL 3, because Qt 5.6 cannot)
```

The relay itself is a separate program in `relay/`, not part of the app package.

`KelpBundles::seal(paths, name)` is what *Start sharing* calls: it snapshots the
paths into a `SourceNode` tree (counting files, skipping symlinks), picks a fresh
random master secret, seals into the store, publishes the bundle on the LAN and
returns the link, bundle id, key, object count, file count, total bytes and the
address it is being served on. Sealing is synchronous on the UI thread, so a large
folder freezes the window while it works — a worker thread is a known TODO.
Objects live in `<app data>/objects/<first id byte>/<id>`, next to the log; they
are never deleted yet, and re-sharing the same folder writes new objects because
every seal uses fresh nonces.

`KelpBundles::republish(bundleId, secret, name, objects, files, bytes)` is the
restore path: it serves a bundle again from the saved row — the objects are
already in the store, and the link is minted afresh because the LAN port is
ephemeral. `seal()` and `republish()` share one `publish()` helper, so a freshly
sealed bundle and a restored one get the same LAN address and relay treatment.

Serving is one listener for the whole app, started the first time anything is
shared and told which bundles are currently published; `unshare()` (called by
*Stop sharing*) takes one back out without disturbing the others. It is
content-agnostic: it reads objects out of the store and never decrypts anything.

Fetching uses that same store, so an object that arrived over the network is
indistinguishable from one that was sealed here — which is what will let a device
serve again what it downloaded, once there is a way to say so in the UI.

Publish posture and encoding details are in
[`META-INF/kelp-design.md`](META-INF/kelp-design.md) §2 and §3.1; the exact
schemas, the `kelp://` link layout and the pinned vectors are in
[`META-INF/kelp-swarm-wire-spec.md`](META-INF/kelp-swarm-wire-spec.md).

## Debugging

Every Qt/QML message and every explicit `kelpLog.*` call lands in one file:

```
~/.local/share/harbour.kelp/harbour-kelp/kelp.log
```

That is `<OrganizationName>/<ApplicationName>` from the desktop file, i.e. exactly
the directory Sailjail provisions for the app — the same path whether it is
launched from the app grid (sandboxed) or from a shell. Objects live in `objects/`
beside it. (It used to be `~/.local/share/harbour-kelp`, which a sandboxed app
cannot create, so a fresh install could not write its log or its store at all; the
app moves an old directory into the new place once, if it finds one.)

`KelpLog::install()` (called first in `main()`) routes `qDebug`/`qWarning`,
QML `console.log(...)` and QML binding errors into it, so nothing has to be
re-enabled before investigating a problem. The path is shown on the **About**
page and **Settings** has a *Clear log* action. Over ssh:

```sh
ssh defaultuser@<host> 'tail -f ~/.local/share/harbour.kelp/harbour-kelp/kelp.log'
```

From QML, use the `kelpLog` context property for structured milestones:

```qml
kelpLog.info("added " + path + " (" + fileCount + " files)")
```

The file rotates to `kelp.log.1` once it passes 512 KB.

There are two ways to get a `kelp://` link written into the log, which is what a
device with no usable clipboard — a headless box, the emulator over ssh — needs:

- **Settings → *Include links in the log*** — a persisted switch, off by default.
  When on, every share's link is written to the log and can be read back with
  `grep`. A link holds the decryption key, and the log is a plain file, so the
  switch is off unless the user asks for it.
- `KELP_DEBUG_LINK=1` in the app's environment — the same effect for a single
  run, without touching the UI. Kept so existing headless scripts keep working.

`KELP_INITIAL_LINK=<kelp://…>` (or passing the link as an argument) opens that
link in *Open a link* at startup. This is how a link is handed *to* a device
that cannot paste into it, and it is the same seam a `kelp://` activation will
use later.

`KELP_ADVERTISE_HOST=<address>` makes a link name that address instead of the one
the listener bound. It is for a device reachable only through another one — the
emulator behind QEMU's NAT, bridged by `src/tests/run-emu-share.sh` — where the
address that works is the *host's*, not the guest's. Only the address written into
the link changes; the socket still binds the real interface.

Both write the link to the log; read it back with:

```sh
grep -o 'kelp://[^"]*' ~/.local/share/harbour.kelp/harbour-kelp/kelp.log | tail -1
```

## Testing `libkelp`

The unit tests are a plain qmake project, so they need the target's Qt and
OpenSSL; run them inside the build engine:

```sh
sfdk -c target=SailfishOS-5.1.0.11-i486 \
    build-shell sh "$PWD/libkelp/tests/run-tests.sh"
```

They pin the RFC 5869 and GCM specification vectors, the manifest and object
vectors from the wire spec §16, and the whole bundle round-trip: chunk →
manifest → hash → verify → decrypt, plus a corrupt-object case (to prove the
content-address check is load-bearing), a segment-index case, and the receiver's
side — walking a bundle that is only half here, asking for exactly what is
missing, and refusing an object that does not hash to the address it was fetched
for.

The app layer above `libkelp` has the same kind of harnesses under `src/tests/`,
each with a `run-*-test.sh` that builds out of source: `tst_kelpfetch` (the LAN
request header), `tst_kelpsettings` (the persisted preference), `tst_kelpshares`
(the persisted rows) and `tst_kelpqr` (QR encoding, pinned against Python's
`qrcode`). Run them the same way, e.g.
`build-shell sh "$PWD/src/tests/run-kelpqr-test.sh"`.

## Building

qmake-based Sailfish OS project, built with
[sfdk](https://sailfishos.github.io/sfdk/) or the Sailfish IDE:

```sh
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build
```

## Permissions

The app requests `UserDirs` (see `harbour-kelp.desktop`) so the pickers can read
your files, the Add page can measure them, and a download can be written into
*Downloads*, and `Internet` because sharing serves objects over the network and
opening a link fetches them. `Internet` is the permission that lifts the network
sandbox (`protocol inet,inet6,netlink`); there is no separate local-network
permission on Sailfish. The first launch after install therefore shows the usual
one-time permission prompt.

The file and folder pickers come from `Sailfish.Pickers`
(`sailfish-components-pickers-qt5`). Like the sibling apps, that package is
**not** declared as a dependency: the Harbour validator rejects the name and it
is a base-image package, so it is always present.

## License

`GPL-3.0-only`.

The QR encoding on the Bundle page uses Project Nayuki's QR Code generator
library (MIT), vendored in `src/qrcodegen.*`. Its license notice is shipped in
`src/qrcodegen.LICENSE` and installed alongside the app; MIT is compatible with
the GPL-3.0-only rest of the tree.
