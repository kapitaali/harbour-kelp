# WS-C4: Stewards and channels — Design

## Status

Scheduled for 0.2.0. Phase 2 (channel keys, signed pointers and links) is
implemented on `dev/0.2.0`; stewards and channel resolution/polling are still
design-only. IPFS bridge explicitly excluded (optional, large dependency).
Progress: `META-INF/PROGRESS-20261112.md`.

## 1. Problem

1. **Durability:** A bundle lives only while the origin phone serves it. If the phone goes offline, the bundle is unreachable (relay cache helps, but cache is eviction-prone).
2. **Mutability:** A `kelp://` link points to one immutable bundle. Updating the shared folder requires a new link. Recipients with old links see stale content.

## 2. Solution

### 2.1 Stewards

Any peer that has fetched a bundle can become a steward: hold its objects and re-serve them, registering as a source with the relay. Multiple stewards mean a link survives even if the original phone and relay are gone.

### 2.2 Channels

A mutable signed pointer object published to a well-known name (derived from a keypair). Recipients poll the pointer via any location backend. Gives "living folder with a stable address" without IPNS resolution machinery.

## 3. Stewards

### 3.1 Concept

A steward is a peer that:
- Has fetched a bundle (all objects verified and stored)
- Holds the objects (does not prune them)
- Re-serves them to other fetchers (same protocol as origin)
- Registers as a source with the relay

### 3.2 Registration protocol

1. Steward announces to relay: `{bundle_id, address, peer_id}` (authenticated with swarm token)
2. Relay adds steward to source list for that bundle
3. Relay hands steward's address to fetchers (alongside origin and other stewards)
4. Steward deregisters: stop announcing. Relay removes after timeout (e.g., 5 minutes of no announce)

### 3.3 Steward discovery

Fetchers discover stewards via the relay (same as peer discovery in WS-C3). The relay's source list for a bundle includes: origin (if online), relay cache, stewards.

### 3.4 Steward incentives

- **Opt-in:** User chooses to steward a bundle (setting per bundle: "Keep as steward")
- **Storage cost:** Steward holds objects (counts against device storage)
- **Bandwidth cost:** Steward serves objects (counts against upload quota)
- **No reward:** Altruistic. Could add reputation later.

### 3.5 Steward lifecycle

- **Become steward:** After successful fetch, user opts in. Objects are not pruned. Steward announces to relay.
- **Stop stewarding:** User opts out. Objects pruned (if not needed for own use). Steward deregisters.
- **Auto-steward:** Optional: automatically steward bundles that are frequently requested (relay tells steward its request count).

### 3.6 Privacy

- Steward exposes its IP to fetchers (same as origin). Steward mode should be opt-in for privacy-sensitive users.
- Steward can seed blindly (trades ciphertext, cannot decrypt). Relay never holds key.
- Relay learns "who fetches what" unless fetches go through steward.

## 4. Channels

### 4.1 Concept

A channel is a mutable signed pointer. The channel address is stable; the pointer behind it changes.

```
kelpchannel://<channel_id_b32>?r=<relay>#k=<secret>
```

- `channel_id` = SHA-256 of channel public key (32 bytes, base32-encoded)
- `secret` = 32 random bytes (fragment, never leaves device)
- `relay` = relay URL (optional, for pointer resolution)

### 4.2 Channel pointer object

A small signed CBOR map, stored as a Kelp object:

```
{
  "v": 1,
  "name": "my-shared-folder",     # human-readable label
  "bundle": <bundle_id>,          # current bundle (32 bytes)
  "seq": 5,                       # monotonic counter
  "ts": 1699999999,              # Unix timestamp
  "pub": <public_key>,            # Ed25519 public key (32 bytes)
  "sig": <signature>              # Ed25519 signature over canonical CBOR of above minus sig
}
```

Signed with the channel's private key. The public key travels in the pointer
because `channel_id = SHA-256(public_key)` is a one-way hash and cannot give it
back. A recipient checks `SHA-256(pub) == channel_id` before trusting the
pointer, then verifies `sig` over the canonical CBOR body. The read secret is
not the signing key: a recipient can read and verify a channel but cannot
publish an update.

On the wire the pointer is a Kelp object sealed under
`HKDF(read_secret, info="kelp/v1/channel")`. A relay stores ciphertext and a
content id; it learns neither the channel name nor the bundle it points at.

