# Kelp

**Secure, minimal peer-to-peer file sharing for Sailfish OS.**

<p align="center"><img src="docs/icon-512.png" width="160" alt="Kelp icon"></p>

Share a folder or file from the phone as a `kelp://` link. A recipient pastes the
link into Kelp and it fetches the bundle. Content-addressed and encrypted by default,
with the key travelling in the link fragment — the relay and any peers only ever
see ciphertext. A link is a *capability*, so it is as secret as its key; when that is
awkward — a link pasted into a group chat, or written on a whiteboard — *Require a
passphrase* mints one whose key is wrapped under a passphrase the sender gives
separately, and the link alone opens nothing at all.

Current state: **a bundle can be shared from the phone and opened on another
device — on the same Wi-Fi, or across networks through a relay.** `libkelp`
chunks, manifests, seals, hashes, verifies and reassembles bundles, with unit
tests and pinned vectors; the app uses it for real in both directions. *Share…*
offers a file, a folder, or a webpage — the folder holding a site — and choosing
one seals it into on-device objects, starts a listener on this device's LAN
address and produces a `kelp://` link carrying that address; pasting that link
into *Open a link* on another device fetches the objects, checks each one against
its address, decrypts the bundle and writes the files into *Downloads*. A webpage
share also *says* it is a page, inside the link and under the key, so the link
text still reveals nothing — that hint is what will let a recipient open the page
in a browser and keep nothing (design §5.5); the receiving half of that is not
built yet. If a relay is configured, the same press also opens an outbound TLS
tunnel to it and puts its address in the link as `r=`, so a recipient on a
*different* network can fetch through it — the phone never has to accept an
inbound connection. Still missing: peers helping each other serve (the swarm),
relay caching/stewards, the relay gateway that serves a recipient who has no app
at all (§5.2), and the receiving half of webpage shares. Also sketched there, and
deliberately *not* planned before a release: anonymous ephemeral chat
(Appendix B) — parked, not a milestone.
See [`META-INF/kelp-design.md`](META-INF/kelp-design.md) for the design,
[`META-INF/kelp-swarm-wire-spec.md`](META-INF/kelp-swarm-wire-spec.md) for the
swarm wire format, and [`META-INF/BUILD.md`](META-INF/BUILD.md) for how to build
and test.

## Screens

- **Bundles** — list of shared bundles, each showing whether it is being served and
  the address it is served on. The list is remembered: sealed shares are restored
  and served again at the next launch. Pull down for *Share…*,
  *Open a link*, *Settings*, *About*; long-press a row for *Open*, *Copy link*,
  *Pause sharing* (or *Resume sharing* on a paused row), *Stop sharing*.
- **Bundle** — details (files, size, location, object count, the address it is
  served on, encryption), the real `kelp://` link laid out in its three parts
  (identity · where · key), *Show QR code* — the whole link, key and all, as a
  scannable code for the other device — a note saying who can reach it,
  *Pause sharing* (silence it without forgetting it; resuming mints a fresh
  link), and *Stop sharing* — which stops serving **and** deletes the row.
- **Share** — three buttons, one kind each: a file, a folder, or a webpage (the
  folder that holds a site). One choice seals one thing into one bundle with its
  own link, so sharing three files means choosing three times. *Share webpage…*
  first checks the chosen folder really holds a page — by the same rule that finds
  a page in a bundle (design §5.3) — and refuses a folder that holds none instead
  of sharing something a recipient could not open. (The repo's `demo/` folder is a page made to
  be shared this way: one small offline site, three files.) Only a share declared as a **webpage**
  is offered to a browser; a folder shared as a folder is files and nothing else, and a page among
  them is the recipient's own business to open (design §5.4/§5.5). Also carries the per-share *Let
  peers help serve what I share here* choice, and a **Protect the link** section at the top: turn on
  *Require a passphrase*, type one (at least eight characters) and every share made from this page
  is minted with its key wrapped under that passphrase instead of carried in the link. The link
  then names no address, no label and no kind until it is unlocked, and the passphrase is never
  stored — the row keeps only the wrapped key, which is enough to mint the same link again after a
  restart. Forget it and the link is locked for good; there is no recovery.
