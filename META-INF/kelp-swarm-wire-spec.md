# Kelp Swarm Wire-Format Specification (v1 draft)

Status: **design draft.** Concretises `kelp-design.md` §3.7 + Appendix A into exact wire
formats. Constants are provisional but internally consistent; anything marked *(TBD)* needs a
decision before implementation. Scope is the **Kelp swarm only** — the IPFS backend and the
BitTorrent-v2 bridge are out of scope here.

---

## 1. Conventions

- **Byte order:** all fixed-width integers on the wire are **big-endian**.
- **Serialisation:** CBOR (RFC 8949), **Core Deterministic Encoding** (§4.2.1):
  - definite-length items only;
  - shortest-form integers;
  - **map keys sorted bytewise by their encoded bytes** (this reorders schemas below — key
    order in the JSON-ish examples is *not* the wire order);
  - no floating-point; no tags; text strings are valid UTF-8.
- **Names** in manifests MUST be **NFC-normalised**; duplicates after NFC are invalid.
- **Binary values** are CBOR byte strings unless stated. In human-facing text (links, UI)
  they are **base32 (RFC 4648, lowercase, no padding)** for 32-byte ids, and **base64url
  (unpadded)** for secrets/tokens.
- Examples use JSON-ish notation for schema readability; the wire is always CBOR.

### 1.1 Primitives

| Name | Definition |
|---|---|
| `hash` | SHA-256, 32 bytes |
| `bundle_id` | object hash of the root manifest (32 B) |
| `object_id` | SHA-256 of the **full object bytes** (envelope included) |
| `peer_id` | SHA-256 of the peer's Ed25519 static public key (32 B) — a name, not a trust anchor |
| `secret` | 32-byte master secret (from the link fragment) |
| `content_key` | `HKDF-SHA256(secret, salt=∅, info="kelp/v1/content", L=32)` |
| `swarm_token` | `HKDF-SHA256(secret, salt=∅, info="kelp/v1/swarm", L=32)` |

`∅` salt = 32 zero bytes (RFC 5869 default). `content_key` and `swarm_token` are independent;
the relay only ever learns `swarm_token`.

### 1.2 Constants

| Constant | Value | Note |
|---|---|---|
| `KELP_VERSION` | `1` | carried in every handshake/manifest |
| `CHUNK_SIZE` | `262144` (256 KiB) | fixed-size chunking in v1 |
| `MAX_MANIFEST_BYTES` | `262144` | cap on a manifest's plaintext CBOR |
| `MAX_OBJECT_BYTES` | `1048576` (1 MiB) | hard cap on any object (envelope incl.) |
| `MAX_FRAME_BYTES` | `4194304` (4 MiB) | peer-wire frame cap |
| `MAX_PEERS` | `64` | peers returned per tracker call |
| `ANNOUNCE_INTERVAL` | `120` s | default, relay may override |
| `T_HS` / `T_IDLE` / `T_DATA` | 10 s / 120 s / 30 s | handshake / idle / request timeouts |
| `TLS_ALPN` | `"kelp/1"` | |

---

## 2. Content objects

Every object — manifest or data chunk — is the same envelope:

```
object = 0x01 || nonce[12] || ct
```

- `0x01` — envelope version.
- `nonce` — 12 random bytes, fresh per object.
- `ct` — `AES-256-GCM(content_key, nonce, plaintext, aad = "kelp/v1/object")`, tag appended
  (16 B).

`object_id = SHA-256(object)`. Because the id is over **ciphertext**, there is no
known-plaintext confirmation oracle, and a relay/gateway sees only random bytes.

`plaintext` is either:
- **data** — raw file bytes (a slice of a file ≤ `CHUNK_SIZE`), or
- **manifest** — canonical CBOR (below).

The type is implicit: a manifest references its data objects; the object itself carries no
type tag. This keeps one noun (`object`) and one verb (`get by id`).

---

## 3. Manifest schemas

### 3.1 File manifest

```
{ "v": 1, "t": "f", "s": <uint>, "c": [ <hash>, ... ], "mime": <text, optional> }
```

- `s` — file size in bytes; chunk count = `ceil(s / CHUNK_SIZE)`; last chunk may be short.
- `c` — ordered data-object ids.

### 3.2 Directory manifest

```
{ "v": 1, "t": "d", "e": [ <entry>, ... ] }        # leaf form
{ "v": 1, "t": "d", "c": [ <hash>, ... ] }         # segment form (large dirs)
```

`entry`:

