# Kelp — a minimal, secure, content-addressed file-sharing system

**Speculative design** (the "same core features as IPFS, but ~500× smaller" alternative
for a Sailfish OS app). Companion to `ipfs-fileshare-PLAN.md`.

Working name: **Kelp** — app id `harbour-kelp`. Kelp grows in dense, connected underwater
forests — a fitting image for a peer-swarm that links many nodes into one living network
(and it keeps the nautical theme of Sailfish and Harbour). The protocol name is just
"Kelp"; the unit of sharing is a **bundle**.

---

## 0. Why not IPFS, restated

Kubo is ~70–110 MB as a single executable, and its bulk is **libp2p**: multi-transport,
multistream-select, Noise/TLS handshakes, yamux, QUIC, a global Kademlia DHT, AutoNAT,
relay/circuit-v2, hole punching, Bitswap, plus IPLD/multiformats codecs and UnixFS. That
is the machinery for *running a node in a global, trustless, permissionless mesh*.

A file-sharing app does not need a global mesh. It needs to (a) take an immutable
snapshot of a folder, (b) hand someone a link, (c) let them fetch it and verify it, and
(d) survive the phone being unreachable. That is a **verifiable object store with a
pluggable location layer**, not a routing network. Most of IPFS's weight exists to solve
problems in (d) that we can solve far more cheaply for the one-to-many, signed-link case.

So the design thesis:

> **Decouple the immutable content layer from the ephemeral location layer.** Make
> every byte self-verifying, then let the transport be as dumb as possible — and reuse
> the OS's existing TLS/HTTP stack instead of shipping one.

The payoff: the whole thing is **under ~200 KB of added app code**, and the only new
runtime dependency is crypto the platform already ships.

---

## 1. The content layer (pure, offline, no networking)

### 1.1 One noun: the object

Everything is an **object**: an immutable byte string identified by the SHA-256 hash of
its *own bytes*. There is exactly one fetch verb:

```
GET /o/<hash>      →  bytes   (and the receiver verifies SHA-256(bytes) == <hash>)
```

Manifests, chunks, and even a bundle's root are all just objects. This is the single most
important simplification versus IPFS's many command surfaces (`add`, `pin`, `cat`, `ls`,
`dag`, `files`, …): **one noun, one verb.**

### 1.2 Chunking and Merkle structure

- A **file** is split into fixed-size chunks (default 256 KiB; content-defined chunking is
  a later upgrade for dedupe/partial-transfer efficiency).
- A **file manifest** lists the ordered chunk hashes (plus total size, name, optional MIME).
- A **directory manifest** lists entries `(name, type, child-hash, size)`; oversized
  directories split across several manifest nodes, so manifests are bounded — a canonical
  Merkle DAG.
- The **bundle root** is the (encrypted) root directory manifest; its object hash is the
  **bundle id**.

This gives IPFS's genuinely good properties — dedup within a bundle, verifiable partial
fetch, resumable transfer — without IPLD's generality.

### 1.3 Encoding: deterministic CBOR

Manifests are **CBOR** (RFC 8949), canonical form, so the same tree always hashes the
same. CBOR is compact, IETF-standard, web3-adjacent, and has *tiny* embeddable codecs
(e.g. QCBOR is a few thousand lines → tens of KB). This replaces IPLD's codec stack.

Sketch (conceptual, not final):

```
# file manifest
{ v:1, t:"file", s:12947230, c:[ h0, h1, h2, ... ], mime:"image/jpeg", m:1729… }

# directory manifest
{ v:1, t:"dir", e:[
    { n:"photos", t:"dir",  h:…, s:… },
    { n:"notes.txt", t:"file", h:…, s:412 }
] }
```

### 1.4 Encryption is not a mode — it is the default

This is where Kelp is *better* than IPFS rather than merely smaller. In IPFS, content is
public and encryption is bolted on. In Kelp, **the addressing layer only ever sees
ciphertext**:

- A bundle carries a random **32-byte secret**.
- Each **object** is `version || nonce(12) || AES-256-GCM(ciphertext+tag)`, with a
  **fresh random nonce** per object (safe at our object counts), keyed by a *content
  subkey* derived from the bundle secret via HKDF — so an independent *swarm-membership*
  token can be derived from the same secret without revealing the content key (Appendix A).
- The **bundle id is the hash of the ciphertext**, so no confirmation oracle exists for
  known plaintext (unlike convergent/plaintext-addressed schemes).
- The **manifest itself is encrypted too** — file names, tree shape, and sizes are opaque
  to relays and gateways. An observer sees only uniformly random blobs.
- Crypto is **libcrypto (OpenSSL 3)** only — already on every Sailfish device and on the
  Harbour allow-list, and AES-GCM is hardware-accelerated on the C2's Cortex-A75 (ARMv8
  crypto extensions). **Zero crypto dependencies added.**

Optional **passphrase links:** derive the secret with Argon2id (tiny embedded impl) or,
dependency-free, PBKDF2-HMAC-SHA256 from libcrypto — so the link need not carry the raw
key. Trade-off noted in §7.

---

## 2. The link (a capability, not a pointer)

```
kelp://<bundle-id>[?r=<relay>][&q=<sealed hints>]#<secret>
```

- `bundle-id` — 32-byte SHA-256, **base32**-encoded (~52 chars, no ambiguous chars).
- `#<secret>` — the bundle secret, **base64url** (~43 chars).
- The fragment is **never sent to servers** — the relay and any gateway see only the id.
- `?r=` names a rendezvous relay; `?q=` carries the publisher's own LAN address and the
  share's label, sealed under `HKDF(secret, "kelp/v1/link")` (wire spec §5). The link text
  discloses neither, while a holder of the fragment — every intended recipient — opens the
  block and dials. An older link spells those hints out in the clear as `l=`/`n=`, which
  decoding still accepts.

A 150–190-char link: fully self-contained, revocable only by withholding key/ciphertext,
and usable by an app or (via §5) a browser.

**Web3 note:** the object identifier is deliberately *one step from a CID* — a
multihash/base32 CIDv1 over the same bytes. Where interoperability matters (§6) we emit a
CID-shaped variant; where it doesn't, we keep the compact native form.

---

## 3. The location layer (where the bytes actually come from)

Because every object is self-verifying, the location layer can be **simple, dumb, and
untrusted**. Kelp defines a *spectrum*, all speaking the same one-verb object API.

### Reachability paths at a glance

The obvious question — *"two users on opposite sides of the planet; does it work?"* — is
answered by there being **two primary paths** (LAN-direct and relay), plus optional
caching and an opt-in peer swarm. Selection is automatic and invisible; the link carries
what's needed for each.

| Path | Applies when | Third party | Needs internet | §  |
|---|---|---|---|---|
| **LAN-direct** | both devices on one routable local network | none | no (works offline) | 3.1 |
| **Relay** | everyone else — i.e. the normal cross-internet case | yes, but **untrusted** (ciphertext only) | yes | 3.2 |
| **Steward / cache** | relay (or a peer) is holding a copy, so the phone can be offline | yes, untrusted | yes | 3.3–3.4 |
| **IPv6-direct** *(future)* | both ends have globally routable IPv6 | none | yes | 3.6 |
| **Swarm** *(optional)* | several fetchers of the same bundle can reach each other | peers (untrusted) | yes | 3.7 |

The receiving client tries the advertised LAN address first **if it is reachable on the
current network**, and otherwise falls back to the relay. "Same Wi-Fi" is a *topology*
statement, not a distance one: LAN-direct is merely the opportunistic fast path (faster,
no bandwidth cost, works with no internet at all), while the relay path is the one that
carries a link to the other side of the planet.

> **Key distinction:** the relay is a *routing* dependency, not a *trust* dependency. It
> only ever relays ciphertext and object hashes, never sees names/sizes/contents, and
> anyone can run their own — or later bypass it entirely with IPv6-direct (§3.6).

### 3.1 LAN-direct (pure P2P, no server)

The phone runs a tiny HTTPS listener on the local network and advertises the bundle via
mDNS (`_kelp._tcp`). A recipient on the same Wi-Fi fetches directly:

```
GET https://<phone-lan-ip>:<port>/o/<hash>
```

TLS is a **self-signed cert whose fingerprint is in the link** (cert pinning), so no CA
and no plaintext-on-LAN. This path involves **no third party at all** — genuinely P2P.

> **What shipped at C2a differs from this sketch, deliberately.** The address rides in the
> link instead of being discovered with mDNS — a link is a capability, it does not
> have to *find* the sender, and mDNS would announce the share to the whole LAN. The
> endpoint is the link-gated form of wire spec §7.2
> (`/v1/bundle/<bundle_id>/object/<object_id>` with a bearer `swarm_token` derived from the
> link secret) rather than an unauthenticated `/o/<hash>`, so only link-holders get objects
> and only while the bundle is shared. And it is plain HTTP: the content is already
> ciphertext that the receiver hash-checks, and TLS on Qt 5.6 means TLS 1.2 and no ALPN
> rather than the 1.3 + `kelp/1` the swarm spec asks for (measured — see the note under the
> C3 entry below). Pinning a cert fingerprint in the link stays the upgrade
> path when LAN metadata confidentiality matters more than being able to use `curl`.

> **"Same Wi-Fi" is a topology statement, not a distance one.** This path requires both
> devices on one routable local network — the only place the phone's private address
> (`192.168.x.x`) means anything. It is the *fast path*, not the only path. A recipient
> anywhere on Earth uses the relay path (§3.2); the app picks automatically (try LAN if
> the advertised address is reachable on the current network, else relay).

> **The address is sealed, and can be withheld entirely.** The address is the publisher's
> *own*, so it is the one part of a link that was not already opaque: it is now sealed
> (`q=`, wire spec §5, key `HKDF(secret, "kelp/v1/link")`), so the link text does not name
> it to anything that sees the link *without* its fragment. It is still a private LAN
> address in the common case, but a carrier-assigned one when the chosen interface is a
> cellular bearer (the LAN host picker falls back to a point-to-point interface). The
> recipient, which holds the fragment, necessarily learns the address in order to dial it —
> so for a share that must not disclose where it is served from at all, the switch is the
> answer: *Settings → Include my local address in the link* (on by default) drops the whole
> LAN publish, so the link carries no address and the relay path is all that remains.

### 3.2 The relay (the answer to NAT) — publisher dials *out*