- **Open a link** — paste a `kelp://` link and download the bundle. The link is
  explained before anything happens (name, content address, where it will be
  fetched from), you pick the folder to save into, and the download reports
  progress and then where the files went. A link tapped in another app (a chat, a
  mail) arrives on this same page prefilled, and is just as inert until the button
  is pressed — design §2.1.
  A link that needs a **passphrase** (the one a sender minted with *Require a
  passphrase*) shows a masked field and an *Unlock* button instead of that
  explanation: the name, the address and the kind are all under the key the
  passphrase unwraps, so there is nothing to say until it is given. Checking it
  takes a moment on purpose — that delay is what makes guessing hopeless — and a
  wrong passphrase costs nothing but the delay, because the sender is never
  dialled until it is right.
  A link that says it is a **webpage** (design §5.5) behaves differently, because
  its publisher asked for it to be looked at rather than kept: there is no folder
  to pick and no *Download*, just **Open in browser**. That one button fetches the
  bundle into memory — never to the object store, never to a file — serves the
  page on `127.0.0.1`, and hands that URL to the browser. Nothing is left behind:
  the copy is gone when the preview session ends (an idle sweep after about 30
  minutes). Pressing the button again re-opens that same session; if it has since
  ended, the page is still in memory, so a fresh session is minted and what the
  browser gets always answers. A finished view belongs to the link that asked for it,
  so a *second* link is fetched as its own page rather than being shown the first
  one's. The page is served **offline**: it can use its own files and
  its own inline CSS and scripts, but nothing from another host, so a page that expects
  something from a CDN renders without it (design §12.11). A page larger than 32 MiB is
  refused before anything is transferred, and says so.
- **Settings** — background seeding, Wi-Fi only, *Allow peer swarm* (a global
  permission), relay address, *Include my local address in the link*, **Storage**
  (what the object store holds, what the shares need, and a *Remove unused
  objects* action), *Include links in the log*, and a *Clear log* action. *Share
  in background* is real: it decides whether Kelp asks the platform not to end it
  while it serves with the window gone (see *Serving with the window gone* below);
  closing the app still ends serving. *Wi-Fi only* is real too: it **never seeds
  over mobile data** — serving stops when the device's only usable connection is
  cellular and starts again by itself when a Wi-Fi or wired interface is back.
  Serving also stops below 15% battery while not charging, for the same reason
  (the battery is not a switch; the rule is design §12.8). A policy stop is not a
  pause the user asked for: the row says why, the link is kept, and the stop is
  forgotten once the condition lifts. *Allow peer swarm* is the remaining parked
  switch: it is remembered now, but only the LAN path is wired up, in both
  directions. Two more switches are real and persisted: *Include my local address
  in the link* decides whether a link carries
  this device's address at all (and so whether the bundle is served locally), and
  *Include links in the log* decides whether a share's `kelp://` link (and so its
  key) is written to the log — part of the dev-extras bundle, so a dev build offers
  it and has it on by default, while a release build never logs a link and does not
  offer the switch (see *Development builds* below). When the address *is* carried
  it is sealed, not spelled out — see *Serving on the LAN* below.
- **About** — version, build id, the log file path, the object store's path,
  object count and disk usage, and the address bundles are served on.
- **Cover** — bundle count and how many are actually being served; the play/pause
  action pauses *every* share for real, or resumes them all (the same per-share
  pause/resume the list and Bundle page offer), and the icon follows the store's
  own served/paused counts rather than guessing.

## Serving and privacy