```
{ "n": <text NFC>, "t": "f" | "d", "h": <hash>, "s": <uint> }
```

- `h` — object id of the child manifest.
- `s` — total subtree size (file size, or sum over a directory) so the UI can show sizes
  without full traversal.
- A directory is **either** a leaf (`e`, all entries fit in `MAX_MANIFEST_BYTES`) **or** a
  segment index (`c`, each child a leaf-form directory manifest). No mixing.
- **Canonical ordering:** entries within a leaf sorted bytewise-ascending by NFC name;
  segments ordered by their first entry's name. Empty directory = `{"v":1,"t":"d","e":[]}`.
- **Every bundle root is a directory manifest**, even a single-file bundle (one entry) — this
  guarantees a stable root and a name.

---

## 4. Canonical object indexing

Needed so availability can be expressed compactly (bit/range) rather than as hash lists.

- Let `m` = number of manifest objects, `d` = number of data objects, `n = m + d`.
- **Manifest order (BFS from root):** index `0` = root; expand a FIFO queue, where a
  directory contributes its child manifests in entry order (segment index, then leaf
  entries). A file manifest contributes no child manifests.
- **Data order** occupies indices `m … n-1`: ordered by the position of the owning file
  manifest in manifest order, then by chunk index within that file.
- Mapping: `i < m` → the `i`-th manifest; `i ≥ m` → the `(i-m)`-th data object.

A peer computes this after fetching the manifests — which it does first (§10), so the
index space is available before bulk transfer. Requests themselves use **hashes** (§11), so
index knowledge is only needed for compact availability reporting.

---

## 5. Link format

```
kelp://<bundle_id_b32>?r=<relay-url>[&r=<relay-url>…][&q=<sealed_hints>]#k=<secret_b64url>
```

- `bundle_id_b32` — base32, lowercase, unpadded (52 chars).
- `r` — relay base URL, repeatable (try in order).
- `q` — the publisher's own hints: where it serves from, the label, and its kind (sealed, below).
- Fragment `k` — the base64url secret. **Fragments are never sent to servers.**

`q` carries the publisher's LAN `host:port` and the share's display label together, so
the link text names neither to anything that cannot open it:

```
q = base64url( nonce(12) ‖ AES-256-GCM ciphertext ‖ tag(16) )

  key       = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/link")
  aad       = bundle_id (32 bytes)
  plaintext = canonical CBOR map { "l": <host:port>, "n": <label>, "p": true }
                                   (any of them may be absent)
```

`l` is where the bundle is served from and `n` is the share's display label. `p` is the
publisher's **kind**: true means "I shared this as a page", which the recipient's app answers by
viewing rather than saving (design §5.5). It is a hint on the recipient's flow and nothing more —
unknown map keys are ignored (§17), so a reader that predates `p` sees an ordinary share, and a
rewritten client may ignore it. A page share mints `q=` even when it has no address for `l`, so
the hint survives a relay-only link.

The key is an independent HKDF output of the link's own secret — not the content key and
not the swarm or relay token — so the fragment is what opens the block, and every intended
recipient holds the fragment. The bundle id is the associated data, so a block cannot be
lifted out of one link and pasted into another. This does not hide the address from the
recipient, who must dial it; it hides it from everything that sees the link *without* its
fragment — a redacted paste, a chat link preview, a log, a browser gateway.

Links minted before this carried the same two hints in the clear, as `l=` and `n=`, and
decoding still accepts that form, so published links keep working:

```
kelp://<bundle_id_b32>?r=<relay-url>[&l=<host:port>][&n=<display name>]#k=<secret_b64url>
```

A link that carries both uses the sealed block. A reader that predates `q=` sees no LAN
address and falls back to a relay.

A CID-shaped variant (`b` multibase prefix) is reserved for the IPFS bridge.

Implemented in `libkelp/kelp_link.{h,cpp}` (`Link::encode` / `Link::decode`), with
the base32/base64url codecs and round-trip tests. Decoding refuses a link with no
`#k=` fragment: without the key it is a name, not a capability. The app mints links
of this shape today (`r=` when a relay is configured, `q=` whenever it advertises an
address) and serves objects over the LAN (wire spec §7.2).

---

## 6. Tracker API (relay)

Media type `application/cbor`. Auth: `Authorization: Bearer <swarm_token_b64url>`.

### 6.1 `POST /v1/swarm/{bundle_id_b32}/announce`

Request:

```
{ "v":1, "pid":<hash>, "n":<uint total objects>,
  "hr":[ [start,count], … ],        # have-ranges over canonical index; sorted, disjoint, count≥1
  "complete":<bool>, "addrs":[<text>,…] }
```

