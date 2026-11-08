# WS-C3: Peer swarm — Design (parked)

## Status

Parked. Not to be implemented before release. Design only.

## 1. Problem

The relay + caching (WS-C1) solve "scale to 10 downloaders" by offloading from the origin to the relay's cache. But for unpredictable popularity (a link posted to a forum), no cache is primed. Peer swarm lets downloaders help distribute chunks, reducing origin load to zero after the first few downloads.

## 2. What it adds

- **Peer discovery per bundle** — relay acts as tracker, hands fetchers the addresses of other downloaders (or pushes peer lists down existing tunnels). No DHT needed.
- **Bitfield exchange** — each peer advertises which chunk hashes it holds.
- **Request scheduling** — rarest-first (so swarm stays complete if origin leaves early); sequential prefetch for streaming.
- **Minimal peer wire protocol** — `HAVE` / `WANT` / `DATA` / `REJECT`. Tens of KB of code. No new content format, no new crypto.

## 3. Why it stays cheap and safe

- **Verification unchanged** — every chunk checked by hash regardless of source. A malicious peer can only withhold, never corrupt.
- **Encryption composes** — peers trade ciphertext; a peer can seed a bundle it cannot decrypt. Stewards seed blindly; relay never holds a key.
- **Origin offload + durability** — phone's upload cost drops as swarm takes over; bundle outlives the phone once enough peers hold it.

## 4. The catch (why opt-in)

- Peering between NAT'd phones needs connectivity — same LAN, globally routable IPv6, port-forward/UPnP, or hole-punching. When none available, traffic falls back through relay (correct, but no bandwidth saved).
- **Direct peering exposes each participant's IP** — swarm mode should be per-share toggle, off for sensitive shares, relay-only for privacy.

## 5. Wire protocol

### 5.1 Transport

TLS 1.3 + ALPN `kelp/1` (same as relay). Peer connections are dial-out from each side (NAT-friendly). Frame format: `len ‖ type ‖ CBOR` (same as wire spec §9).

### 5.2 Message types

New type block (wire spec §17: unknown types ignored, not fatal):

| Type | Name | Payload | Meaning |
|---|---|---|---|
| `0x30` | `HELLO` | `{peer_id, bundle_id, tok?}` | Introduce self; optionally authenticate to a peer's share |
| `0x31` | `HAVE` | `{chunks: [object_id, ...]}` | Bitfield: which chunks this peer holds |
| `0x32` | `WANT` | `{chunks: [object_id, ...]}` | Request specific chunks |
| `0x33` | `DATA` | `{id, b: <object bytes>}` | Chunk data (verified by recipient) |
| `0x34` | `REJECT` | `{id, reason}` | Cannot serve this chunk (don't have it, or rate-limited) |
| `0x35` | `CANCEL` | `{id}` | Cancel a pending WANT |
| `0x36` | `PEX` | `{peers: [{addr, port}]}` | Peer exchange: share known peers |

### 5.3 Flow

1. **Discovery:** Fetcher asks relay for peers for bundle B. Relay returns list of `{addr, port, peer_id}`.
2. **Connect:** Fetcher dials each peer. On success, sends `HELLO`.
3. **Bitfield:** Both sides exchange `HAVE` (chunk IDs they hold).
4. **Schedule:** Fetcher computes missing chunks, selects rarest-first, sends `WANT`.
5. **Transfer:** Peer responds with `DATA` (chunk bytes) or `REJECT`.
6. **Verify:** Fetcher checks chunk hash. On success, adds to store. On failure, blacklists peer.
7. **PEX:** Periodically exchange peer lists to grow the swarm.

### 5.4 Scheduling algorithm

- **Rarest-first:** For each missing chunk, count how many peers have it. Request chunks with lowest count first.
- **Sequential prefetch:** For streaming (WS-C2), request chunks in file order after rarest-first for the first file.
- **Parallelism:** Up to 4 concurrent WANTs per peer, 8 total across peers.
- **Peer scoring:** Track success rate and throughput per peer. Prefer fast, reliable peers.

### 5.5 NAT traversal

- **Same LAN:** Direct connection (reuse §3.1 LAN path).
- **Globally routable IPv6:** Direct connection.
- **Port-forward/UPnP:** Attempt UPnP; if successful, listen on forwarded port.
- **Hole-punching:** STUN-like via relay (relay tells peer its public address). Simultaneous open.
- **Fallback:** If no direct path, route through relay tunnel (no bandwidth saved, but correct).

### 5.6 Privacy

- Direct peering exposes IPs. Swarm mode is per-share toggle, off by default.
- Relay-only mode for sensitive shares (no peering attempted).
- Peer IDs are ephemeral (random per session), not tied to identity.

## 6. Implementation phases

### Phase 1: Peer wire protocol (M, 2-3 days)

Implement HAVE/WANT/DATA/REJECT/CANCEL/PEX message types in libkelp. Add peer connection manager (TCP/TLS). Test with two local instances.

**Files:** libkelp/kelp_wire.h, new libkelp/kelp_peer.h/.cpp

### Phase 2: Peer discovery (M, 2-3 days)

Extend relay to track peers per bundle and push peer lists. Add peer discovery to KelpClient.

**Files:** kelp-relay, src/kelpclient.cpp

### Phase 3: Scheduling + integration (L, 3-5 days)

Rarest-first scheduling, peer scoring, fallback to relay. Integrate with KelpClient fetch flow (prefer peers, fall back to relay).

**Files:** src/kelpclient.cpp, src/kelpfetch.cpp

### Phase 4: QML + settings (S, 1 day)

Per-share swarm toggle (already in AddPage.qml, gated by global setting). Peer status display.

**Files:** qml/pages/AddPage.qml, qml/pages/BundlePage.qml

## 7. Dependencies

- C1 (relay caching, done) — relay already caches objects
- Enables C4 (stewards) — stewards are peers that seed blindly

## 8. Risks

1. **NAT traversal complexity** — hole-punching is unreliable. Mitigation: relay fallback always works.
2. **Peer churn** — peers come and go. Mitigation: rarest-first ensures completeness; relay is always available.
3. **Malicious peers** — can withhold or inject junk. Mitigation: hash verification on every chunk; junk is rejected.
4. **IP exposure** — direct peering reveals IPs. Mitigation: opt-in, per-share toggle, relay-only default.
5. **Testing difficulty** — needs multiple devices. Mitigation: local test harness with simulated peers.

## 9. Not in scope

- DHT (relay acts as tracker)
- BitTorrent interop (optional bridge, §3.7)
- Stewards (separate workstream, C4)
- Anonymous chat (Appendix B, separate protocol)