There is no "public" vs "private" bundle, and no encryption toggle: **every bundle
is AES-256-GCM encrypted**, with the key in the link fragment, so a bundle is a
*capability* — readable only by whoever holds the link. Relays and peers see only
ciphertext.

A **passphrase link** is the same capability with its key wrapped under a
passphrase (`kelp://…#p=<wrapped key>`, design §12.3): the link text names nothing
— not the address, not the label, not the kind — until someone has both halves.
It is not a weaker *encryption* setting (there is no such setting); it is a way to
hand over the capability in two pieces. PBKDF2-HMAC-SHA256 at 600 000 iterations is
what stands between a stolen link and a guess, so it is only as strong as the
passphrase: it solves the *distribution* problem, not the *weak passphrase*
problem. The work factor is carried inside the link, so it can be raised later
without breaking links already out there.

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

## Serving with the window gone

Serving does not stop when Kelp leaves the foreground. Sailfish keeps a
backgrounded app genuinely running — the cover view is a live process, not a saved
state — so a share sitting on the cover is still reachable, and a recipient
fetching then gets exactly what a fetch in the foreground gets. Verified with the
app on its cover (PROGRESS §3.17).

*Settings → Share in background* makes that deliberate rather than incidental:
while something is really being served and the switch is on, Kelp holds the
platform's `Nemo.KeepAlive` request — how an app asks not to be ended under memory
or power pressure — and with nothing to serve it releases the request rather than
sitting on it. The switch is persisted — a user who turns background serving
off does not get it back at the next launch — and comes on by default.

A **preview session** holds that request on its own account, with or without the
switch: a webpage link is opened *from* the app into the browser, so the party in
the foreground is the browser, not Kelp, and the app must not be ended while it is
still serving the page. (The log tells the two apart — `KeepAlive: background
serving on (setting off, 1 served, 1 previewing)` is a preview holding it by
itself.)

Two limits are real and worth saying plainly:

- **Closing Kelp ends serving.** Serving is the app's own listeners, and there is
  no always-on service behind them — a Harbour app cannot ship one (that would be
  a systemd user service). The old wording promised "keep seeding after the app is
  closed", which was never true.
- **Serving stops on mobile data and on a low battery, and comes back by
  itself.** *Wi-Fi only* means *never seed over mobile data*: while the only
  usable connection is cellular, serving pauses and says why. Below 15% while
  not charging, it pauses for the same reason. Neither is a *user* pause — the
  link is kept, and the stop is forgotten once the condition lifts, so a link
  already handed out works again untouched (design §12.8).

To silence a share without giving up its place in the list, *Pause sharing* from a
row's menu — or the cover action, which pauses them all — stops serving while
keeping the row and the objects; *Resume sharing* serves it again, with a freshly
minted link.

## Opening a link

*Open a link* is the other half of the LAN path: it takes a `kelp://` link, asks
the address inside it for what the bundle needs, and writes the files out.

The link is explained before anything is fetched — the name, the content address
and the address it will be fetched from all come out of the link itself, so the
preview works with no network at all. (For a passphrase link the name and the
address come out of the same link once the passphrase has unwrapped them — still
with no network, and still before anything is asked of the sender.) Downloading then goes in rounds, and the
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

A link that declares itself a **webpage** ends this walk differently: the objects
go to a memory store of that view's own instead of the app's object store, nothing
is written out, and the page is served to this device's browser through the same
loopback preview a share uses. See *Open a link* under *Screens*.