Response:

```
{ "v":1, "interval":<uint seconds>,
  "peers":[ { "pid":<hash>, "addrs":[<text>,…], "complete":<bool> }, … ] }   # ≤ MAX_PEERS
```

### 6.2 `GET /v1/swarm/{bundle_id_b32}/peers` → same `peers` array.
### 6.3 `POST /v1/swarm/{bundle_id_b32}/bye` → `204`.
### 6.4 Errors → HTTP status + `{ "e":<code>, "msg":<text> }` (codes in §14).

The relay stores `{bundle_id → set of {pid, addrs, hr, complete}}` gated by `swarm_token`;
it never sees `content_key` and need not carry bulk traffic.

---

## 7. Transports

### 7.1 Peer transport (swarm)

- **Transport:** TLS 1.3 over TCP (or QUIC later), ALPN `"kelp/1"`.
  > **Implemented by hand over `libssl` (decided at C3).** Qt 5.6's `QSslSocket` offers no
  > `TlsV1_3` and no client-side `setAlpnProtocols`, so the tunnel does not use Qt's TLS:
  > it drives OpenSSL 3's `libssl.so.3` (already on the device, alongside the 1.1 stack
  > Qt5Network pulls in) directly, pinned to TLS 1.3 with ALPN `"kelp/1"`. See the note
  > under the C3 entry in `kelp-design.md`.
- **Certificate:** self-signed; peers accept any cert (content is hash-verified regardless).
  TLS protects metadata confidentiality; membership is gated by §8, not by PKI.
- **Frame:**

```
+----------+---------+---------------------------+
| len: u32 | type:u8 | payload: CBOR (len-1 B)   |
+----------+---------+---------------------------+
```

`len` counts `type` + payload, big-endian, `≤ MAX_FRAME_BYTES`. One connection carries one
bundle.

### 7.2 Direct object fetch (LAN-direct and origin tunnel)

The simple way to move bytes: ask for one object by id, with no framing, no scheduling and
no swarm state. Used by the `l=` fast path, and reused by the relay when it asks the origin
for an object over the publisher's outbound tunnel (§12) and by a relay cache or steward
holding a copy — all of them serve the same two requests.

```
GET /v1/bundle/<bundle_id_b32>/info                  → 200 application/json
GET /v1/bundle/<bundle_id_b32>/object/<object_id_b32> → 200 application/octet-stream
Authorization: Bearer <swarm_token_b64url>
```

| Status | Meaning |
|---|---|
| `200` | object bytes exactly as stored (a content object, §2) |
| `400` | malformed request, or headers larger than 8 KiB |
| `401` | `bad_token` — also what an unknown or no-longer-shared bundle id answers |
| `404` | `not_found` (no such object here, or no such endpoint) |
| `405` | method other than `GET` |
| `408` | nothing but an incomplete request arrived within 10 s |
| `503` | more than 32 connections were left unfinished |

The last two are the floor under §13: a peer on the Wi-Fi should not be able to
wedge the listener by opening sockets and saying nothing.

`info` → `{ "v":1, "objects":<n>, "files":<n>, "bytes":<n> }`, so a client can show what it
is about to fetch. JSON rather than CBOR on purpose: this is a control endpoint meant to be
read by hand with curl, and the object bytes are the hot path.

- The bearer token is the `swarm_token` of §1, derived from the link secret. It is therefore
  **not** in the link: deriving it *is* proving you hold the link.
- Requests name the bundle because that is the unit of sharing policy — an object is served
  only while its bundle is being shared. Whatever is served is still ciphertext that the
  receiver id-checks, so a server can withhold but cannot substitute.
- A bundle that is not shared, a wrong token and an unknown bundle id all answer `401`,
  so the endpoint cannot be used to enumerate what a device is serving.
- **Plain HTTP is deliberate.** Confidentiality does not depend on the transport (the
  content is encrypted and hash-verified), and a curl-able endpoint is what makes bring-up
  possible. The cost is that on-path observers on the same network learn the token, and
  therefore availability and the sender's address — but not the plaintext: the content key
  and the swarm token are separate HKDF outputs of the same secret (§1). The swarm transport
  in 7.1 keeps TLS, where peer addresses are handed around and the stake is higher.

