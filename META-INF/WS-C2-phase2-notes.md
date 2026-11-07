# Phase 2: True streaming — implementation notes

## Current state after Phase 1

Phase 1 (manifest-first fetch) is complete. In stream mode:
- First round: fetch manifests only (missingManifests)
- After manifests arrive: switch to content pass (missingObjects)
- Content pass fetches all remaining objects (content chunks) into the store
- After all objects arrive: writeFiles() runs TreeWriter (reads from store)

So Phase 1 reorders the fetch but does NOT reduce peak disk usage. Content objects are still stored before files are written.

## Phase 2 goal

In stream mode, write files as content chunks arrive, WITHOUT storing them in the object store. Peak disk usage: ~file size + 256 KiB instead of 2× bundle size.

## Approach: per-file sequential chunk fetch

After manifests arrive:
1. Walk the manifest tree to collect a list of files, each with:
   - destination path
   - list of chunk IDs (in order)
   - expected size
2. For each file (in order):
   a. Create a temp file in the download directory
   b. For each chunk ID (in order):
      - Request the chunk from the source (one at a time, inflight=1)
      - Wait for onObject callback
      - Verify chunk against ID (existing storeReceived check)
      - Write chunk to temp file
      - Do NOT store in object store
   c. Close temp file
   d. Rename temp → final
   e. Update progress (files, bytes)
3. After all files written, delete manifests from store
4. Set phase to Done

## State machine

New members in KelpClient:
- `m_streamFiles`: QVector of StreamFile (path, chunks, size, bytesWritten)
- `m_streamFileIndex`: int (current file being written)
- `m_streamChunkIndex`: int (current chunk being fetched)
- `m_streamTempFile`: QFile* (current temp file)
- `m_streamWaiting`: bool (true when waiting for a chunk to arrive)
- `m_streamStreaming`: bool (true during stream write phase)

New methods:
- `startStreamWrite()`: collect files from manifests, start first file
- `streamWriteFile()`: open temp file, start first chunk
- `streamNextChunk()`: request next chunk from source
- `streamOnObject(id, bytes)`: verify and write chunk, request next
- `streamFinishFile()`: close temp, rename, update progress, next file
- `streamFinish()`: delete manifests, set Done

## File collection from manifests

Reuse TreeWriter's readDirectory logic but without writing. Need to walk the manifest tree and collect file entries with their chunk IDs.

The manifest tree structure:
- Root is a directory manifest
- Directory manifests contain entries (name, isDir, hash, size)
- File manifests contain chunks (list of object IDs) and size

To collect files:
1. Read root manifest (directory)
2. For each entry:
   - If directory: recurse (readDirectory)
   - If file: read file manifest, collect chunks

This is essentially what TreeWriter does, but collecting instead of writing.

## Chunk fetch coordination

In stream mode content phase:
- kMaxInflight is effectively 1 (sequential fetch)
- onObject() checks if we're streaming; if so, calls streamOnObject()
- streamOnObject() verifies, writes, and requests next chunk
- If no more chunks for current file, calls streamFinishFile()
- If no more files, calls streamFinish()

## Failure handling

- If a chunk fails verification: delete temp file, fail fetch
- If temp file write fails: delete temp file, fail fetch
- If rename fails: delete temp file, fail fetch
- Retry on relay: restart entire fetch (same as today)

## View mode interaction

Views are unaffected. They fetch only manifests and serve content on demand.

## Store mode

Unchanged. Store mode uses the existing flow (fetch all, then TreeWriter).

## Implementation order

1. Add stream state members and helper methods to KelpClient
2. Implement file collection from manifests
3. Implement per-file sequential chunk fetch and write
4. Modify walk() to call startStreamWrite() instead of writeFiles() in stream mode
5. Modify onObject() to route to streamOnObject() in stream mode
6. Clean up manifests after all files written
7. Test

## Estimated effort: L (3-5 days)

This is the most complex part of WS-C2. Phase 1 (manifest-first fetch) is already done and provides value on its own.
