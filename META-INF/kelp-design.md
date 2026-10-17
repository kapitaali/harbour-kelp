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

## 5. Browser access (recipient with no app)

Because the transport is HTTP and objects are content-addressed, a recipient's browser
gets served by the **relay acting as a gateway**:

```
https://<relay>/b/<bundle-id>#<secret>
```

The relay returns a tiny self-contained **HTML+JS viewer** (also just an object) which:
fetches the (encrypted) root manifest, decrypts it with the fragment key via **WebCrypto
AES-GCM**, walks the tree, fetches chunks on demand, verifies each hash, decrypts, and
offers per-file download or "download all" (assembling a ZIP in-browser). The relay serves
ciphertext only; **decryption happens client-side** and the key never leaves the fragment.

This gives the same "just send them a link" UX as an IPFS public gateway — but the gateway
is *your* relay and it can't read anything.

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
unless you use per-fetch blinded ids or fetch through a steward.

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
4. **Browser viewer** — one self-contained HTML+JS object (WebCrypto + assemble/ZIP).
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
- **C4** — Browser viewer + gateway.
- **C5** — UI integration (pages/cover/settings), encryption UX, passphrase links.
- **C6** — Stewards/channels/CID+IPFS export bridge (optional).
- **C7** — Swarm mode: peer discovery via relay, have/want wire, rarest-first; measure
  origin-offload on a real 5–10 peer swarm (optional, and only if C3 shows the relay is a
  bandwidth bottleneck).

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