The phone cannot accept inbound connections through carrier NAT. The elegant fix is that
**the phone is always the client**:

```
phone  ── persistent outbound tunnel (WSS/QUIC) ──▶  relay  ◀── fetch ──  recipient
```

The relay multiplexes recipient `GET /o/<hash>` requests back down the tunnel the phone
opened. **No port forwarding, no hole punching, no STUN/TURN, no AutoNAT, no circuit-v2**
— none of libp2p's reachability machinery is needed, because we invert who dials. (Same
trick as ngrok/Cloudflare tunnel, for a phone.)

Crucially the relay is **untrusted**: it only ever carries ciphertext and hashes; it
cannot read names, sizes, or contents. Recipients verify every object locally.

### 3.3 Store-and-forward (links that outlive the phone)

The same relay can optionally **cache objects** (encrypted blobs) for a bundle. This is
Kelp's equivalent of a pinning service, but it falls out of the relay for free and needs
no separate protocol. A user can run their own relay (a few hundred lines) or use a
shared/community one.

### 3.4 Stewards (federation without a DHT)

Any peer that has fetched a bundle can become a **steward** — it holds the objects and
re-serves them, and registers as a source with the relay. Multiple stewards mean a link
survives even if the phone and the original relay are gone. This gives redundancy you'd
otherwise get from the DHT, with no DHT.

### 3.5 Optional DHT / web3 bridge (future, behind the same API)

If global, serverless discovery is ever wanted, a small Kademlia-over-UDP module can be
added as *another* location backend without touching the content format. Likewise an
**IPFS/filecoin/Arweave mirror** can be added as a steward that speaks another protocol
(§6). This is the key property: **the format is stable; transports are pluggable.**

### 3.6 IPv6-direct (future: planet-scale with no relay)

Many mobile carriers hand out **globally routable IPv6** addresses, so the phone becomes
directly dialable with no NAT at all. When both ends have it, a recipient across the world
can fetch straight from the phone — **no relay, no third party, no hole punching** (IPv6
hosts are simply reachable). This is the cheapest route to relay-free cross-internet
sharing, and because the content layer is transport-agnostic it is an *addition* to the
location spectrum, not a change to the format or the link.

### 3.7 Swarm mode — torrent-style peering among fetchers

**Idea:** when 10 people fetch one bundle, don't serve all 10 from the phone/relay — let
the fetchers trade chunks among themselves, BitTorrent-style. Kelp is already most of the
way there: chunks are immutable and hash-verified, which is the *hard part* of torrenting.

**What it adds (and nothing more):**

- **Peer discovery per bundle ("tracker").** The relay already mediates every bundle, so it
  can hand each fetcher the addresses of the others (or push peer lists down existing
  tunnels). No DHT needed.
- **Bitfield exchange.** Each peer advertises which chunk hashes it holds.
- **Request scheduling.** Rarest-first (so the swarm stays complete even if the origin
  leaves early); sequential prefetch for streaming.
- **A minimal peer wire protocol** — `HAVE` / `WANT` / `DATA` / `REJECT`, tens of KB of
  code. No new content format, no new crypto.

**Why it stays cheap *and* safe:**

- **Verification is unchanged.** Every chunk is checked by hash no matter who served it, so
  a malicious peer can only *withhold*, never *corrupt* — stronger than BitTorrent, which
  trusts the torrent's piece hashes.
- **Encryption composes.** Peers trade *ciphertext*, so a peer can help distribute a bundle
  it **cannot decrypt**. Stewards can seed blindly; even the relay never holds a key.
- **Origin offload + durability.** The phone's upload cost drops as the swarm takes over,
  and the bundle outlives the phone once enough peers hold it.

**The catch (why it's opt-in, not default):** peer-to-peer between two NAT'd phones needs
connectivity — precisely the problem the relay tunnel was built to avoid. Direct peering
only works when **at least one side is reachable**: same LAN (§3.1), globally routable IPv6
(§3.6), port-forward/UPnP, or hole-punching. When it isn't, traffic falls back through the
relay — still correct, but no bandwidth saved. Also, direct peering **exposes each
participant's IP to the others** (BitTorrent's well-known privacy property), so swarm mode
should be a **per-share toggle**, off for sensitive shares, with relay-only for privacy.

**Middle ground that needs no peering at all:** §3.3/§3.4 caches and stewards already turn
one origin into several sources, so "scale to 10 downloaders" is largely solved by caching;
full swarming is the next step for *zero-infrastructure* scaling (e.g. a link posted to a
forum), where popularity is unpredictable and no cache was primed.

**Optional deeper compatibility:** BitTorrent **v2** already uses SHA-256 plus a Merkle tree
over pieces — conceptually identical to Kelp's chunk tree. Aligning the tree layout would
let a Kelp bundle be exported as a `.torrent`/magnet and exchanged with real BitTorrent
clients, and let Kelp seed into the wider BitTorrent swarm. Treat as a bridge (§6), not a
dependency: "torrent-compatible" without shipping a torrent stack in the default build.

---

## 4. What "web3-compatible" should mean (and shouldn't)

Being honest about this is part of the design. Kelp is web3-*aligned* rather than
IPFS-*implementing*:

| Property | How Kelp delivers it |
|---|---|
| Content addressing / immutability | SHA-256 objects; ciphertext-addressed |
| No trusted party | Relay is untrusted; E2E encryption; recipient verifies locally |
| Self-sovereign, self-hostable | Users can run their own relay; LAN path is serverless |
| Capability-based access | The link *is* the credential; no accounts, no logins |
| Interoperability | Optional CID-shaped ids + **export bridges** to IPFS/web3 storage for permanence |

The honest framing: **Kelp is the edge capture-and-share tool; permanence/interop is
outsourced through bridges.** You get web3's trust model (verify, don't trust) without
shipping web3's entire node stack. Where the user *wants* IPFS, `ipfs-fileshare-PLAN.md`'s
`Backend { EmbeddedNode | ExternalNode | RemotePinning }` still applies — the UI and links
are shared.

---

## 5. Web access: two trust models

Kelp shares files, but a bundle can also be a **web page**, and a browser is the one place
the app itself cannot reach. There are two ways to get a bundle into one, and they are not
two versions of the same idea — they differ in *whose code runs*:

| | §5.1 Shared-page preview | §5.2 The relay gateway |
|---|---|---|
| The recipient has | the Kelp app | only a browser |
| The page's code comes from | the app, shipped in the RPM | the relay, untrusted |
| Who decrypts | the app | the page, in WebCrypto |
| Needs | Kelp plus a source (LAN or relay) | a relay, with the publisher's tunnel up |

Both need one shared convention — which file in the bundle is the page, and how a path
becomes a URL (§5.3) — and both make the app's file list one view of a bundle rather than
the only one. The earlier "browser viewer + gateway" milestone hid this split; it is now
**C4a** (preview) and **C4b** (gateway) in §11, because the trust argument for each is
opposite and hardly any work is shared beyond that convention.

One more section belongs to this group without being a third trust model: **§5.4** is the
receive-side door into §5.1 — the same preview, entered from a download that has just finished
instead of from a share in the list. **§5.5** goes the other way: it is a *kind* the publisher
declares — "this share is a page" — which changes the recipient's flow from saving to viewing,
and is the one place a link carries something §5.3 refused to put there.

### 5.1 Shared-page preview — the app serves the page to the recipient's browser

**The shape.** The sender shares a bundle containing an entry document — `index.html` and
whatever it needs. The recipient's app is the only new part: it fetches and decrypts, serves
the plaintext to `127.0.0.1` on a random port under an unguessable token path, and hands the
URL to a browser. The page's relative URLs then resolve against the bundle, and the app keeps
resolving them per object, on demand, over the LAN or the relay — exactly as it already does
for a file download.

**Why the browser cannot simply talk to the sender.** Not policy: the platform forbids it.
WebCrypto's `crypto.subtle` exists only in a **secure context** — `https://`, or
`localhost`/`127.0.0.1` — and the LAN transport is deliberately plain `http://` on a private
address (§3.1). A page loaded from `http://192.168.x.y:PORT/` on the sender's phone
therefore has no AES-GCM and no HKDF, and cannot decrypt anything. The two ways out are both
worse than re-serving: put TLS on the LAN path (a self-signed certificate, a click-through
warning on every share, and a contradiction of §3.1 for a path whose content is already
ciphertext), or route through a relay whose certificate is real (§5.2). So the recipient's
app decrypts — and the browser sees plaintext over loopback. That is why the token, the CSP
and the lifetime rules below are load-bearing: the loopback origin *is* the security
boundary.

**What it costs.** Less than it looks. The manifest can carry a per-file `mime` (§1.2) — in
practice it never does, because nothing in the app records one, so `Content-Type` comes from the
extension table — and fixed-size chunking means an object's plaintext length follows from its
ciphertext length, so a byte range is arithmetic rather than a search. Objects are
hash-verified ciphertext, so the server is a resolver — path → chunk list → bytes — with the
existing `KelpSource` fetching whatever the store lacks. `kelp://` is already a registered
scheme, so a link clicked in a browser lands in the app. And the background-serving work already
asked the platform to keep the app alive while it serves (the answer was yes, §12.8): a preview
needs the same request while a page is live.

**The server.** One `KelpPreviewServer`, one loopback listener, one session per open bundle.

| | |
|---|---|
| Bind | `127.0.0.1`, ephemeral port — never `0.0.0.0`, the same rule as the LAN listener (§3.1). |
| URL space | `http://127.0.0.1:<port>/<session-token>/<path>`, the token 32 random bytes; without it, `404`. The port is not a secret, the token is. |
| Methods | `GET` and `HEAD`; `Range: bytes=a-b` on a file path. |
| `Content-Type` | the manifest's `mime` when the sender recorded one, else a small extension table — with `nosniff`, so a browser never second-guesses the answer. |
| `Range` | With fixed 256 KiB chunks (§1.2) a range is a contiguous object range: byte `a` lives in chunk `a / 256K` at offset `a % 256K`. `bytes=a-b`, `bytes=a-` and `bytes=-n` are honoured; a multi-range request or a malformed one is ignored and the whole file served (always a valid answer), and a range past the end is `416`. **This is the property CDC would break**: content-defined chunks have no closed-form offset, so adopting it (§12.1) means recording per-chunk lengths in the manifest, or fetching to seek. |
| A directory | `/sub/` serves `sub/index.html`; `/sub` without the slash is a `301` to `/sub/`, because the page's relative URLs would otherwise resolve against the parent. |
| Unknown path | `404`, including `..`, a NUL, a file used as a directory, and any path not in the manifests — every name is re-validated against the manifest's own rules on the way out, so the URL space is exactly the bundle. |
| A bundle not all here | `503`, not `404`: a manifest or data object the store cannot answer means the question has no answer *yet* (“not fetched”), which must not be shown as “no such file in this bundle”. Every object an answer needs is checked before a byte goes out, so a page is never half-served. |
| Lifetime | A session is dropped after 30 minutes without a request, or when it is closed; the listener dies with the app. A 32-byte token is the whole address, so a stale URL is worthless. |
| Plaintext at rest | none: objects are decrypted per response, straight to the socket, one chunk at a time, and never written. |