A receiver uses only these two endpoints, and uses them in rounds. It asks for `info`, then
for the bundle's root manifest, then for whichever of the ids that manifest names it does not
have — a few at a time — and so on down the tree, since a manifest is the only way to learn
what is below it. Nothing is stored until it hashes to the id it was requested under, so a
server cannot answer a request with something else, and a partial or garbled answer leaves no
trace behind. That walk and that check are `Kelp::missingObjects()` and
`Kelp::storeReceived()` in `libkelp`, where they are unit-tested; the app supplies only the
network.

---

## 8. Handshake & authentication

Goal: prove membership (possession of `swarm_token`) so that only link-holders learn your IP
and availability; content integrity does not depend on this.

```
A → B   HELLO    { "v":1, "bid":<hash>, "pid":<hash>, "nonce":<4..16B> }
B → A   HELLO    { "v":1, "bid":<hash>, "pid":<hash>, "nonce":<4..16B> }
A → B   AUTH     { "mac": HMAC-SHA256(swarm_token, "i" || nonce_A || nonce_B || bid) }
B → A   AUTH     { "mac": HMAC-SHA256(swarm_token, "r" || nonce_B || nonce_A || bid) }
```

Both sides verify; mismatch → close with `ERR bad_token`. `bid` mismatch → `ERR bad_bundle`.
Version mismatch → `ERR version`. After auth, each side sends `HAVE_RANGES`.

---

## 9. Peer messages

| # | Type | Payload schema | Direction |
|---|---|---|---|
| `0x10` | `HELLO` | `{v, bid, pid, nonce}` | → |
| `0x11` | `AUTH` | `{mac}` | ⇄ |
| `0x12` | `HAVE_RANGES` | `{hr:[[start,count],…]}` | ⇄ |
| `0x13` | `HAVE` | `{i:<uint>}` | → |
| `0x14` | `WANT` | `{h:<hash>}` | → |
| `0x15` | `DATA` | `{i:<uint>, h:<hash>, b:<bytes>}` | → |
| `0x16` | `REJECT` | `{h:<hash>, why:<uint>}` | → |
| `0x17` | `CANCEL` | `{h:<hash>}` | → |
| `0x18` | `PEX` | `{peers:[{pid,addrs}]}` | ⇄ |
| `0x20` | `PING` / `0x21` `PONG` | `{t:<uint ms>}` | ⇄ |
| `0x7f` | `ERROR` | `{e:<uint>, msg:<text>}` | ⇄ |
| *reserved* | `CHOKE`/`UNCHOKE` (`0x01`/`0x02`) | — | not used in v1 |

- Requests identify objects by **hash**; `DATA` additionally carries the canonical **index**
  `i` so receivers update availability cheaply.
- Receivers MUST verify `SHA-256(b) == h` (and `h == object_id`); on mismatch → drop `DATA`,
  optionally `ERROR bad_data`, and MAY disconnect the peer.
- `HAVE` announces a newly completed object by index; peers re-derive ranges locally.
- `hr` uses canonical indices (§4): ranges sorted ascending, disjoint, `count ≥ 1`.

---

## 10. Scheduling

1. Fetch manifests first (BFS): request root, then child manifests as discovered, so the
   file tree and the index space (`m`, `n`) are known early.
2. Compute availability by OR-ing all peers' `hr` into a per-index counter.
3. Request data objects **rarest-first** from peers that have them; tie-break randomly.
4. Optionally request sequentially (index order) when the UX is "peek/stream".
5. On completion, send `HAVE` so others benefit; keep seeding while the app is sharing.
6. No choking/tit-for-tat in v1 (membership is capability-gated).

---

## 11. Per-object fallback ladder

```
direct swarm peers (rarest-first)  →  steward holding it  →  relay cache  →  relay → origin tunnel
```

Identical verification at every hop. Any source may be mixed per object, so partial
reachability still makes progress.

---

## 12. Relay-only mode