Not there yet: peers helping serve the same bundle to each other (the swarm),
relay-side caching so a link outlives the phone, and any streaming — a save holds
the bundle's objects on disk and then writes the files, so a bundle far larger
than the device's storage would not fit, and a *view* holds its whole page in
memory and so is capped at 32 MiB on purpose.

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
  kelp_crypto.{h,cpp}    SHA-256, HKDF-SHA256, PBKDF2-HMAC-SHA256, AES-256-GCM (libcrypto)
  kelp_object.{h,cpp}    the object envelope and its content address
  kelp_manifest.{h,cpp}  file/directory manifests, segmentation, NFC names
  kelp_store.{h,cpp}     the object-store interface, plus an in-memory store
  kelp_bundle.{h,cpp}    seal a tree, open a tree, plan a fetch, list every object
                         a bundle is made of, store what arrives, derive the keys
  kelp_link.{h,cpp}      encode/decode a kelp:// link, base32 and base64url, and the
                         passphrase form: wrap/unwrap the key under a passphrase
  kelp_web.{h,cpp}       the page convention: which file in a bundle is its page,
                         how a URL path maps onto it, MIME types, byte ranges
  tests/                 unit tests with pinned vectors
```

`libkelp.pri` is the include point for the app. The app code on top of it:

```
src/
  kelpstore.{h,cpp}      KelpObjectStore: objects on disk, sharded by id, written
                         atomically, and removable; KelpFileSource: reads files
                         for the sealer
  kelpbundles.{h,cpp}    KelpBundles, the QML-facing seal()/storeInfo()/storeUsage()/
                         pruneStore(), and the publisher's relay tunnels
  kelpserver.{h,cpp}     KelpServer, the LAN listener: serves objects to whoever
                         holds the link (wire spec §7.2)
  kelpshares.{h,cpp}     KelpShares: the shared-bundle rows on disk, so a restart
                         restores and re-serves them (owner-only JSON)
  kelpfiles.{h,cpp}      KelpFiles: which file in a folder is its page (the same
                         rule the preview uses), and sizes in words
  kelphandoff.{h,cpp}    KelpHandoff: the kelp:// scheme's door — validate the URL,
                         hand it to the running instance over a 0600 socket, or
                         become the instance (design §2.1)
  kelpfetch.{h,cpp}      KelpSource and its two implementations: the LAN fast path
                         and the relay tunnel, behind one interface
  kelpclient.{h,cpp}     KelpClient, the other end: fetches a link, verifies what
                         arrives, writes the files into Downloads
  kelptunnel.{h,cpp}     KelpTunnel, the publisher's outbound tunnel to a relay
  kelptls.{h,cpp}        KelpTls, the TLS 1.3 + ALPN "kelp/1" session the tunnel
                         is built on (OpenSSL 3, because Qt 5.6 cannot)
```

`demo/` holds a small self-contained web page — `index.html`, `style.css`, `kelp.svg` — to
share as a webpage share when showing Kelp to someone. It has no external reference at all
(no CDN, no webfont, no image from anywhere), which is what a page shared this way has to be:
the preview serves a received bundle offline. Its three files are the point as well —
relative sub-resources resolve through the bundle's own objects.

The relay itself is a separate program in `relay/`, not part of the app package.

`KelpBundles::seal(paths, name, page, passphrase)` is what the *Share* page calls: it
snapshots the paths into a `SourceNode` tree (counting files, skipping symlinks),
picks a fresh random master secret, seals into the store, publishes the bundle on
the LAN and returns the link, bundle id, key, object count, file count, total
bytes, the address it is being served on, whether it was declared a webpage
(`page`) and — for a passphrase share — the wrapped key as `passBlob`. An empty
`passphrase` is the ordinary link; a non-empty one (eight characters or more, or
`seal` refuses before sealing anything) is wrapped under a `PBKDF2-HMAC-SHA256`
key with the bundle id as associated data, so the link carries `#p=` and the
wrapped blob (`passBlob`) is what the row keeps instead of the passphrase. The folder check that
decides whether a folder *can* be declared a page is
`KelpFiles::pageEntry`, which applies the same `Kelp::Web::chooseEntry` rule the
preview does, so the two cannot disagree. Sealing is synchronous on the UI thread,
so a large folder freezes the window while it works — and so does a passphrase
derivation, which is deliberately slow; a worker thread is a known
TODO. Objects live in `<app data>/objects/<first id byte>/<id>`, next to the log.
Re-sharing the same folder writes new objects because every seal uses fresh
nonces.