Every response also carries these headers:

```
Content-Security-Policy: default-src 'self'; img-src 'self' data:; media-src 'self';
  style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline';
  connect-src 'self'; frame-ancestors 'none'; base-uri 'none'
X-Content-Type-Options: nosniff
Referrer-Policy: no-referrer
Cache-Control: no-store
```

The CSP is what makes "the page works offline" true in *any* browser, and `no-store` keeps
decrypted bytes out of a browser cache; both therefore do their work in either route below,
while an embedded WebView can also refuse a request outright.

**What is built.** Everything in the table above, plus the §5.3 convention, is in the tree:
`libkelp/kelp_web.{h,cpp}` (which file is the page; path → file or directory; the chunk range
behind a byte range; the extension table — manifests only, never a data object) and
`src/kelppreviewserver.{h,cpp}`. `src/tests/tst_kelppreview.cpp` drives the server over a real
socket with no browser and no display, and `tst_kelpbundles` covers the app seam; `BUILD.md`
has the counts. The seam is `KelpBundles::pageInfo/previewPage/endPreview`, and the first UI is
a **Web page** section on a bundle's own page — shown only when the bundle has one — with
*Open in browser* (route B below). No menu was added: a labelled section on the page is where
the design put it, and the whole flow is one button.

**What is not built yet.** The lazy fetch through `KelpSource`, so a preview serves exactly what
the store already holds and answers `503` for anything else. The per-bundle confirmation
(§12.10) — deferred until the page being opened is someone else's. The *recipient* side is no
longer blocked on a kept bundle: §5.4 opens the preview for the session a download just finished,
and keeping a received bundle (§12.13) is what would make that preview re-openable and the bundle
seedable.

**Two ways to open it, and the recommendation.**

- **Route B — hand the URL to the system browser** (`Qt.openUrlExternally`): tens of lines,
  and the recipient gets tabs, find, the share menu and their own history for free. The cost
  is that Kelp becomes a background server for another app, so it must hold the keepalive
  while a session is live, and the browser is the party making requests — the server's own
  headers are the only leash.
- **Route A — an embedded `Sailfish.WebView`** (Harbour-allowed, with
  `sailfish-components-webview-qt5`): Kelp owns the session, no keepalive is needed, closing
  the preview ends it, and a request interceptor can refuse anything that is not loopback,
  which is a stronger guarantee than CSP. The cost is a heavy webengine dependency, and
  QtWebEngine under Sailjail is unproven here — a spike, not an assumption.

**Recommendation: build the server once — it is the whole feature — ship route B, and treat
route A as a UI upgrade decided on the device**, where "does handing the browser away feel
wrong?" has a real answer. Nothing in the server differs between them.

**A preview runs the sender's code, and the design says so.** Today a Kelp link yields inert
files, opened by apps the recipient chose. A preview runs the *sender's* JavaScript, on the
recipient's device. So it is a visible choice and never a default:

- *Preview* is a separate action from *Open*, offered only when the bundle has an entry
  document (§5.3). *Open* keeps meaning "inert files, your apps".
- The first preview of a bundle asks, in plain words, that this runs code from the sender, on
  this device, offline. The answer may be remembered **per bundle id** — which is safe
  precisely because ids are content addresses: the same id is the same immutable bytes
  (§1.1), so a remembered "trusted" cannot later be swapped for different content. This is
  the one place the content layer's immutability pays a security dividend for free.
- No bridge to the app: no `QWebChannel`, no custom scheme handler reaching the store, no
  engine profile tricks. The page speaks HTTP to the loopback server and nothing else.
- Offline, enforced by the CSP above. A page that wants the wider network is a separate
  decision that must not happen by accident.
- An ephemeral browser profile where the route allows it — no cookies, no local storage — so
  a page leaves no state behind and two bundles cannot see each other.
- What it does not protect: the clipboard, the screen, the OS and the CPU. Closing the preview
  ends the session.

**Failure modes, spelled out.**

| Situation | Behaviour |
|---|---|
| No HTML in the bundle | no *Preview* action; the file list and *Open* are unchanged |
| Several HTML files | `index.html` at the root, else the shallowest `index.html`, else a picker of them; the chosen file's folder is the web root (§5.3) |
| Entry document in a subfolder | that folder is the web root, so its `index.html` is `/` |
| Sender offline, or the tunnel dropped | the page's requests answer `503`, and the app says why rather than letting the page fail silently |
| Relay-only bundle | works, slower; the LAN address is preferred whenever the link names one |
| A big file in the bundle | streamed per object and seekable by `Range`; never assembled in memory first |
| No browser installed | **verified on the emulator: `Qt.openUrlExternally` returns `true` and nothing opens** — Qt 5.6 reports only that the URL was *handed over*, so the app must not treat its result as a promise (the emulator image has no `sailfish-browser` at all). Route B therefore depends on a browser being present, which is an argument for route A beyond UI taste (§12.9) |
| Kelp closed while the browser shows the page | serving ends (a Harbour app cannot outlive itself, §12.8) and the page starts failing; the preview says so before you leave |
| Two previews at once | one server, one token path per session, one keepalive while any session is live |

**Explicit non-goal: the sender hosting the page for a LAN browser.** Tempting — the sender
already holds the bytes — but it would serve *decrypted* content over plain HTTP to whoever
is on the network, with the URL as the only secret, to a recipient who by construction (a
browser and the link, but no app) is §5.2's problem anyway.

### 5.2 The relay gateway — recipient with no app

The other half. Because the transport is HTTP and objects are content-addressed, a
recipient's browser is served by the **relay acting as a gateway**:

```
https://<relay>/b/<bundle-id>#<secret>
```

The relay returns a tiny self-contained **HTML+JS loader** (itself just an object). The
loader fetches the encrypted root manifest, decrypts it with the fragment key via
**WebCrypto AES-GCM**, walks the tree, fetches chunks on demand, verifies every hash,
decrypts — and then does what §5.1 does, with the same convention: it mounts the sender's own
entry document (§5.3) and fetches the rest from the same origin. So the page a
recipient-with-the-app sees and the page a recipient-without-the-app sees are the same page,
with the relay standing in for the recipient's app. The relay serves ciphertext only;
**decryption happens client-side** and the key never leaves the fragment. This is the same
"just send them a link" UX as an IPFS public gateway.

Two caveats belong here rather than in a footnote. First, the loader is served *by the
relay*, so a **malicious relay can serve a loader that exfiltrates the fragment key**:
encryption protects against a relay reading, not against one that gets to write the
recipient's code. That is inherent to browser gateways without a trusted bootstrap, and it
is precisely why §5.1 exists for recipients who have the app. Second, WebCrypto needs a
secure context, so the gateway must be HTTPS with a certificate a browser accepts — the
self-signed certificate the app clicks through (§11, C3.1) is not enough. That also settles
a C4b detail: the relay must accept ALPN `http/1.1` beside `kelp/1`, since a browser
negotiates the former.

### 5.3 Which file is the page — the shared convention

Both routes need one answer, and it is deliberately not a new wire field:

- The **entry document** is `index.html` at the bundle root if there is one; otherwise any
  `index.html`, the shallowest one; otherwise the sole HTML file; otherwise the recipient picks
  from the HTML files present. A file is HTML when its recorded `mime` is `text/html` *or* its
  name ends `.html`/`.htm` — both are the sender's own words, so this is which file to *offer*,
  not a check.
- The **web root** is the folder holding the entry document, and the **URL space** is the
  bundle's paths relative to it: `/` is the entry document, `/sub/x.png` a file, and `/sub/`
  serves `sub/index.html` when one exists.
- A bundle with no HTML is simply not previewable; nothing else about it changes.

`index.html` is the convention of every static site, zip preview and git host; a
Kelp-specific manifest pointer would be a new field on the wire for no gain. So nothing about
a link changes: a previewable bundle is an ordinary bundle, the sender only decides to
include HTML, and the recipient decides to preview it. The app may *label* such a bundle
("contains a web page") on its detail page, which is also what keeps the confirmation from
being a surprise.

**A bundle with no page is not a gap in §5.2.** For §5.1 "not previewable" costs the
recipient nothing — the app's file list and *Open* still work, which is what they were using
anyway. For §5.2 there is no app to fall back on, so the loader renders a **generated file
list** of its own (names, sizes, per-file download, optionally a ZIP assembled in the
browser) when the bundle has no entry document. The convention above decides which page the
*sender* supplied; the loader's list is the floor beneath it. *(§5.5 later adds one link hint —
not for "which file is the page", which this section still refuses to put on the wire, but for
the publisher's **kind**: "I shared this as a page". Contents can decide the first and cannot
decide the second.)*

### 5.4 The preview from the receive side — one session, nothing kept

**The gap.** §5.1 was designed for a recipient — "the recipient opens the page" — but the only
door into it is a bundle *this device shares*: **Web page** lives on `BundlePage`, and that page
is reached from the bundle list, which is `shares.json`. A bundle fetched from a link is written
out as ordinary files and then forgotten — no row, no kept key — so the person the feature exists
for never sees the page. This is the receive-side half of §5.1, and it is deliberately the
smallest shape that closes it.

**The shape.** When a download finishes, the Open-link page's Done panel gains one button —
**Open in browser** — shown when the bundle just fetched has an entry document (§5.3). Pressing
it opens a session against the bundle already in the object store and hands the URL to the
browser, exactly as the sender's button does. Nothing else changes: no row, no stored key, no
list entry; the ordinary 30-minute idle session that dies with the app; and no fetch, because
`KelpClient` already stored every object to verify it (§11 C2b). A *webpage* link (§5.5) never
reaches this panel — it is view-only from the start, with no download to add a button to — so what
this section offers is every *other* share that happens to contain a page.