### 4.3 Channel lifecycle

1. **Create:** Generate Ed25519 keypair. channel_id = SHA-256(public_key). Store private key locally. Publish first pointer (seq=1, bundle=current).
2. **Update:** After sharing a new bundle, publish new pointer (seq++, bundle=new). Old pointer superseded.
3. **Resolve:** Recipient fetches pointer from relay/steward, verifies signature, extracts bundle_id, fetches bundle.
4. **Poll:** Recipient periodically checks for new pointer (seq > last_seen). If found, offer to fetch new bundle.
5. **Revoke:** Delete channel (stop publishing). Recipients with old pointer can still fetch old bundle (immutable), but won't get updates.

### 4.4 Channel resolution

- **Via relay:** `GET /channel/<channel_id>` → returns latest pointer object. Relay caches latest pointer.
- **Via steward:** Steward holds channel pointers for channels it stewards. Same protocol.
- **Direct:** If publisher is online and reachable, fetch pointer directly (LAN/IPv6).

### 4.5 Channel storage

- Channel pointers are stored in the object store (same as bundles). Identified by their object ID (SHA-256 of pointer bytes).
- Channel metadata (name, ids, sequence, timestamp) and the read secret live in
  `channels.json`; the Ed25519 private keys live in `channel-keys.json`. Both are
  owner-only (0600) in the app data directory, never in `settings.conf`, and
  neither secret is returned to QML.
- Channel pointer history: keep last N pointers (for rollback). Default N=5.

### 4.6 Channel QML

- **Create channel:** Settings → Channels → "New channel" → name → creates keypair, publishes first pointer.
- **Update channel:** Share page → "Update channel" → selects existing channel → publishes new pointer.
- **Channel list:** Settings → Channels → shows name, current bundle, seq, last updated.
- **Open channel:** Open-a-link page → paste `kelpchannel://...` → resolves pointer → fetches bundle.
- **Channel notifications:** When a polled channel has a new bundle, show notification "Channel 'X' has an update."

## 5. Implementation phases

### Phase 1: Steward registration (M, 2-3 days)

Extend relay to track stewards per bundle. Add steward registration/deregistration to KelpClient. Add "Keep as steward" toggle to BundlePage.

**Files:** kelp-relay, src/kelpclient.cpp, qml/pages/BundlePage.qml

### Phase 2: Channel keypair + pointer (M, 2-3 days)

Generate Ed25519 keypair. Create channel pointer object. Sign/verify. Store in object store. Channel metadata, read secret and signing keys in owner-only app files.

**Files:** src/kelpchannels.h/.cpp (new), libkelp/kelp_channel.h/.cpp

### Phase 3: Channel resolution + polling (M, 2-3 days)

Resolve channel via relay. Poll for updates. QML for channel management and open-channel flow.

**Files:** src/kelpclient.cpp, qml/pages/OpenLinkPage.qml, qml/pages/SettingsPage.qml

### Phase 4: Channel notifications (S, 1 day)

Notify user when a polled channel has a new bundle.

**Files:** qml/harbour-kelp.qml

## 6. Dependencies

- C1 (relay caching, done) — relay caches objects and pointers
- C3 (peer swarm, parked) — stewards work via relay only without C3; full P2P stewards need C3
- Ed25519 signing — OpenSSL 3 (already linked)

## 7. Risks

1. **Key management:** Channel private key must be stored securely. Harbour sandbox protects app data. Key loss = channel lost (no recovery). Mitigation: export keypair (QR or file).
2. **Channel spam:** Anyone can create channels. Mitigation: relay rate-limits channel creation per IP.
3. **Stale pointers:** If publisher goes offline, channel points to old bundle. Mitigation: timestamp + TTL. Recipients warned if pointer is old.
4. **Signature verification:** Must be correct. Mitigation: use OpenSSL's Ed25519 (well-tested).
5. **Storage for stewards:** Stewards hold objects indefinitely. Mitigation: user controls stewardship per bundle; storage page shows steward-held objects separately.

## 8. Not in scope

- IPFS bridge (optional, large dependency, parked separately)
- BitTorrent interop (WS-C3 optional, parked)
- Channel federation across relays (single relay per channel)
- Channel search/discovery (channels are shared via link, not browsable)
- Real-time updates (polling-based, not push)