`KelpBundles::republish(bundleId, secret, name, objects, files, bytes, page, passBlob)`
is the restore path: it serves a bundle again from the saved row — the objects are
already in the store, and the link is minted afresh because the LAN port is
ephemeral. The `page` flag comes from the row, so a restored webpage share is
still a webpage share, and `passBlob` comes from the row too, so a restored
passphrase share is still opened by the passphrase its sender gave out (a stored
`passBlob` that cannot be read is refused rather than published as an ordinary
link — failing open would hand out the key). `seal()` and `republish()` share one
`publish()` helper, so a freshly sealed bundle and a restored one get the same LAN
address and relay treatment. `KelpBundles::linkInfo()` and `unlockLink()` are the
reading side of both link forms, and the unlock is what `KelpClient` does before it
creates a source (`fetch(link, passphrase)`), so a wrong passphrase never costs a
network round trip.

Nothing is deleted automatically: seals and fetches both add objects, so Settings →
**Storage** shows what is there and offers **Remove unused objects**. *Unused* has
to be decided globally, because the store is one deduplicated pool and the graph
points down (an object does not know which manifests name it). `Kelp::bundleObjects`
walks every share's object graph in full to build the "in use" set; `KelpBundles::storeUsage`
and `pruneStore` turn that into the display and the deletion, taking the share rows
from the UI so "in use" is exactly what this device is serving. The walk is strict —
a manifest that will not open fails the whole call, and nothing is deleted — because
an object that cannot be accounted for must never be treated as junk. What the walk
does *not* cover is a download's cached objects: those belong to no share here, and
deleting them costs a re-download, not the content, since downloads are kept as
ordinary files in *Downloads*.

Serving is one listener for the whole app, started the first time anything is
shared and told which bundles are currently published; `unshare()` (called by
*Stop sharing*, and by *Pause sharing* through `pause()`) takes one back out
without disturbing the others. **Pause** stops serving but keeps the row and the
objects, and records that in `shares.json`, so a silenced share stays silent
until it is resumed — resuming is `republish()`, the same call that restores a
share at startup, and it mints a fresh link. It is
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
device with no usable clipboard — a headless box, the emulator over ssh — needs.
Both are part of the **dev-extras bundle**, so they exist in a dev build only: a
release build never writes a link into the log, because the log is a plain file
and a link holds the decryption key (see *Development builds*).

- **Settings → *Include links in the log*** — a persisted switch, on by default in
  a dev build and not offered in a release. When on, every share's link is written
  to the log and can be read back with `grep`.
- `KELP_DEBUG_LINK=1` in the app's environment — the same effect for a single
  run, without touching the UI. Kept so existing headless scripts keep working.

`KELP_INITIAL_LINK=<kelp://…>` (or passing the link as an argument) opens that
link in *Open a link* at startup. This is how a link is handed *to* a device
that cannot paste into it, and it is the same page a `kelp://` activation fills
(design §2.1).

`KELP_ADVERTISE_HOST=<address>` makes a link name that address instead of the one
the listener bound. It is for a device reachable only through another one — the
emulator behind QEMU's NAT, bridged by `src/tests/run-emu-share.sh` — where the
address that works is the *host's*, not the guest's. Only the address written into
the link changes; the socket still binds the real interface.

Both write the link to the log; read it back with:

```sh
grep -o 'kelp://[^"]*' ~/.local/share/harbour.kelp/harbour-kelp/kelp.log | tail -1
```

**Did anyone fetch it?** The serving side keeps its own account of a transfer and
writes one line per peer, about five seconds after that peer stops asking:

```text
2026-10-16 21:01:55.708 INFO  KelpServer: "10.0.2.15: 1 fetch(es) started, 1 object(s), 102 byte(s) of \"Screenshots\"; 3 request(s) refused"
```