**Why not keep it.** §12.13 asks whether a received bundle should become a row (id + key) so it
can be previewed again, re-downloaded, or seeded. That is a bigger decision — a key this device
never minted, held on disk; a list that mixes "shared by me" with "received" — and it is not
needed to answer the question this exists for: *does the recipient see the page?* What the
ephemeral button gives up is stated plainly: the URL dies with the app, the bundle is findable
again only through the link, and a received bundle can be neither re-shared nor seeded.

**How the app knows there is a page.** The §5.3 convention, asked once when the fetch reaches
*done* and never in a binding — the rule `BundlePage` already follows. The seam is the existing
pair `KelpBundles::pageInfo(bundleId, secret)` and `previewPage(...)`, which take exactly the two
strings a link carries and care nothing about where the consumer came from. So `KelpClient` grows
two read-only properties, `bundleId` and `secret`, filled from the decoded link and cleared by
`reset()`; the page asks and opens through the same calls the bundle page uses. No new C++ in the
preview path, one listener, one server. The key is not newly exposed by this — the Open-link page
already holds the whole link, fragment included, and redacts it before anything is logged — and
the rule is unchanged: the secret never reaches the log, the store, or the screen.

**One thing the receive side needs that the send side did not: the keepalive.** `Nemo.KeepAlive`
is held today only while *Share in background* is on and something is being served. A recipient
turned that switch on for nobody and has nothing in the list being served, so the request not to
be ended would not be made — while the party in the foreground is the browser, not the app. The
condition becomes: held while **any preview session is live**, or while serving is on and a share
is unpaused. §12.8 already noted a preview needs this; this is where it lands.

| Situation | Behaviour |
|---|---|
| The bundle just downloaded has no HTML | no button; the Done panel is exactly what it was |
| The fetch failed or was cancelled | no button: there is no complete bundle to serve |
| Several HTML files | §5.3's choice is served, and a line says how many pages the bundle holds; no picker, matching the sender's own page |
| A single-file bundle that is a page | works: the entry document is the file, the web root is the bundle root |
| The page wants a CDN | blocked by the same CSP; offline is the design (§12.11), and this button does not change it |
| Kelp closed, or the session times out | the URL stops answering; the download sits untouched in Downloads |
| Objects pruned mid-session | the page answers `503`, as any preview does for what the store cannot answer |
| The emulator, which has no browser | `Qt.openUrlExternally` returns `true` and nothing opens; the session still exists, so a `curl` from inside the guest is the proof, and this is how it is verified |

**Explicit non-goals of this shape.** Keeping the row (id + key), which is what would make a
preview re-openable and a received bundle seedable; a remembered per-bundle consent — every
preview here is a fresh, explicit press, so there is nothing to remember, and §12.10's prompt
stays parked until a received bundle *is* kept; a page picker; and serving a received bundle to
anyone else.

**On trust, one line.** This is the first door through which a page someone *else* wrote reaches
the preview's code path, and the answer is the one §5.1 already had: the button is a distinct,
visible action and never a default, the page gets the loopback origin and the CSP with no network,
and the session ends with the app. Route B is the route for the same reasons — and here it is the
recipient's own device, so §12.9's "what if there is no browser?" becomes a recipient-side
question too.

### 5.5 Webpage shares — a kind the publisher declares, a page the recipient only views

**Why a kind, when §5.3 already finds the page.** §5.3 answers *which file is the page* out of the
bundle's own contents, and it deliberately added no wire field. It cannot answer a different
question: *was this shared as a page, or as files?* The folder of a small site and a folder that
merely happens to contain an `index.html` can be byte-identical, and then their bundle ids are
identical too — bytes cannot carry an intent. The two want opposite recipient flows, though: files
are downloaded, a page is looked at. So the publisher declares one, in the link.

**The hint.** `q=`'s sealed map (wire spec §5) gains a third key:

```
plaintext = canonical CBOR map { "l": <host:port>, "n": <label>, "p": true }
```

— "I shared this as a page". It rides with the other hints inside the sealed block, so the link
text still says nothing about the bundle beyond its content address. It is a *hint*, not a switch:
the wire spec already requires unknown map keys to be ignored (§17), so a reader that predates it
sees an ordinary share, and a rewritten client can ignore it outright. Nothing about a recipient's
security turns on it — they hold the key and can decrypt whatever they like. What it buys is that
the ordinary client does the right thing by default. A page share therefore mints `q=` even when it
has no address to put in one, so the hint survives a relay-only share.

**What the recipient does with it.** For a link carrying `p`, the Open-link page is a *view*, not a
*save*:

- No **Save to** and no *Download*. Nothing is written to Downloads, and no folder is chosen.
- One button — **Open in browser** — fetches the bundle and then serves it to the browser from
  `127.0.0.1`, exactly as §5.1 does. One listener, the same server.
- The fetch lands in a **session store held in memory**, not in the app's object store: the page
  exists only while it is being looked at. The store is shared with the session that serves it, so
  it dies when the session does — the page's requests stop coming and the idle sweep drops it, or
  the app exits — so there is no cleanup step, no destructive deletion, and, the whole point,
  *nothing of the page is left on the device*. Leaving the screen is deliberately **not** an end:
  the browser is what the user is looking at, and it may still be loading or reloading after Kelp
  is out of sight, so the session lives until its own idle timeout (the same ~30 minutes the
  owner-side preview has always had) and its memory is released then. A *second* webpage link
  therefore cannot free the first session's store: the session holds a reference of its own, which
  is why `KelpPreviewServer::open()` takes a `StorePtr` rather than a bare store.
- Being memory, there is a ceiling: **32 MiB**, enough for a document-shaped page and not for a
  gallery. `/info` gives the sender's claimed size, which is checked before a byte is fetched and
  enforced again while storing — the claim is only a claim; a bundle over the ceiling is refused
  with the reason and the way out (ask the sender to share it as a folder). Lazy fetch (§5.1's open
  item) is what would lift this, because fetching only what the browser asks for needs no
  whole-bundle copy at all.
- Pressing **Open in browser** again after the page has been served re-opens the *same* session —
  the same URL — rather than fetching again or minting a second token.
- **KeepAlive** is held while the session is live (the rule §5.4 needs too), because the browser now
  owns the foreground and the app is only serving it.

**What the publisher's side looks like.** The main page's button becomes **Share…** and opens a page
with three actions — **Share file**, **Share folder**, **Share webpage** — in place of the old
pickers and the staging list. Each gesture is the whole share: pick, and the bundle is sealed and
appears in the list. *Share webpage* picks a folder and insists it holds a page by the §5.3 rule,
evaluated on the folder's names before anything is sealed; a folder with no HTML is refused there
and then, with the wording that points at *Share folder*. The staging list goes away with the
old pickers: it existed so several entries could be sealed in one blocking pass, and one kind per
gesture leaves nothing for a batch to buy — the cost is that two different kinds can no longer be
collected in one pass. The §5.3 rule is factored so the filesystem answer and the manifest answer
cannot drift: one pure chooser over (relative path, is-HTML) pairs, fed from the folder by the app
and from the manifests by `Kelp::Web::page`.

**What it means for §5.4.** Nothing is lost: §5.4's *Open in browser*, offered after a download,
stays for a page that arrives inside an ordinary directory share — you still get to look at it,
for that session. §5.5 is the declared case, where there is no download to add a button to.

**Failure modes, spelled out.**

| Situation | Behaviour |
|---|---|
| *Share webpage* picks a folder with no HTML in it | refused before sealing, pointing at *Share folder* |
| A webpage link's bundle turns out to have no page (a hand-made or dishonest link) | "this link says it is a page, but there is no web page in it" — no save is offered, because the kind was view-only |
| The link has no `p` (older link, or a directory share) | an ordinary share: save as always, and §5.4 offers the browser afterwards if the bundle holds a page |
| The link carries `p` but no address and no relay | **Open in browser** cannot start; the reason is the same one the save path already gives |
| The sender goes offline mid-page | the page's requests fail, as any preview does — and there is nothing saved to fall back on, by design |
| The page is bigger than the ceiling | refused before fetching, naming the sender-side workaround |
| The recipient leaves the screen, or closes Kelp | the page keeps being served until the session's idle sweep (about 30 minutes), then the memory is dropped and the URL is dead; closing Kelp ends it at once. Nothing was ever written |
| The recipient's device has no browser | nothing opens (the emulator); the session exists, so a `curl` from inside proves it (§12.9) |

**Explicit non-goals.** Letting the recipient override the kind — a webpage link is view-only, and a
sender who wants the files shared has *Share folder*; remembering the page afterwards (no row, no
key, no objects — §12.13); choosing among several HTML files; and re-seeding a received page. The
hint is a default, not enforcement: it is not a security boundary and is not defended as one.

---

## 6. Bridges to the wider web3 ecosystem (optional, later)

- **CID bridge:** emit a CIDv1 (multihash + base32) for a bundle's objects, so the same
  identifiers work in IPFS-adjacent tooling.
- **IPFS export:** a bundle (unencrypted) can be pushed to a user's own IPFS node or a
  pinning service to become globally retrievable via existing IPFS gateways. Kelp stays
  tiny; IPFS becomes a *destination*, not a dependency.
- **Permanence:** mirror to Filecoin/Arweave/web3.storage via a relay- or companion-app
  action.
- **Mutable names (IPNS/ENS-like):** a **channel** = a small *signed* pointer object
  published to a well-known name (derived from a keypair). Recipients poll the pointer via
  any location backend. Gives "living folder with a stable address" without IPNS's
  resolution machinery.

---

## 7. Security model & threat model

**Protects against:** relays/gateways/ISPs reading content or filenames; tampering (every
object hash-verified); on-path plaintext (TLS + E2E); known-plaintext confirmation (no
convergent addressing).

**Does not protect against:** the recipient forwarding the link (capability links are
un-recallable — document it); endpoint compromise; traffic-analysis of *sizes/timing*
(mitigate with chunk padding, optional); the relay learning *who fetches what bundle id*
unless you use per-fetch blinded ids or fetch through a steward; and, once a bundle's page is
*previewed* (§5.1), **code the recipient did not write** — the sender's JavaScript runs on the
recipient's device inside the browser sandbox, and Kelp's guarantees end at the bytes it
serves.