When direct peer connection is impossible (both sides NAT'd, no IPv6), the relay proxies the
swarm messages over the existing publisher/recipient tunnels. The wire format is unchanged;
only the transport differs. Correctness is identical; only bandwidth locality is lost.

### 12.1 Relay tunnel protocol (C3)

The publisher is always the client (design §3.2): it dials *out*, so no port-forwarding or
hole-punching is needed, and the relay never connects in.

**One address, two roles.** A relay listens on a single port, TLS 1.3 + ALPN `"kelp/1"`, and
serves both publisher tunnels and recipient fetches there; the role is decided by what arrives
first — an HTTP request line begins with an ASCII method letter, while a wire frame begins with
a 4-byte length (a NUL or another low byte). One advertised address is what a link's `r=` can
name, so the two cannot be separate ports without the link carrying both.

**Credential.** The relay is gated by a token of its own, *not* the swarm token:

```
relay_token = HKDF-SHA256(secret, salt=∅, info="kelp/v1/relay", L=32)
```

It is a third independent derivation (§1.1), so a relay that holds it learns nothing about
`content_key` or `swarm_token`. Anyone holding the link derives it; the relay is given it and
nothing else.

**Tunnel (publisher → relay).** TLS 1.3, ALPN `"kelp/1"` (§7.1), then one frame:

```
REGISTER (0x30)  { "bid":<bundle_id>, "tok":<relay_token>, "o":<objects>, "f":<files>, "b":<bytes> }
```

The relay records `{bundle_id → relay_token, shape, live tunnel}` and keeps the connection open
to carry `WANT`/`DATA`. A second REGISTER on the same tunnel registers another bundle. On a bad
token the relay answers `ERROR bad_token` and closes. `o`/`f`/`b` let the relay answer `/info`
without ever seeing a manifest (it has no content key).

**Recipient (HTTPS to the relay, same TLS 1.3 + ALPN `"kelp/1"` transport).** Same shape as
§7.2, but relay-scoped — the token is in the path, so it never travels in an `Authorization`
header the relay could conflate with the swarm token:

```
GET /v1/relay/<relay_token_b64url>/bundle/<bundle_id_b32>/info
GET /v1/relay/<relay_token_b64url>/bundle/<bundle_id_b32>/object/<object_id_b32>
```

- The relay checks the path token against the one registered for that bundle id. Mismatch,
  unknown bundle, and no-live-tunnel all answer the same way (`401`/`404`) so the relay is not
  an enumeration oracle.
- An object request becomes `WANT { "h": <object_id> }` down the bundle's tunnel; the relay
  returns the bytes from the matching `DATA` frame, or `REJECT not_found` → `404`.
- `503` when a tunnel is overloaded. The recipient verifies every object against its id exactly
  as on the LAN path, so the relay can **withhold but not substitute**.

**Link.** `r=` stays a plain base URL (§5); the holder of the link derives `relay_token` and puts
it in the path. The link format is unchanged, and no relay-scoped secret is written into it.

---

## 13. Limits & DoS protections

- Reject frames > `MAX_FRAME_BYTES`; reject objects > `MAX_OBJECT_BYTES`.
- Cap concurrent `WANT`s per peer (e.g. 64) and per connection; queue extras.
- Cap known peers per bundle; apply `MAX_PEERS` on tracker responses.
- Rate-limit `announce` per `swarm_token`/IP; per-response `interval` backpressure.
- Verify `hr` bounds (`0 ≤ start`, `start+count ≤ n`); ignore/discard invalid ranges.
- Verify `DATA` hash before use; never trust `i`/`h` consistency beyond the hash check.
- Drop peers that repeatedly send invalid frames; remember by `peer_id` (T_TTL).

---

## 14. Error codes

| Code | Name | Meaning |
|---|---|---|
| `1` | `bad_token` | wrong/missing `swarm_token` |
| `2` | `bad_bundle` | unknown/incorrect `bundle_id` |
| `3` | `version` | unsupported `v` |
| `4` | `bad_data` | `DATA` failed hash check |
| `5` | `bad_frame` | malformed/oversized frame |
| `6` | `not_found` | requested object unavailable at this peer |
| `7` | `rate_limited` | slow down (`interval` hint) |
| `8` | `internal` | relay/peer internal error |

`REJECT.why` reuses these (commonly `6`).

---

## 15. Sequence diagrams

### 15.1 Discover a swarm, then fetch

```
Fetcher A                     Relay                        Seeder B (origin/peer)
   |  POST /announce {hr:[],addrs} |                                |
   |----------------------------->|                                  |
   |  {interval, peers:[B,…]}     |                                  |
   |<-----------------------------|                                  |
   |                                                                 |
   |  TLS(ALPN kelp/1)  HELLO/AUTH  ───────────────────────────────► |
   |  ◄────────────────────────────  HELLO/AUTH                      |
   |  HAVE_RANGES ⇄                                                  |
   |  WANT {h} ────────────────────────────────────────────────────► |
   |  ◄────────────────────────────────────── DATA {i,h,b}          |
   |  (verify SHA-256(b)==h)                                         |
   |  HAVE {i} ────────────────────────────────────────────────────► |
   |  POST /bye                                                      |
```

### 15.2 Manifest-first traversal

```
root manifest (i=0) ─► child manifests (BFS) ─► know m, n ─► data objects rarest-first
```

### 15.3 Relay-only fallback

```
A ── GET /o/{h} ──► relay ──(tunnel)──► B        # no direct path; same DATA semantics
```

---

## 16. Test vector — directory manifest (canonical CBOR)

Plaintext value:

```
{"e":[{"h":h'000102…1f',"n":"a.txt","s":1234,"t":"f"}],"t":"d","v":1}
```

Canonical CBOR (deterministic key order `e<t<v`, inner `h<n<s<t`), hex:

```
A3
  61 65 81 A4
    61 68 58 20 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
                10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F
    61 6E 65 61 2E 74 78 74          # "n":"a.txt"
    61 73 19 04 D2                   # "s":1234
    61 74 61 66                      # "t":"f"
  61 74 61 64                        # "t":"d"
  61 76 01                           # "v":1
```

Full byte run (grouped for readability — strip whitespace to get the wire bytes):

```
A3 6165 81 A4 6168 5820
000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F
616E 65612E747874 6173 1904D2 6174 6166 6174 6164 6176 01
```

> The object envelope (`0x01 || nonce || AES-GCM`) is deterministic given a fixed key+nonce,
> but real ciphertext depends on `content_key`. The **hash** vectors that MUST be pinned
> are: SHA-256 of the CBOR above and of the empty-directory manifest `{"e":[],"t":"d","v":1}`:
>
> | vector | SHA-256 |
> |---|---|
> | directory manifest above (65 B) | `a10359058a699255134d971b806a1c383a46a3a77aaa8a5d867f4b9cecfac798` |
> | empty directory `a361658061746164617601` (11 B) | `81edc9892795bd2983a56faeded349074809fb87f899e8a453947ae3f8f51269` |
>
> **Object vector** (pinned in `libkelp/tests/tst_kelp.cpp`): with
> `content_key = 000102…1f` (32 bytes), `nonce = 000102030405060708090a0b` (12 bytes) and
> plaintext `"kelp object vector"`, the object is
>
> ```
> 01                        # envelope version
> 000102030405060708090a0b  # nonce
> 2c67ba6be58aa071e822e3abc78c1b19eca4eb392c7113e4c96d748088a6c6825897
>                           # ciphertext || tag (18 + 16 bytes)
> ```
>
> and `object_id = f3bfa0257b6d7a6a2dfe757ccac665541845d1e8d717ab58c290f0809160fd4d`.

---

## 17. Versioning & extensibility

- `v` in `HELLO`/manifests/tracker bodies is a major version; ALPN pins the protocol family.
- Unknown **map keys MUST be ignored** (forward-compatible); unknown **message types** MUST
  be ignored, not fatal.
- Reserved: symlink entries (`"t":"l"`), chunk-size negotiation, CDC chunking (new `"t"`),
  `CHOKE`/`UNCHOKE` tit-for-tat.

---

## 18. Security considerations

- **Integrity** never depends on peers, TLS, or the relay — every object is hash-verified;
  a malicious peer can only *withhold*.
- **Confidentiality** is E2E (`content_key`); the relay/gateway get only `swarm_token` and
  ciphertext, and HKDF separation means a leaked `swarm_token` cannot decrypt content.
- **Membership gating** stops non-recipients from joining a swarm and observing IPs.
- **IP exposure:** members still see each other's IPs (inherent to peering) — a per-share
  toggle; relay-only mode (§12) avoids it.
- **Traffic analysis:** object sizes/timing leak; padding is a v1 *non-goal* (§19).
- **Revocation:** stop publishing + delete ciphertext from relays/stewards; already-fetched
  objects cannot be recalled.

---

## 19. Explicit non-goals (v1)

- Global DHT / serverless discovery.
- Tit-for-tat / economic anti-leech.
- Cross-bundle or cross-user dedup.
- Size/timing padding.
- Symlinks, extended attributes, sparse files.
- Native BitTorrent-v2 interop (bridge only).

---

## 20. Open items *(TBD)*

1. QUIC vs TCP+TLS for the peer wire (QUIC helps NAT/IPv6; more code).
2. `hr` vs bitfield for very large bundles (`n > 2^18`): range-set streaming vs summary.
3. Multi-relay announce (fan-out) and relay selection order.
4. Steward/pinning semantics: who counts as a steward, and how it advertises other than via `announce`.
5. PEX policy (off by default for privacy?).
6. Peer identity: is `peer_id` needed at all, or is an ephemeral id sufficient?
