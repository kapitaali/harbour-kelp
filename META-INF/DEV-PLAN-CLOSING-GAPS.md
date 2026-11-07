# Development plan — closing the gaps (post-0.1.0)

Branch: `dev/closing-gaps` (created from master 08bb318, 2026-11-07).

## 0. Premise

0.1.0 is submitted (Harbour review pending). This plan covers everything the
release deliberately left out, plus the three items in the verified list that
turned out to be already done (worker-thread sealing, relay cert, gateway domain).
No code changes ship in 0.1.0; everything below is 0.2.0+ work.

## 1. Triage of the verified gap list

### 1.1 Genuinely not built (no code exists)

| # | Gap | Effort | Priority |
|---|-----|--------|----------|
| G1 | Peer swarm / C7 (HAVE/WANT/DATA/REJECT + HAVE_RANGES/HAVE/CANCEL/PEX) | XL | P3 |
| G2 | Stewards / channels / C6 (relay-side caching, channel pointers) | XL | P3 |
| G3 | Relay-side caching (relay holds objects, not just tunnels) | L | P2 |
| G4 | Full-bundle streaming (stream objects, don't hold all on disk) | L | P2 |
| G5 | Auto-start / background service (Harbour constraint) | M | P4 (may be impossible) |
| G6 | Per-bundle confirmation prompt (§12.10) | S | P2 |
| G7 | Receive-side preview of page in folder share (§5.4 / C4a.1) | M | P2 |
| G8 | Embedded Sailfish.WebView (§12.9) | M | P3 |
| G9 | Serve-again-what-you-downloaded (re-publish received bundle) | M | P2 |
| G10 | Ephemeral rooms / anonymous chat (Appendix B) | XL | P4 (parked) |

### 1.2 Partially built (code exists, gaps remain)

| # | Gap | What exists | What's missing | Effort | Priority |
|---|-----|-------------|----------------|--------|----------|
| G11 | C4a preview | kelppreviewserver, preview tests | per-bundle confirm, on-device browser pass, WebView, keep received bundle as row | M | P2 |
| G12 | C4b gateway | relay gateway, loader, SW | `#p=` passphrase case, app Copy browser link (removed 2026-10-21) | S | P2 |
| G13 | C2a LAN publish | LAN serving, address rebind | TLS on LAN, auto-start, old-link repair after network move | M | P3 |
| G14 | C2b LAN fetch | LAN fetch, retry on relay | re-serve downloaded bundles | M | P2 |
| G15 | C3 relay fallback | relay tunnel, fallback code | caching/stewards/swarm/TLS-on-LAN, design doc stale | L | P2 |
| G16 | C1.5 sealing | worker-thread sealing, pruneStore (manual) | auto-delete of unused objects | S | P2 |

### 1.3 Already done (stale items in prior list — no work needed)

| Item | Status |
|------|--------|
| Sealing on worker thread | Done (C1.5, 68029cf) |
| C4b gateway domain + browser-trusted cert | Done (kelp.kapitaali.com, Let's Encrypt) |
| Relay ALPN list (http/1.1 + kelp/1) | Done |

### 1.4 Branch/divergence items

| Item | Status |
|------|--------|
| `publish/no-rewrite` vs `master` divergence | Needs reconciliation (cherry-pick or merge) |
| run-emu-share.sh --relaunch address selection | Needs fix (autodetect doesn't pick 172.28.172.3) |
| Harbour upload | Owner action (external) |

## 2. Dependency graph

```
G16 (auto-delete) ──┐
G6 (per-bundle confirm) ──┤
G11 (C4a preview) ──┤
G7 (folder preview) ──┤──► G14 (re-serve downloaded) ──► G9 (serve-again UI)
G12 (gateway #p=) ──┤
G3 (relay caching) ──┤──► G2 (stewards/channels)
G4 (streaming) ──────┤
G15 (relay fallback) ─┘
G1 (peer swarm) ──► G3 (relay caching) ──► G2 (stewards)
G10 (ephemeral rooms) ── independent, parked
G5 (auto-start) ── independent, may be impossible on Harbour
G8 (WebView) ── independent, UI decision
G13 (TLS on LAN) ── independent
```

## 3. Phased delivery

### Phase A — Quick wins (S effort, no dependencies)

1. **G16**: Auto-delete unused objects (call pruneStore automatically after unshare/pause)
2. **G6**: Per-bundle confirmation prompt (§12.10)
3. **G12**: Gateway `#p=` passphrase case (PBKDF2-HMAC-SHA256 in WebCrypto)
4. **run-emu-share.sh**: Fix address autodetection (add 172.28.x.x candidate)

### Phase B — Medium features (M effort, some dependencies)

5. **G7**: Receive-side preview of page in folder share (§5.4)
6. **G11**: Complete C4a preview (on-device browser pass, keep received bundle as row)
7. **G14**: Re-serve downloaded bundles (foundation for G9)
8. **G9**: Serve-again-what-you-downloaded (UI)
9. **G13**: TLS on LAN path (optional, may break compatibility)

### Phase C — Large features (L/XL effort, heavy dependencies)

10. **G3**: Relay-side caching
11. **G4**: Full-bundle streaming
12. **G15**: Complete C3 relay fallback (design doc update, caching integration)
13. **G1**: Peer swarm / C7
14. **G2**: Stewards / channels / C6

### Phase D — Parked / may be impossible

15. **G5**: Auto-start / background service (Harbour constraint)
16. **G8**: Embedded Sailfish.WebView (UI decision)
17. **G10**: Ephemeral rooms / anonymous chat (Appendix B)

### Phase E — Branch reconciliation

18. **Reconcile `publish/no-rewrite` with `master`**: cherry-pick or merge the 7 divergent commits
    (relay fallback proofs, store listing updates, QML string changes) into dev/closing-gaps.

## 4. Workstream details

### WS-A1: Auto-delete unused objects (G16)

**Goal:** Call `pruneStore()` automatically when a share is unshared or paused, instead of
requiring manual invocation.

**Files:** `src/kelpbundles.cpp`, `src/kelpbundles.h`, `qml/harbour-kelp.qml`

**Approach:**
- After `unshare()` completes, call `pruneStore(shares)` internally
- After `pause()` completes, call `pruneStore(shares)` internally
- Add a setting: "Auto-clean unused objects" (default: on)
- Report freed bytes in the UI toast

**Tests:** Extend tst_kelpbundles.cpp with auto-prune test cases.

**Risk:** Low. pruneStore already exists and is tested.

### WS-A2: Per-bundle confirmation prompt (G6, §12.10)

**Goal:** Remember per-bundle consent so a previously-accepted bundle doesn't re-prompt.

**Files:** `src/kelpclient.cpp`, `src/kelpclient.h`, `qml/pages/OpenLinkPage.qml`

**Approach:**
- Store accepted bundle IDs in settings (key: `receive/accepted_bundles`)
- On receive, check if bundle ID is in accepted set; if so, skip prompt
- Add "always ask" toggle in settings

**Tests:** Extend tst_kelpclient.cpp.

**Risk:** Low. Design doc has the spec.

### WS-A3: Gateway `#p=` passphrase case (G12)

**Goal:** Support passphrase-protected links in the browser gateway.

**Files:** `relay/gateway/kelp-loader.js`, `relay/gateway/loader.html`

**Approach:**
- Implement PBKDF2-HMAC-SHA256 in WebCrypto (already supported)
- Read work factor from blob, derive key, unwrap secret
- Show passphrase input field in loader when `#p=` detected

**Tests:** Node tests against fixtures.

**Risk:** Medium. WebCrypto PBKDF2 is supported but needs careful implementation.

### WS-A4: run-emu-share.sh address fix

**Goal:** Auto-detect 172.28.x.x addresses (Docker bridge / VM network).

**Files:** `src/tests/run-emu-share.sh`

**Approach:**
- Add candidate scanning for 172.16-31.x.x addresses (private range)
- Prefer Wi-Fi, then default route, then 172.x.x.x, then others
- Print all candidates, let user override with --host

**Tests:** Manual verification.

**Risk:** Low.

### WS-B1: Receive-side preview of page in folder share (G7, §5.4)

**Goal:** When receiving a folder share that contains an HTML page, offer to preview it.

**Files:** `src/kelpclient.cpp`, `qml/pages/OpenLinkPage.qml`, `src/kelppreviewserver.cpp`

**Approach:**
- Detect index.html in received bundle manifest
- Offer "Preview" button alongside "Open in browser"
- Reuse kelppreviewserver to serve on 127.0.0.1

**Tests:** Extend tst_kelpclient.cpp and tst_kelppreview.cpp.

**Risk:** Medium. Need to handle folder-with-page detection.

### WS-B2: Complete C4a preview (G11)

**Goal:** Finish the remaining C4a preview items.

**Items:**
- On-device browser pass (verify with real browser on phone)
- Keep received bundle as a row (§12.13) — re-open, re-serve, re-download
- Per-bundle confirmation (covered by WS-A2)

**Files:** `src/kelpclient.cpp`, `qml/pages/OpenLinkPage.qml`, `qml/harbour-kelp.qml`

**Risk:** Medium. Keeping received bundles as rows changes data model.

### WS-B3: Re-serve downloaded bundles (G14, G9)

**Goal:** Allow re-publishing a received bundle.

**Approach:**
- Add "Share again" button on received bundle rows
- Re-seal the bundle with a new secret (or reuse original if still available)
- Requires G11's "keep as row" to be done first

**Dependencies:** G11 (WS-B2)

**Risk:** Medium-High. Security implications of re-sharing.

### WS-B4: TLS on LAN path (G13)

**Goal:** Serve LAN shares over HTTPS.

**Approach:**
- Generate self-signed cert per device (or use device cert)
- Add TLS to KelpServer
- Update link format (https:// instead of http://)
- Handle cert trust on recipient side

**Risk:** High. Recipient must trust sender's cert. May not be practical for LAN.

### WS-C1: Relay-side caching (G3)

**Goal:** Relay caches objects so links outlive the publisher's tunnel.

**Approach:**
- Add object storage to kelp-relay
- Cache on first fetch, serve from cache on subsequent
- Add cache eviction policy (LRU, size limit)
- Update relay protocol (cache headers)

**Dependencies:** None (relay-side only)

**Risk:** Medium. Storage management on relay.

### WS-C2: Full-bundle streaming (G4)

**Goal:** Stream objects instead of holding all on disk.

**Approach:**
- Modify kelpfetch to stream objects to final destination
- Remove intermediate disk buffering
- Handle partial failures (resume)

**Dependencies:** None

**Risk:** Medium. Error handling complexity.

### WS-C3: Peer swarm / C7 (G1)

**Goal:** Implement peer-to-peer object distribution.

**Approach:**
- Implement HAVE_RANGES/HAVE/CANCEL/PEX wire messages
- Add peer discovery (local network broadcast)
- Add rarest-first scheduling
- Gate behind "Allow peer swarm" setting

**Dependencies:** G3 (relay caching) for fallback

**Risk:** Very High. New protocol, complex scheduling.

### WS-C4: Stewards / channels / C6 (G2)

**Goal:** Implement steward nodes and channel pointers.

**Approach:**
- Define steward registration protocol
- Implement channel (signed pointer) objects
- Add IPFS export bridge

**Dependencies:** G3 (relay caching), G1 (peer swarm)

**Risk:** Very High. Depends on G1 and G3.

### WS-D: Parked items

| Item | Status | Notes |
|------|--------|-------|
| G5 (auto-start) | Parked | Harbour constraint: no background services |
| G8 (WebView) | Parked | UI decision, not code |
| G10 (ephemeral rooms) | Parked | Appendix B, after release |

## 5. Branch reconciliation (Phase E)

**Problem:** `publish/no-rewrite` has 7 commits that `master` doesn't:
1. `2c1dc73` Add relay fallback for off-network recipients
2. `7207518` Update store listing, privacy policy and submission report for relay
3. `426c60c` Update QML strings for relay support; refresh RPM hashes
4. `6f27020` Describe the relay without publishing its address
5. `aba6c0e` Prove the client retries a dead LAN address on the relay
6. `fdcb45e` Point the store at the branch it publishes from
7. `e9fdd8d` Record the rewritten history and the force-push it needs

**Approach:**
- Cherry-pick commits 2-6 (relay fallback, QML strings, store listing) into dev/closing-gaps
- Skip commits 1 and 7 (already in master under different commit hashes)
- Verify no conflicts with master's relay cert work

**Risk:** Medium. Cherry-pick conflicts possible.

## 6. Testing strategy

- Every workstream gets unit tests in `src/tests/`
- Integration tests via `run-emu-share.sh`
- QML smoke tests (9/9 pages Component.Ready)
- Harbour validator (18/18 sections) before any release build
- No release build without `sfdk check -s harbour` passing

## 7. Definition of done (per workstream)

- Code committed on dev/closing-gaps
- Unit tests pass
- QML smoke passes (if QML changed)
- Harbour validator passes (if RPM changed)
- Progress report updated
- No secrets/links/keys in committed docs

## 8. Open questions

1. Should G1 (peer swarm) and G2 (stewards) be deferred to 0.3.0?
2. Is TLS on LAN (G13) worth the complexity for a LAN-only app?
3. Should auto-delete (G16) be on by default?
4. Should the branch reconciliation (Phase E) happen first or last?