**Revocation:** stop publishing + delete from relays/stewards. Anything already fetched
cannot be recalled.

**Passphrase caveat:** PBKDF2 (dependency-free) is weaker than Argon2id against GPU
cracking; if we care, embed a small Argon2id. Decide in v1.

---

## 8. Size & performance budget (the whole point)

| Component | Added to app binary |
|---|---|
| Crypto (AES-GCM, hashing) | **0** — libcrypto already on device, Harbour allow-listed |
| Transport (HTTP/TLS/WSS) | **0** — Qt5Network / libcurl already on device, allow-listed |
| CBOR codec (vendored QCBOR) | ~40–60 KB |
| Protocol core (chunking, manifests, links, verify) | ~30–60 KB of our own code |
| QR generator (Nayuki, from the other plan) | ~10 KB |
| **Total app delta** | **≈ 100–200 KB** |
| Relay (server-side, not shipped in the app) | small self-hostable binary (~a few hundred LOC) |

Versus **~100 MB** for an embedded Kubo. That is ~**500× smaller**, with no Go toolchain,
no CGO, and none of the Go-runtime/Qt signal-handling risk that gates the IPFS plan.

Transfer performance is *better* than Kubo for this use case: object-granular resume,
range requests, parallel chunk fetch, and no DHT lookup latency before the first byte
(the link already says where to look).

---

## 9. Comparison to IPFS, feature by feature

| Feature | IPFS/Kubo | Kelp |
|---|---|---|
| Content addressing | ✅ CID / IPLD / multiformats | ✅ SHA-256 objects (CBOR manifests) |
| Merkle DAG / dedup | ✅ UnixFS | ✅ bounded manifests; CDC optional |
| Partial fetch / resume | ✅ Bitswap | ✅ object granularity + HTTP Range |
| P2P / no server needed | ✅ global | ✅ LAN-direct (serverless); relay for WAN |
| Global discovery | ✅ Kademlia DHT | ◻ optional DHT-lite / steward federation |
| Pinning | ✅ + separate pinning services | ✅ relay cache / stewards (built in) |
| Swarming / torrent-style | ✅ Bitswap | ◻ opt-in peer swarm (§3.7; BitTorrent-v2-aligned) |
| HTTP gateway for browsers | ✅ | ✅ relay gateway + in-browser decrypt |
| Encryption | ⚠️ bolt-on, off by default | ✅ **default; names+sizes encrypted too** |
| Mutable names | ✅ IPNS | ◻ signed "channel" pointers (later) |
| **Footprint** | ~100 MB, Go toolchain, CGO | **~150 KB, platform libs only** |
| **Reachability** | DHT + AutoNAT + relay + hole-punch | outbound tunnel to a dumb relay |

---

## 10. Deliberate non-goals

- Being a general DAG/IPLD computation platform.
- A global routing mesh or permissionless provider DHT (v1).
- Multi-writer / collaborative mutable filesystems (channels are pointers, not CRDTs).
- Being able to `ipfs get` a Kelp bundle natively (only via the export bridge).
- Recalling a link once it's been fetched.
- The sender hosting a *decrypted* page for a browser on its own LAN (§5.1 says why: the URL
  would be the only secret, and that recipient has no app by construction).

---

## 11. Implementation sketch

**Shared with the IPFS plan (unchanged):** Silica UI pages, cover, Sailfish.Pickers file
selection + `FolderListModel`, QR, `Sailfish.Share`/clipboard, Sailfish.Secrets for
storing secrets, the whole `Backend` interface, and the encryption-in-fragment link model.

**New work unique to Kelp:**

1. **`libkelp` (C/C++) — the content layer.** Chunking, CBOR manifest build/parse,
   canonical encode, object hash + verify, AES-GCM seal/open, link encode/decode. Pure,
   testable, no I/O. This is the core deliverable (~a few thousand LOC at most).
2. **Local publish/seed service** — tiny HTTPS listener + mDNS advertiser + outbound
   tunnel client to the relay. (Often confused with "a server"; it is a client.)
3. **Relay** — separate, self-hostable: WebSocket tunnel endpoint + blob cache + gateway
   front-end + optional steward registry. Not shipped in the app RPM.
4. **Browser access** — two pieces, one convention (§5.3). For §5.1, a loopback preview
   server in the app (path → objects, the manifest's MIME, CSP/no-store, `Range`, lazy fetch
   through `KelpSource`) plus the *Preview* action; for §5.2, one self-contained HTML+JS
   *loader* object that decrypts in WebCrypto and mounts the sender's own entry document.
5. **Swarm layer (optional, §3.7)** — relay-as-tracker handoff, bitfield exchange, rarest-first
   scheduler, and the minimal `HAVE/WANT/DATA/REJECT` peer wire. Reuses `libkelp`
   verification untouched (peers trade ciphertext; verification is source-independent).
6. **Backend glue** — `KelpBackend : Backend`, parallel to `IpfsBackend`.

**Milestones (sketch):**

- **C1** — `libkelp` round-trip: chunk → manifest → hash → verify → decrypt, with unit
  tests (the crypto+format core; biggest correctness risk, do first). **Done** — see
  `libkelp/` and `libkelp/tests/tst_kelp.cpp`; pinned vectors live in §16 of the wire
  spec.
- **C1.5 (done)** — app wiring: the `kelp://` link codec, the on-disk object store, and
  *Start sharing* sealing for real. Each selected entry becomes one bundle; the row, the
  **Bundle** page and the link show what was sealed, and the read path has been exercised
  on device against the real objects. Not done here: nothing serves an object to a
  recipient yet, objects are never deleted, and sealing blocks the UI thread.
- **C2** — LAN-direct publish/fetch end-to-end on two devices on one Wi-Fi (proves the
  core UX with zero servers).
  - **C2a (done)** — the *publish* half: one listener for the app, bound to this device's
    LAN address (never `0.0.0.0`, so mobile data opens nothing), the address written into
    the link — in the clear as `l=` then, sealed as `q=` now (wire spec §5) — and the
    direct-fetch endpoint of wire spec §7.2 gated by the
    link-derived `swarm_token`. Verified by fetching objects back over the LAN address with
    `curl` and checking every byte against its content address; a bundle stops answering the
    moment it is unshared, while others keep serving.
    Still open: there is no TLS on this path (see the note under §3.1), the address is
    captured when sharing and goes stale if the interface changes, and serving exists only
    while the app runs — there is no auto-start.
  - **C2b (done)** — the *fetch* half in the app: `KelpClient` (`src/kelpclient.cpp`) takes a
    pasted `kelp://` link, reads the address out of it, derives the content key and the
    swarm token, asks `…/info` for the shape the sender claims, and then downloads in
    rounds. `Kelp::missingObjects()` names what is missing — to begin with only the root
    manifest, because everything else is inside it — up to four objects are fetched at a
    time, and each one is kept only if it hashes to the address it was asked for
    (`Kelp::storeReceived()`). When nothing is missing, `open()` reassembles the tree and
    the files are written into *Downloads*: a bundle holding a single file becomes that
    file, anything else a folder named for the link, and every name is validated again on
    the way out so a hostile manifest cannot write outside it. Verified end-to-end on the
    emulator with two instances and two stores: 22 objects fetched over the LAN, six files
    written byte-identical to the source, plus the failure cases — a wrong key, a bundle
    the sender is not serving, a link with no address, and a cancelled-then-retried fetch
    (the objects already stored are kept, so the retry only asks for the rest).
    Still open: no relay (a link with no address says so and stops), the whole tree is held in
    memory before it is written, and nothing in the app can serve again what it downloaded.