`fetch(es) started` counts authenticated `/info` calls, which the client makes once
per fetch; `object(s)` and `byte(s)` are what actually left the device; and
`request(s) refused` counts `401`s — a link or a token that was not recognised.
Without it, a recipient that fetched everything and one that never arrived left the
same trace: none. Nothing in the line is secret, so it is always logged. A fetch
that arrives over a relay is served by the tunnel and reported by a `KelpTunnel:`
line instead, per tunnel rather than per peer (the relay multiplexes recipients):
objects wanted, objects served, bytes, and anything wanted but not held here.

To check a share *before* sending its link to anyone, `run-emu-share.sh --fetch`
fetches from it the way a recipient would — reachability, token, objects — and
prints that line for the traffic it just made. The fetching is
`src/tests/kelp-probe.py`: it parses the link so it can derive the token, but never
prints the link, so its output is safe to read or paste.

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
request header), `tst_kelpsettings` (the persisted preferences), `tst_kelpshares`
(the persisted rows), `tst_kelpqr` (QR encoding, pinned against Python's
`qrcode`) and `tst_kelpbundles` (seal, the local-address and link-log gates,
storage, pause/resume). Run them the same way, e.g.
`build-shell sh "$PWD/src/tests/run-kelpqr-test.sh"`, or run everything — libkelp,
the app layer and the relay harness — with
`build-shell sh "$PWD/src/tests/run-all-tests.sh"`. Because a couple of suites
check a default that follows the build's dev-extras bundle, run that twice, once
with `KELP_DEV_EXTRAS=1`; [`META-INF/BUILD.md`](META-INF/BUILD.md) is the
canonical build and test document.

## Building

qmake-based Sailfish OS project, built with
[sfdk](https://sailfishos.github.io/sfdk/) or the Sailfish IDE:

```sh
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build    # phone (Jolla C2)
sfdk -c target=SailfishOS-5.1.0.11-i486    build    # emulator
```

**Switching target needs a clean first** (the build is in-source), and the same
applies when the build's defines change. [`META-INF/BUILD.md`](META-INF/BUILD.md)
has the whole story: the clean rule, the dev-extras bundle that separates a
development build from a release, how to run every test suite, and the build
traps that have actually cost time. Making a release is one line in
`rpm/harbour-kelp.spec` (`%define kelp_dev_extras 0`); see *Development builds*.

### Development builds

A build carries the **dev-extras feature bundle**
([`src/kelpbuild.h`](src/kelpbuild.h)) only when it asks for it: `KELP_DEV_EXTRAS=1`
in the environment at build time defines `KELP_DEV_BUILD`, and the RPM spec sets it
while developing, so `sfdk build` gives a dev build. A release sets
`%define kelp_dev_extras 0` in [`rpm/harbour-kelp.spec`](rpm/harbour-kelp.spec); a
build that asks for nothing — a bare `qmake` — is a release, so the safe answer is
what you get when nobody said otherwise.

The bundle is where conveniences meant for building and testing live together, so
one switch turns all of them off at once and the next one cannot quietly ship by
being forgotten. Today it holds one: writing each share's `kelp://` link — and so
its key — into the log, and offering the *Include links in the log* switch that
turns that off. In a release build the default is off, the switch is not shown, and
`KelpBundles` refuses to write a link even if a preference carried over from a dev
build says otherwise. `KELP_DEBUG_LINK` is part of the same bundle, for the same
reason. Every launch says which build it is:
`Kelp: build mode — dev extras on|off`.

[`META-INF/BUILD.md`](META-INF/BUILD.md) has the detail, including what to check
after flipping the switch.

## Permissions

The app requests `UserDirs` (see `harbour-kelp.desktop`) so the pickers can read
your files and a sealed share can be measured and named, and a download can be
written into *Downloads*, and `Internet` because sharing serves objects over the
network and opening a link fetches them. `Internet` is the permission that lifts the network
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
