# WS-C2: Full-bundle streaming — Design

## 1. Problem

The receiving flow today:

1. Fetch all objects (manifests + content) from source → store each in the on-disk object store.
2. After all objects arrive → `writeFiles()` → `TreeWriter` reads manifests from the store, streams files to disk chunk-by-chunk.
3. Objects remain in the store (pruned later if unused).

For a 335 MiB bundle on a device with 2 GiB free, step 1 writes 335 MiB of objects to the store, then step 2 writes 335 MiB of files. Peak disk usage: ~670 MiB plus store overhead. On a low-memory device this can fail with `std::bad_alloc` or out-of-space.

The TreeWriter already streams *files* (one chunk in memory at a time). The remaining gap is the object store holding *everything* before any file is written.

## 2. Solution: two modes with a setting

A setting `receiveMode` in KelpSettings with two values:

- **`store`** (default, current behaviour): fetch all objects into the store, then write files. Simple, proven, but high disk usage.
- **`stream`**: manifest-first fetch, incremental file writing, objects deleted as they are consumed. Lower disk usage, more complex.

The setting is read at fetch start. Existing bundles already in the store are unaffected — only new fetches use the selected mode.

## 3. Streaming mode: manifest-first fetch

### Phase 1 — Manifests

1. Fetch the root manifest (object ID = bundle ID).
2. Decode it. If it's a file manifest → Phase 2 with one file. If it's a directory → recursively fetch child manifests.
3. Store all manifests in the object store (they're small — ≤256 KiB each, typically <10 KiB).
4. After all manifests are fetched and decoded, the full file tree is known. Total manifest size is typically <1% of bundle size.

### Phase 2 — Content streaming

1. Walk the file tree (depth-first, same order as TreeWriter).
2. For each file:
   a. Create a temp file in the download directory.
   b. For each chunk ID in the file's manifest:
      - Request the chunk from the source.
      - Verify the chunk against its object ID (existing `storeReceived` check).
      - Write the chunk to the temp file.
      - Delete the chunk from the object store immediately.
   c. Close the temp file.
   d. Rename temp → final (atomic on same filesystem).
   e. Update progress (files, bytes).
3. If any chunk fails verification, delete the temp file and fail the fetch.

### Phase 3 — Cleanup

1. Delete all manifest objects from the object store.
2. The files are on disk; objects are no longer needed (snap model).

### Disk budget during streaming receive

- Manifests: ~tens of KiB (held until Phase 3).
- One temp file being written: up to file size.
- One chunk in memory: 256 KiB.
- Peak: ~file size + 256 KiB + manifests. No 2× bundle size.

## 4. Key decisions

| # | Question | Decision | Rationale |
|---|---|---|---|
| 1 | Mode selection | Setting `receiveMode`: `store` or `stream`. Default `store`. | User choice; store mode is proven. |
| 2 | Manifest-first? | Yes. Fetch root → walk manifests → then content. | Required to know file structure before streaming content. |
| 3 | Verification | Per-chunk (existing `storeReceived` check). No per-file hash. | Chunk verification against object ID is already cryptographic. |
| 4 | Failure recovery | Temp file + rename on success. On failure, delete temp file and all partial objects. | Consistent with existing TreeWriter pattern. |
| 5 | Storage target | No hard constraint — design scales to any bundle that fits as files. | Natural property of streaming. |
| 6 | Relay vs LAN | Both. Same fetch order regardless of source. | Source is abstracted by `KelpSource`. |
| 7 | View mode | No change. Views fetch only manifests. | Already optimal. |

## 5. What changes

### 5.1 KelpSettings (src/kelpsettings.h/.cpp)

Add:
```cpp
Q_PROPERTY(QString receiveMode READ receiveMode WRITE setReceiveMode NOTIFY changed)
```
Values: `"store"` (default) or `"stream"`. Persisted in QSettings.

### 5.2 KelpClient (src/kelpclient.cpp/h)

**New member:**
```cpp
QSet<QByteArray> m_manifestIds;   // objects that are manifests (kept until Phase 3)
```

**Fetch flow changes:**

Current: `beginFetch()` → `startFetch()` → `walk()` fetches all objects in source order.

Streaming: `beginFetch()` → `startFetch()` → if stream mode:
  1. Fetch root manifest.
  2. On arrival, decode. If directory, fetch child manifests (recursive).
  3. After all manifests fetched, walk file tree and fetch content chunks.
  4. Write each file incrementally as chunks arrive.
  5. Clean up manifests.

**New methods:**
- `fetchManifests()` — recursive manifest fetch.
- `onManifest(const QByteArray &id, const QByteArray &bytes)` — decode and queue children.
- `streamFile(const Kelp::Manifest::Entry &entry, const QString &path)` — write one file chunk-by-chunk.
- `finishStreaming()` — Phase 3 cleanup.

**Existing methods affected:**
- `onObject()` — in stream mode, route to manifest handler or content handler based on whether the ID is in `m_manifestIds`.
- `writeFiles()` — in stream mode, this is replaced by incremental writing. In store mode, unchanged.
- `reset()` — clear `m_manifestIds`.

### 5.3 TreeWriter (src/kelpclient.cpp)

In store mode: unchanged (reads from store).

In stream mode: replaced by incremental writing within `KelpClient`. The TreeWriter class can remain for store mode.

Alternatively, refactor TreeWriter to accept a "chunk provider" callback instead of reading directly from the store. This would unify both modes. Recommended: keep separate for simplicity; streaming writes inline.

### 5.4 KelpSource (src/kelpsource.cpp/h)

No change. The source already abstracts LAN vs relay. Fetch order is KelpClient's decision.

### 5.5 QML (qml/pages/OpenLinkPage.qml)

Add a receive mode indicator/selector:
- Show current mode in the link page (e.g., "Streaming" or "Store").
- Allow switching mode before starting a download.
- After download, show which mode was used in the result summary.

## 6. Implementation phases

### Phase 1: Manifest-first fetch (M, 2-3 days)

Change `KelpClient` to fetch manifests before content. In store mode, this is just reordering — objects still go to the store. In stream mode, this is the prerequisite for Phase 2.

**Files:** src/kelpclient.cpp, src/kelpclient.h

**Tests:** Existing kelpclient tests must still pass. Add tests for manifest-first ordering.

### Phase 2: Incremental file writing (L, 3-5 days)

Replace `writeFiles()` in stream mode with incremental chunk-by-chunk writing. Each file is written to a temp file, verified chunk-by-chunk, and renamed on success.

**Files:** src/kelpclient.cpp

**Tests:** Add tests for streaming mode: single file, multi-file folder, partial failure (chunk verification fails mid-file), resume after failure.

### Phase 3: Object store cleanup (S, 1 day)

Delete content objects after each file is written. Delete manifest objects after all files are written.

**Files:** src/kelpclient.cpp

**Tests:** Verify store is empty after streaming receive (snap model).

### Phase 4: Settings + QML (S, 1 day)

Add `receiveMode` setting. Add mode selector in OpenLinkPage.

**Files:** src/kelpsettings.cpp/h, qml/pages/OpenLinkPage.qml, qml/pages/SettingsPage.qml

## 7. Risks

1. **Manifest decoding during fetch**: Moving manifest decoding into the fetch phase means handling partial failures (a manifest arrives but its parent hasn't been decoded). Mitigation: fetch manifests depth-first; each manifest's children are only fetched after the parent is decoded.

2. **Chunk ordering**: Content chunks must be written in order. The source may deliver them out of order. Mitigation: fetch chunks sequentially (one at a time) rather than in parallel. This is slower but simpler and sufficient for LAN. For relay, parallel fetch with reordering buffer (max one file's chunks = 256 KiB × chunks per file).

3. **Retry on failure**: If a chunk fails verification, the temp file must be truncated. Mitigation: track bytes written per file; on failure, truncate to last verified chunk boundary.

4. **View mode interaction**: Views fetch only manifests. If we change the fetch order, views must still work. Mitigation: views already only fetch manifests; no change needed. In stream mode, views behave identically to store mode.

5. **Relay caching (WS-C1)**: The relay caches objects. In stream mode, objects are deleted from the local store after writing, but the relay still has them. This is fine — the relay is a separate cache.

## 8. Open questions (resolved)

1. Mode selection: setting `receiveMode` with `store`/`stream`. ✓
2. Manifest-first: yes. ✓
3. Per-chunk verification: yes. ✓
4. Temp file + rename: yes. ✓
5. No hard storage constraint. ✓
6. Both relay and LAN. ✓
7. No change to view mode. ✓

## 9. Not in scope

- Per-file integrity hash (chunk verification is sufficient).
- Resume after partial failure (re-fetch entire bundle on failure, same as today).
- Parallel chunk fetch (sequential is simpler; can add later if needed).
- Changes to `Kelp::open()` in libkelp (streaming bypasses it in stream mode).