- **C3** — Relay: outbound tunnel + object proxy + cache; cross-network fetch from a
  different network (this is the Kelp equivalent of the IPFS M0 NAT spike).
  - **C3.1 (done) — the tunnel transport.** `src/kelptls.{h,cpp}` is a `KelpTls` session
    that speaks the spec's §7.1 transport: TLS 1.3 only, ALPN `"kelp/1"`, any certificate
    accepted (content is hash-verified; TLS is for metadata in transit), and the ALPN is
    *enforced* — a peer that does not negotiate `kelp/1` is refused. Because Qt 5.6 cannot
    do either (see the constraint note below), it drives OpenSSL 3's `libssl.so.3` directly,
    integrated with Qt's socket through a pair of memory BIOs so OpenSSL's records and Qt's
    socket buffering never contend for the same fd. It serves both ends (Client for the
    publisher dialling out, Server for the relay), and lives in the app layer — `libkelp`
    stays pure.
    Verified in the build engine against OpenSSL's own `s_server`: a TLS 1.3 server with
    `-alpn kelp/1` negotiates and carries an HTTP/200 reply; a server offering no ALPN is
    refused (`did not negotiate ALPN "kelp/1"`); a server pinned to TLS 1.2 is refused
    (`tlsv1 alert protocol version`). Recipe: build a tiny qmake target that compiles
    `src/kelptls.cpp` and connects a client to `openssl s_server -tls1_3 -alpn kelp/1 …`
    (generate a self-signed cert with `openssl req -x509 …`; kill servers with `pkill -x
    openssl`, never `pkill -f s_server`, which matches the caller's own command line).
    Still open: the relay itself, the tunnel's framing (wire spec §7.1 frame; §12
    relay-only), and the outbound-tunnel reconnect/keepalive policy.
  - **C3.2 — the tunnel wire and the relay (done, minus the app's own tunnel).**
    `libkelp/kelp_wire.{h,cpp}` is the pure frame + message layer both ends speak: the
    §7.1 frame (`len:u32 || type:u8 || CBOR`), a `FrameReader` that reassembles frames from
    arbitrary TLS chunks and refuses an oversized one from its header, the §8 handshake MAC
    (`HMAC-SHA256(swarm_token, prefix || nonce₁ || nonce₂ || bundle_id)`, constant-time
    compare), and the data-plane messages (HELLO, AUTH, WANT, DATA, REJECT, PING/PONG,
    ERROR) plus the tunnel-setup REGISTER. Also adds `Crypto::hmacSha256` (checked against
    the RFC 4231 vector) and `Kelp::relayToken`. Swarm-only messages (HAVE_RANGES, HAVE,
    CANCEL, PEX) are deferred with C7. 304 unit checks, 0 failures.
    `relay/` is the relay process itself (`kelp-relay`, wire spec §12.1): publisher
    tunnels and recipient requests over TLS 1.3 + ALPN `"kelp/1"` on two ports, a
    `{bundle_id → relay_token, shape, tunnel}` registry, and the object proxy that turns a
    recipient `GET …/object/<id>` into a `WANT` down the publisher's tunnel and returns the
    matching `DATA`.
    **The relay credential is a third, relay-scoped derivation (decided):**
    `relay_token = HKDF-SHA256(secret, salt=∅, info="kelp/v1/relay")`, carried in the
    recipient's URL path. The relay never holds `swarm_token` or `content_key` — it is given
    its own credential and nothing else, and it is still only ever trusted to withhold, not
    to substitute (the recipient id-checks every object).
    Verified end to end: `relay/test/run-relay-test.sh` builds the relay and a harness that
    plays both roles, and checks `/info`, a real object fetch through the relay, a
    missing-object 404 and a wrong-token 401 — all pass. (A `KelpTls` double-`closed()`
    crash this surfaced was fixed: `closed()` is now a one-shot, and the relay's teardown is
    idempotent.)
  - **C3 (done) — cross-network fetch through a relay.** The app now publishes on a relay
    and fetches from one. `KelpBundles::seal()` opens a `KelpTunnel` (src/kelptunnel.{h,cpp})
    for the bundle when relays are configured (Settings → `setRelays`), registers
    `{bundle_id, relay_token, shape}` over TLS 1.3 + ALPN `"kelp/1"`, answers the relay's
    `WANT` from the store, keeps the tunnel alive with PING/PONG and reconnects with
    backoff; the relay's address rides in the link as `r=`. On the receiving side the fetch
    loop was factored behind `KelpSource` (src/kelpfetch.{h,cpp}): `LanSource` is the
    existing plain-HTTP fast path, `RelaySource` is HTTP inside the same TLS 1.3 tunnel, and
    `KelpClient` is unchanged in shape — it asks a source for what the walk says is missing.
    The relay itself is one address serving both roles (a recipient's HTTP request and a
    publisher's frame are distinguished by their first byte), which is what lets a single
    `r=` name it.
    Verified end to end on the emulator with the relay running there: a receiver given the
    link **with `l=` stripped** (so the relay is the only source) fetched 22 objects in 6
    rounds over TLS, wrote 6 files / 1 687 667 bytes, and the tree was byte-identical to the
    source. The relay's own harness (`relay/test/run-relay-test.sh`) covers `/info`, a real
    object fetch, a missing object (404) and a wrong token (401).
    Still open (deliberately): no caching/stewards (C6), no swarm (C7), no TLS on the LAN
    path (only the tunnel), and the relay is a separate binary (`kelp-relay`) the user runs.
- **C4a** — Shared-page preview (§5.1): **built, off-device.** The `Web` convention
  (`page`/`resolve`/`chunkRange`/`mimeType`, manifests only), the loopback `KelpPreviewServer`
  (token path, path → objects, `Range`/`HEAD`, the CSP/`no-store`/`nosniff` headers, `404` vs
  `503`, a 30-minute idle session), the `KelpBundles` seam, and *Open in browser* on a bundle's
  page. `tst_kelppreview` drives it over a real socket and `tst_kelpbundles` covers the seam;
  both run without a browser or a display. Still to do: the lazy fetch through `KelpSource`
  (today a missing object is a `503`), the receive side, now **C4a.1** below, the per-bundle
  confirmation (§12.10), and an on-device pass with a real browser. An embedded
  `Sailfish.WebView` remains a later UI decision (§12.9).
- **C4a.1 — the preview from the receive side (§5.4).** Not built. One **Open in browser**
  button on the Open-link page's Done panel, offered when the bundle just fetched has an entry
  document (§5.3) and served by the same `KelpPreviewServer` through
  `pageInfo`/`previewPage`; `KelpClient` exposes the two strings that seam takes
  (`bundleId`, `secret`) and clears them on `reset()`; `KeepAlive` is held while any preview
  session is live, not only while a share is served. Nothing is persisted, so §12.13 stays
  open. Verified by fetching a real bundle on the emulator and `curl`-ing the session there
  (no browser to hand it to), then with a real browser on the C2.
- **C4a.2 — view-only webpage links (§5.5, recipient half).** **Built; verified on the emulator and
  through a real browser on the C2.** For a link carrying `p`, the Open-link page shows no *Save to* and no *Download*:
  one **Open in browser**, fetching into a session-owned `MemoryStore` and opening the preview
  on the same single `KelpPreviewServer` — whose `open()` takes the store, so a session becomes
  a store plus a key, and one listener still serves shares and received pages alike. The store
  dies with the session, so nothing is left; a ceiling refuses an oversized page before fetching
  and names the workaround; `KeepAlive` is held while the session is live. Verified on the
  emulator by fetching a real page link and `curl`-ing the session (and checking the app's store
  never grew), and then **from a real browser on the C2** (2026-10-18): the page came up in the
  browser and nothing was written. The C2 pass also showed what §12.11 is about: the demo page had
  been fetched from a site and expects Tailwind and an icon font from two CDNs, so with the
  preview's CSP honouring the recommendation — no off-origin subresource, ever — it rendered
  *unstyled*. Its content and its links are all there; what it wants from other hosts is not.
- **C4b** — Relay gateway (§5.2): the decrypting loader, the relay's `/b/<bundle-id>` route,
  ALPN `http/1.1` beside `kelp/1`, a browser-acceptable certificate, and verified
  in-browser decryption of a real bundle.
- **C5** — UI integration (pages/cover/settings), encryption UX, passphrase links.
- **C5.1 — share kinds (§5.5, publisher half).** **Built and verified on the emulator.** The
  main-page button is **Share…**; the page it opens carries three actions — *Share file*, *Share
  folder*, *Share webpage* — each sealing one entry, with the staging list retired. *Share webpage*
  insists on a §5.3 page, evaluated on the folder's names before sealing, through the one pure
  chooser the manifest side also uses (`Kelp::Web::chooseEntry`, shared with `KelpFiles::pageEntry`).
  A page share puts `p` in the sealed `q=` map (wire spec §5) and a `page` flag in its `shares.json`
  row, so `republish()` re-mints it on the next launch; `linkInfo` reports it. Verified: sealing a page
  folder logs the flag and the minted link decodes with `p` (including a page-only link with no
  address at all); a folder with no HTML is refused with the wording that points at *Share folder*; a
  share that survives a restart comes back as a webpage share (`page: true` in `shares.json`, and the
  restored link decodes with `p`); and a link with no `p` — every link already out there — behaves
  exactly as it does today. The same pass caught two bugs: `saveShares()` was not persisting the flag
  at all, and the list's `state` role was being shadowed by `Item::state`, so every row read
  "Offline" and a paused share never offered *Resume sharing* (the role is now `shareState`).
- **C6** — Stewards/channels/CID+IPFS export bridge (optional).
- **C7** — Swarm mode: peer discovery via relay, have/want wire, rarest-first; measure
  origin-offload on a real 5–10 peer swarm (optional, and only if C3 shows the relay is a
  bandwidth bottleneck).

**Not on this list: ephemeral rooms (anonymous chat).** Appendix B sketches it — a
`kelpchat://` link that opens a room with nothing stored anywhere — and it is explicitly
parked: no part of it is to be built, or designed further, until the app itself is otherwise
finished and released. It is written down so that the decisions above can be checked against
it, and so the answer to "could Kelp do this?" does not have to be re-derived.

> **Session records.** Dated progress/handoff notes live beside this file as
> `PROGRESS-<date>.md` — what was run, what was verified, what broke, and the recipes and
> traps that go with it. The newest is [`PROGRESS-20261016.md`](PROGRESS-20261016.md): the
> first real device-to-device LAN fetch, plus three bugs (a Sailjail data-directory bug
> that stopped a fresh install writing anything, the link not being recoverable without
> `KELP_DEBUG_LINK`, and a LAN token-encoding regression in the `KelpSource` refactor).
> This file stays the design record; those files are the log.

> **Constraint for C2/C3 (checked at C2a):** on Sailfish 5.x the linker warns that
> `libQt5Network.so` pulls in `libcrypto.so.1.1` while `libkelp` links `libcrypto.so.3`.
> The device in fact already loads *both* (sailfishapp brings in Qt5Network regardless),
> and Kelp's `EVP_*`/`EVP_KDF` work still resolves to 3.x and works — the direct dependency
> wins symbol resolution, so the two coexist. The split stands: Qt owns sockets, libcrypto
> owns content, and no content crypto goes through Qt.
>
> **TLS re-checked for C3 (done, against the 5.1.0.11 target, Qt 5.6.3):** `QSslSocket`
> cannot meet the swarm spec's §7.1 transport on two counts, both proved by compiling
> against the target headers and running a probe in the build engine:
> - **No TLS 1.3.** `QSsl::SslProtocol` stops at `TlsV1_2OrLater` — there is no `TlsV1_3`
>   enumerator — so the default `SecureProtocols` tops out at TLS 1.2. Qt was built against
>   and loads OpenSSL 1.1.1w, which *can* speak 1.3; the ceiling is Qt's API, not the crypto
>   library.
> - **No client-side ALPN.** `qsslconfiguration.h` exposes only `nextNegotiatedProtocol()`
>   (read side); there is no `setAlpnProtocols()`, and `libQt5Network` exports no ALPN
>   symbol, so a Qt 5.6 client cannot offer `"kelp/1"`.
>
> So a C3 relay tunnel on Qt 5.6 would be **TLS 1.2 with no ALPN**.
>
> **Decision (C3): hand-roll the tunnel over `libssl.so.3`, keeping TLS 1.3 + ALPN
> `"kelp/1"`.** The two options were to accept TLS 1.2 and carry the protocol version in
> the URL instead of ALPN, or to get 1.3 + ALPN by driving OpenSSL 3's `libssl` directly.
> The second was chosen: the spec's transport is worth keeping, `libssl.so.3` is already on
> the device (and `libkelp` already binds `libcrypto.so.3`), and the crypto is the part we
> least want to improvise — this uses OpenSSL's TLS implementation, only the event-loop
> integration is ours. Qt's roles stay split as before: Qt owns sockets and the LAN path,
> `libssl` owns the relay tunnel. The endpoint is still `swarm_token`-gated and the payload
> still ciphertext, so the tunnel remains a transport-metadata concern, not a content one.
> New app code owns this (not `libkelp`, which stays pure): the TLS transport lives in the
> app layer beside `KelpServer`/`KelpClient`.

---

## 12. Open design questions

1. **Chunking** — fixed 256 KiB first, or invest in content-defined chunking for dedupe
   and cheaper re-shares immediately?
2. **Relay posture** — ship with a default community relay (convenient, some
   centralization) or require the user to supply one (maximally sovereign, worse first-run)?
3. **Passphrase links** — PBKDF2 (zero deps) or embed Argon2id (better, ~adds code)?
4. **Sizes/timing privacy** — do we pad chunks to hide size distributions, or accept the
   leak in v1?
5. **Relationship to the IPFS app** — is Kelp the *replacement* (drop IPFS entirely) or the
   *default backend* with IPFS kept as an optional power-user backend behind the same UI?
6. **Swarm posture** — *(partly resolved: a **per-share** "let peers help" toggle, **off by
   default**, enabled only when a global "Allow peer swarm" permission is on. Encryption stays
   always-on regardless — peers trade ciphertext, so swarm is a *distribution* choice, not a
   visibility one; there is no "public bundle".)* Still open: real BitTorrent-v2 interop, or just
   the internal swarm?
7. **Store lifetime** — *(resolved: Settings → **Storage** shows what the store holds and how
   much of it the shares on this device need, and a **Remove unused objects** button deletes
   exactly the rest. There is no per-bundle Remove and no LRU: the store is one deduplicated
   pool, so "unused" can only be computed against the whole root set at once — every share's
   object graph, walked in full. An object no share reaches is either what a fetch left behind
   or what a share that is gone no longer needs; downloads themselves are kept as ordinary files
   in Downloads, so deleting their cached objects costs a re-download, not the content. The walk
   is strict: a share whose manifests cannot be read fails the whole operation, because "not in
   the set" must mean "safe to delete" and an object that merely cannot be accounted for is not
   junk. See §11 and `Kelp::bundleObjects`, `KelpBundles::storeUsage/pruneStore`.)*
8. **Background serving** — *(partly resolved, 2026-10-17.)* Two questions were
   hiding under one word.

   *Does serving survive the window going away?* Yes — and it always did. Sailfish
   keeps a backgrounded app genuinely alive (the cover view is a live process, not
   a snapshot), so nothing in the app had to change for serving to continue while
   Kelp sat on its cover. The part worth adding is the *request* not to be ended,
   and the platform supplies it: `Nemo.KeepAlive`. Kelp holds it exactly while
   something is really being served and *Share in background* is on, and releases
   it otherwise. Verified live with the app backgrounded (PROGRESS §3.17).

   *Should it?* is the part that is still design, and it is a **policy**, not a
   service: what to do off Wi-Fi, below a battery threshold, or while the screen
   sleeps. None of that is implemented, the switch that gates the keepalive is
   not persisted, and `wifiOnly` is still a placeholder.

   *And once the app is closed?* Nothing — and nothing can be, from a Harbour app:
   an always-on seeder is a systemd **user service**, which is OpenRepos territory,
   off by default even there, and a separate battery/privacy commitment. The
   Settings label used to promise "keep seeding after the app is closed", which was
   never true; it now says what is true.

   Pause/resume is the other half of the same picture: *Stop sharing* is
   destructive (it deletes the row), so a share can be paused instead — serving
   stopped, row and objects kept, `paused` recorded in `shares.json`, resumed with
   the same `republish()` that restores a share at startup. That pair is the
   non-destructive half of the serving controls, and the shape background serving
   needed.

9. **Preview route** — the system browser (recommended: browser affordances for free, no new
   dependency) or an embedded `Sailfish.WebView` (stronger leash via a request interceptor and
   no keepalive, but a heavy dependency and QtWebEngine under Sailjail is unproven)? §5.1
   recommends building the server first and deciding with it on the device. *(On-device note:
   the emulator image ships **no browser at all**, and Qt 5.6's `Qt.openUrlExternally` returned
   `true` while nothing opened — its result says the URL was handed over, not that anything
   will show it. So route B silently depends on a browser being installed, which the emulator
   cannot settle; the C2 can.)*
10. **Preview trust** — ask before every preview, or ask once per bundle id and remember the
   answer? *(Recommended: ask once per bundle, rememberable — content addressing makes the
   decision durable, because the same id is the same immutable bytes; a remembered "trusted"
   can never be swapped for different content. Deferred by the first cut: what is opened today
   is the sender's own share, and a prompt over your own content is noise. The prompt belongs
   with §12.13, when the page is someone else's. **Still parked after §5.4:** fetching someone
   else's bundle and pressing *Open in browser* is a fresh, explicit choice every time, and
   nothing is remembered for a prompt to be about; the prompt is for the day a received bundle
   is kept.)*
11. **A page's network reach** — offline forever, enforced by the CSP in §5.1 *(recommended)*,
   or an explicit per-preview "let this page use the internet" switch? *(2026-10-18: the first
   real-browser pass makes the trade visible. A page saved from a website and shared as it stands
   expects the site's own CSS and fonts — Tailwind and an icon font from two CDNs here — and
   `default-src 'self'` refuses all of it, so the page *renders unstyled*. That is the recommended
   answer doing its job: the recipient's device never talks to a third party the sender chose, and
   the sender's page cannot learn that it was opened (which is what a CDN request would tell it).
   The demo problem is better solved by sharing a self-contained page. If the switch is ever built,
   it belongs with the sender's declaration and the recipient's consent — §5.5 already established
   that a page share is a *kind* the sender declares and the reader can see — never silently in the
   preview.)*
12. **Download-all in the gateway** — a ZIP assembled in the browser (no `CompressionStream`
   guarantee in the engine; STORE + CRC32 in JS works), or per-file downloads only?
13. **Keeping what a recipient receives** — a bundle fetched from a link is written out as
   ordinary files in Downloads, and the app keeps no key and no record of it: its objects are
   stored while it is verified (§11 C2b), but nothing points at them once the fetch ends, so
   they fall to *Remove unused objects* (§12.7). So a recipient cannot preview the bundle
   (§5.1), re-download it, or help seed it, and *the* flow C4a was designed for — "the recipient
   opens the page" — cannot be tried on a real device. Keeping the row (id + key, in
   `shares.json` or a received list of its own) is what would fix that, at the cost of holding a
   key this device never minted and of a bundle list that mixes "shared by me" with "received".
   *(Leaning: keep received bundles as rows that can be paused and removed and are marked as
   received, because the alternative is a preview that only ever shows you your own page.
   **2026-10-17: the first half is answered without keeping anything** — §5.4 offers the preview
   for the one session a download just finished, so a recipient does see the page, with no key on
   disk and nothing added to the list. What keeping the row would still add: a preview that can be
   re-opened later, a re-download without the link, and seeding a received bundle. And §5.5's
   webpage links step around the question for the case a publisher can declare: view-only, held in
   memory for the session, so there is nothing on disk either to keep or to prune.)*

*(Resolved: name **Kelp**, app id `harbour-kelp`, native link scheme `kelp://` — kept.)*

---

## Appendix A — Swarm wire protocol (sketch)

> **Full wire format** (exact CBOR schemas, framing, handshake, error codes, test vectors):
> see `kelp-swarm-wire-spec.md`.

Concrete sketch of §3.7. Deliberately small: it reuses the object layer (verification),
CBOR, and the existing TLS/HTTP stack, and adds only discovery + a 6-message peer wire.

### A.1 Key derivation (this is why the swarm needs a master key)

The link fragment carries a **32-byte master secret**; everything else is derived:

```
m           = fragment secret (32 bytes)
content_key = HKDF-SHA256(m, info="kelp/v1/content")   # encrypts/decrypts objects
swarm_token = HKDF-SHA256(m, info="kelp/v1/swarm")     # gates swarm membership
```

- The relay is given only `swarm_token` (an opaque bearer token) — never `content_key`.
  Since they're independent HKDF outputs, a relay (or a leaked token) still cannot read
  content.
- Anyone with the link derives both; anyone without it **cannot join the swarm**, so
  non-recipients cannot learn a participant's IP address.

### A.2 Tracker exchange (the relay, extended)

```
POST /swarm/{bundle_id}/announce
     Authorization: Bearer <swarm_token>
     body: { peer_id, addrs:[...], have: <bitfield|range-summary>, complete: bool }
  -> { interval: 120,
       peers: [ { peer_id, addrs:[...], complete: bool }, ... ] }   # a bounded subset

GET  /swarm/{bundle_id}/peers   (same auth)  ->  peers[]
POST /swarm/{bundle_id}/bye                    ->  204
```

- The relay holds a per-bundle swarm set keyed by `bundle_id`, gated by `swarm_token`.
- It introduces peers but never needs the content key, and need not carry bulk traffic.
- Optional push over the existing publisher tunnel: `PEER_JOIN` / `PEER_LEAVE`.
- If a relay is unavailable, peers can bootstrap from `addrs` carried in the link, and
  optionally gossip via `PEX` (A.3).

### A.3 Peer wire (direct, over TLS or QUIC)

Frame: `u32 length | u8 type | CBOR payload`. One connection carries one bundle.

| Type | Dir | Payload | Meaning |
|---|---|---|---|
| `HELLO` | → | `{peer_id, bundle_id, v}` | open the connection |
| `BITFIELD` | → | `{have}` | objects held (1 bit per canonical object index) |
| `HAVE` | → | `{i}` | newly acquired object `i` |
| `WANT` | → | `{i}` | request object `i` |
| `DATA` | → | `{i, b}` | object bytes — **receiver verifies SHA-256 == `i`** |
| `REJECT` | → | `{i}` | don't have it |
| `CANCEL` | → | `{i}` | cancel a pending `WANT` |
| `PEX` | → | `{peers[]}` | optional peer exchange (gossip) |
| `CHOKE`/`UNCHOKE` | ⇄ | `{}` | optional tit-for-tat — **skipped in v1** |

- Objects are indexed in canonical manifest order; `have` is a compact bitfield for typical
  bundles, or a range-set for very large ones.
- Every `DATA` is hash-verified, so a peer can only *withhold*, never *corrupt*.
- Fetch order: manifests first (so you can list files immediately), then rarest chunks.

### A.4 Scheduling

- **Rarest-first** from aggregated availability (peers broadcast bitfields; the relay may
  publish a compact availability summary to save bandwidth).
- Sequential prefetch when the UX is "stream/peek" rather than "download whole bundle".
- No choking/tit-for-tat in v1: membership is capability-gated, so freeloading is not the
  threat model.

### A.5 Per-object fallback ladder

```
direct peers (rarest-first)  →  steward holding it  →  relay cache  →  relay → origin tunnel
```

Identical verification at every hop, so any mix of sources — and any source failing —
degrades gracefully.

### A.6 What the user actually sees

Not a global "node list." Membership is per-share, so the UI is a **Sources panel inside a
bundle's page**:

| Row shows | Backed by | Example |
|---|---|---|
| The bundle itself | `bundle_id` (SHA-256, base32) | `c1vq…` (~52 chars) |
| Your phone (origin) | your `peer_id` | `7d97e9…` |
| Relay / cache | relay id | `relay.example` |
| Direct peers (swarm) | each `peer_id` + transport | `⬆ 2.1 MB/s · LAN` |
| Stewards | `peer_id` | `⬇ seeding` |
| Optional IPFS/pinning source | IPFS peer id / gateway | `ipfs.io (bridged)` |

Three different identifiers exist and shouldn't be conflated: **`bundle_id`** (one per
share), **object hashes** (many; shown only in a "details/verify" view), and **peer IDs**
(one per device in a swarm). A 40-hex string like `7d97e98f…` is a SHA-1-era BitTorrent v1
infohash/peer ID; Kelp uses **SHA-256** (~52-char base32), which is also what BitTorrent v2
uses — one reason v2 is the interop target, not v1.

---

## Appendix B — Ephemeral rooms (anonymous chat)

> **Status: a possible future feature — parked, and explicitly not to be started before the
> app itself is otherwise finished and released.** C1–C7 (§11) closed, a release build
> published, and the shared-page preview behind us: only then is this a candidate, and only
> if the user still wants it. Nothing below is implemented, promised, or specified to the
> standard of the rest of this document or of `kelp-swarm-wire-spec.md`; if it ever graduates
> it gets its own wire-spec document, the way the swarm did. It is written down because the
> question was asked — send a link, a chat opens, nothing is stored anywhere, it lives only
> until the phone reboots — and because the answer is instructive: **the transport and the
> link fit it almost unchanged, while the content model is its exact opposite.**

### B.1 The shape

Two people, one link. The initiator mints a room and sends it however they like (a message, a
QR code, a spoken-out URL); the other opens it in Kelp; a chat page appears and they talk.
The room lives in the two apps' memory and in the relay's memory, and when the app closes or
the phone reboots it is gone: no history for a late joiner, nothing held for someone who was
offline, nothing to find afterwards. That property *is* the feature.

### B.2 Why it is a sibling protocol, not a feature of this one

- The content layer is **immutable, single-writer and content-addressed** (§1.1); a room is
  mutable, multi-writer and addressed by nothing.
- The peer messages (wire spec §9) are all object-oriented, and the relay path (wire spec
  §12.1) is **pull-shaped**: a recipient's `GET` becomes a `WANT` down the publisher's tunnel,
  and nothing is ever pushed *to* a recipient. A room needs fan-out.
- This document's §10 lists multi-writer mutable state as a non-goal.
- **The id stops being a hash** (B.3) — the first Kelp identifier that is not derived from its
  content.

What carries over, unchanged: the link as a capability (§2); the HKDF-separation principle,
and specifically **C3.2's rule that the relay gets a credential of its own and never the
content key**; the dial-out tunnel and its NAT answer (§3.2, wire spec §12.1); TLS 1.3 + ALPN
`kelp/1`, the `len ‖ type ‖ CBOR` frame and the handshake MAC's shape (C3.1/C3.2); the object
envelope (§1.1), so that a message can simply *be* an object; and the relay's posture
"withhold, not substitute" (wire spec §18), extended here to "…and inject junk, but never read
or forge".

### B.3 Keys and the link

```
m          = fragment secret (32 random bytes, fresh for every room)
chat_key   = HKDF-SHA256(m, salt=∅, info="kelp/v1/chat")   # encrypts messages
room_token = HKDF-SHA256(m, salt=∅, info="kelp/v1/room")   # the relay's own credential
```

Two independent outputs, for the reason C3.2 gives: the relay needs a bearer token to route
and to keep non-recipients out, and if that token *were* the message key, the relay could read
the room. Kept separate, the relay routes what it cannot read.

The **room id is 32 random bytes — the first Kelp id that is not a content hash.** Nothing
about a room is content-addressed, so the id is only a name, and two rooms may even share one
if their secrets differ. That is the conceptual break, and the reason the link is a **second
scheme**:

```
kelpchat://<room_id_b32>?r=<relay>#k=<secret>
```

rather than a `t=` on a `kelp://` link. A second scheme describes itself in a chat preview, and
an older Kelp simply does not handle it — instead of decoding a room as a bundle and failing
somewhere confusing. The desktop file gains a second `x-scheme-handler`; `Open a link` routes
on the scheme.

### B.4 Messages

A message **is an object** (wire spec §2): `Object::seal(chat_key, plaintext, fresh nonce)`,
the plaintext a canonical CBOR map. Reusing the envelope means `libkelp` is reused whole —
seal/open, CBOR, the 1 MiB cap — the relay handles opaque blobs, `object_id` gives integrity
for free, and a replay inside a session is recognised by its id.

```
message (plaintext CBOR, inside the object envelope)
  { "v":1, "seq":<uint>, "from":<session id, 16 random bytes>,
    "n":<display name?>, "t":<text?>, "o":<object_id?> }
```

- `seq` is per sender and monotonic, for ordering; there is no global clock and no
  authoritative order, so display is `(arrival, seq)` per sender.
- `from` is the sender's **session** id (random, per join), not an identity (B.6).
- `"t"` and `"o"` let one frame carry text, a file, or both.

Transport frames — a new type block, which wire spec §17 already sanctions (unknown types are
ignored, not fatal, so an old build does nothing rather than breaking):

| Type | Message | Meaning |
|---|---|---|
| `0x40` | `ROOM_JOIN` | `{room_id, tok, from, n?}` — authenticate, enter |
| `0x41` | `ROOM_MSG` | `{b:<object bytes>}` — fanned out to the room |
| `0x42` | `ROOM_LEAVE` | `{}` — the relay drops the member |
| `0x43` / `0x44` | `ROOM_PING` / `ROOM_PONG` | liveness, as §9's PING/PONG |
| `0x7f` | `ERROR` | reused |

There is no acknowledgement, no retransmission queue and no receipt — **fire and forget** —
because there is nowhere to retry from.

### B.5 The relay's part: rooms, not storage

- A room exists while at least one member is connected and ceases to exist when the last
  leaves: **memory only, no queue, no cache, no store.** That is compatible with what the relay
  already is (in-memory tunnel registry; the *cache* is C6 and unbuilt), and it is the whole
  "stored nowhere" promise — after the fact there is nothing to seize.
- Fan-out: a `ROOM_MSG` from one member is written to every other connected member of the room.
- Caps matter more here than for bundles, where a relay needs only quotas: a maximum message
  size (the envelope's 1 MiB), members per room, messages per second per member and per IP,
  rooms per relay, and an idle-room lifetime of exactly zero. With no store, a hostile room can
  burn only CPU and bandwidth, and these bound both.
- **What the relay can do:** learn the room id, see who is connected and when, see message
  sizes and timing, refuse to route, drop messages, inject junk (undecryptable without
  `chat_key`). **What it cannot:** read a message, forge a readable one, or produce a
  transcript after the fact.

### B.6 What "anonymous" means here — and does not

- **Membership is the link.** No accounts, no roster, no kicks and no revocation: any holder
  can join, read and write, and removing someone means minting a new room and link.
- **One shared key means no attribution.** `chat_key` is held by everyone in the room, so any
  participant can produce a message that decrypts and verifies while claiming any name and any
  `from`. A display name is a label, not an identity. Making it real needs a per-device
  ephemeral keypair, exchanged in-band, pinned for the session (trust on first use), with a
  short out-of-band comparison (a SAS phrase) for the careful — a feature of its own, and one
  this appendix does not attempt to specify.
- **No scrollback, no offline delivery.** A joiner sees only what arrives after they join; a
  message sent to an empty room is lost. Bridging a sleep means the relay holding RAM for N
  minutes — the smallest possible change, and it is, briefly, storing.
- **Metadata flows.** The relay learns the room, the members, addresses, sizes and timing; on a
  LAN-direct path the two peers see each other's IPs (wire spec §18). This is content
  confidentiality, not participant anonymity, and nothing in Kelp provides the latter.
- **Forward secrecy is the one cheap upgrade.** An ephemeral ECDH (X25519 is in the OpenSSL 3
  already linked, and is Harbour-allowed) whose transcript is discarded means a relay that
  hoarded ciphertext can never read it. Small new crypto in `libkelp`, vector-tested — and
  optional, because a room that dies with the process has little history to protect.

### B.7 The app side

Room state lives in RAM only: no store, no file, no persisted room id or link, and **no message
content in `kelp.log` in any build** — a stronger rule than the dev-extras bundle, where a
*link* may be logged while developing. The app holds `Nemo.KeepAlive` while a room is open,
exactly as it does while a share is served, so backgrounding does not end the chat; closing the
app or rebooting does, which is the promise being made.

### B.8 What it would cost, if it ever graduates

A rough shape, not a plan: the scheme and link extension (small); the room frames and a client
state machine (moderate); relay rooms and fan-out (moderate, and a genuinely new relay
capability); a Chat page and its lifecycle (moderate); optional key agreement and SAS
(small–moderate). It depends on C3's tunnel and relay, not on C4. It is not smaller than C4a,
and it is the only sketched feature that adds a mutable, multi-writer protocol to a system whose
entire design is single-writer and immutable — which is the real reason it waits.

### B.9 Deliberately undecided

Ordering across senders (any causal or wall-clock claim), attribution (per-device keys, TOFU,
SAS), the sleep-bridging buffer (if any), whether a room may hold more than two people (a group
room is this protocol with a bigger fan-out and the same anonymity problems), file transfer
inside a room, notification behaviour, and whether rooms are a mode of `kelp-relay` or a
separate binary.
