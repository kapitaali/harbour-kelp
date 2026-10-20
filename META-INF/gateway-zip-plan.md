# Downloading the folder as one ZIP — plan (design §12.12)

**Status: proposed, nothing built.** Written before any code, as this project requires.

## 1. What it is

A bundle with no page **is a folder**. The gateway's file list — the view a recipient gets for such
a bundle, and the fallback when a page cannot be served — gains one action:

> **Download folder (12 files, 24.6 MiB)**

**One press, one archive, whatever the number of files.** The archive is that folder: its contents,
with its folder structure, named after it. The per-file buttons stay as an alternative, for someone
who wants one file without the rest; they are not the flow.

**Not** for bundles that are served as a page: the sender's own document replaces the bootstrap page
there, so there is no file list and nothing to add a button to.

## 2. Why it is built in the browser

The relay carries ciphertext only and **never sees the key** — that is the whole design (§5.2). A
server-side archive would need the relay to decrypt, which would end that property. So the ZIP is
assembled in the recipient's browser, from the same verified-and-decrypted objects the per-file
buttons already use. **No relay change, no protocol change, no new endpoint, no new permission.**

The download mechanism is the one already proven on device for a single file (§3.36/§3.37): a
`Blob`, `URL.createObjectURL`, and an `<a download>` click. The CSP needs no change for it (that path
already works under `default-src 'self'`); the only difference is `application/zip` and a larger blob.

## 3. The format: STORE, not deflate

- **Method 0 (STORE)**, with a real **CRC-32** per entry. A ZIP is valid without compression, every
  extractor reads it, and it needs nothing the browser may not have: `CompressionStream` is not
  guaranteed (and is absent in older engines), while CRC-32 is ~20 lines of tested JavaScript.
- The cost is size: the archive is the sum of the files. For a bundle of photos, PDFs or a page's
  assets (already compressed or tiny) that is close to what deflate would give anyway. The design
  accepts that, and the button quotes the exact size up front because the manifest already knows it.
- No ZIP64. The caps below keep every archive strictly inside what classic ZIP can express
  (< 4 GiB, < 65 535 entries); `finish()` asserts this too, so a future change cannot silently emit a
  corrupt archive.
- Fixed timestamp (1980-01-01 00:00, the DOS epoch) rather than "now": the builder stays a pure
  function of its inputs, which is what makes it testable byte for byte.
- UTF-8 names with the language-encoding flag (0x0800) set; folder entries for the tree's folders,
  so empty folders survive.
- Each file's byte count is checked against its manifest before its header is written; a manifest
  that lies aborts the archive instead of writing a header with the wrong size.

## 4. Limits, progress, failure

- **Caps: 128 MiB of file bytes and 65 535 entries.** Both are computed from the manifests *before*
  any object is fetched, so the page can disable the button and say why instead of dying halfway.
  Per-file download remains available above the cap.
- The cap is a memory decision: the browser holds the fetched bytes until the archive is handed to
  the downloader, so peak use is roughly twice the archive (data parts, then the `Blob`). 128 MiB is
  a single constant (`ZIP_MAX_BYTES`) and can be raised or lowered without touching the format.
- **One file at a time.** A per-file download fetches its chunks in parallel; the archive loop walks
  files sequentially (chunks in parallel inside a file), which bounds the working set and is gentle
  on a relay that answers 503 for an object already in flight (the existing retry still applies).
- **Progress** in the status line: *Fetching 4 of 12 (8.1 MiB of 24.6 MiB)*. The button becomes
  **Cancel** while running.
- **Failure = no file.** The blob is created only after every object has been fetched, verified and
  appended. A dead sender, a substituted object, a size mismatch or a cancellation produces a
  sentence and *no* partial archive. Cancel is checked between files.
- While the archive runs, the per-file buttons are disabled (and the archive button is disabled
  while a per-file download runs), so two large fetches cannot compete.
- The caps are safety limits, not a per-file flow: below them the number of files does not change
  the interaction at all — one press, one archive.

## 5. Names — the part that has to be right

Entry paths come from the sender. In a ZIP they become paths on the recipient's disk, so a name that
can escape the extraction folder is the classic zip-slip bug. The manifest already forbids `/`, `.`,
`..`, and NUL in a *component* (`Manifest::isValidName`), but a ZIP also crosses Windows and macOS
extractors, which add rules of their own. So every entry name is built component by component:

- NFC-normalise; replace `\` and every control character (0x00–0x1F, 0x7F) with `_`;
- `.` and `..` → `_`; strip trailing dots and spaces (Windows silently does, which can merge names);
- defuse a Windows drive prefix (`C:` → `_C:`); empty → `_`;
- truncate a component at 255 UTF-8 bytes on a code-point boundary;
- **dedupe**: entries are compared NFC-and-lower-cased, because Windows and macOS extract
  case-insensitively; a collision gets ` (2)`, ` (3)` before the file's extension — the same shape as
  `uniquePath()` in the app. Directories and files share one namespace.
- The archive's own file name is sanitised by the same rule (it is written into `a.download`).

**Archive name: the folder's name, from the link itself.** The link's `q=` block is not opaque to the
browser: it is sealed under `HKDF(secret, "kelp/v1/link")` with the 32-byte bundle id as associated
data and carries `{l: address, n: label, p: is-a-page}`. The label `n` is the share's name — for a
folder share, the folder's name. The browser already holds the secret and the bundle id, so it can
open `q=` with a small `openLocations()` beside the existing HKDF and the existing CBOR reader:
no relay change, no extra metadata exposed (the relay is not asked for anything; the link already
carried this), and the archive is `<folder>.zip`, which is what zipping that folder would give.

If the link carries no `q=`, no label, or the block does not open (an older link, a passphrase link
whose hints were omitted, a key that does not match), the archive falls back to
`kelp-<first 8 of the bundle id>.zip`. A name is never a reason to refuse the download. The label is
sender-controlled text like any path, so it goes through the same sanitiser.

## 6. Where the code goes

`core.js` stays the pure, DOM-free half and holds everything worth testing:

- `crc32(bytes)` / `crc32Update(state, bytes)` — table-based, unsigned;
- `zipSafeName(path)` and `zipNames(files, dirs)` — sanitisation and dedupe, pure and vector-testable;
- `zipWriter()` — `add(name, parts)`, `addDirectory(name)`, `finish()` → `{parts, bytes, entries}`;
  it writes local headers, the central directory and the EOCD, and stores references (no copy);
- `zipBundle(fetcher, files, dirs, options)` — caps, names, the sequential fetch loop, the size check,
  `onProgress`, a `cancelled()` check; returns the same `{parts, bytes, entries}`;
- `openLocations(secret, bundleId, blob)` — opens the link's `q=` block (the `kelp/v1/link` HKDF, the
  GCM, the existing CBOR reader) and returns `{name, page, lan}`; the archive name comes from it;
- `ZIP_MAX_BYTES`, `ZIP_MAX_ENTRIES`.

`walk()` gains a `dirs` array (relative paths, root excluded). The existing `directories` count stays,
so nothing that reads it today changes.

`page.js` keeps only the edge: the button, the status text, Cancel, `Blob` + `a.download`, and
disabling the other buttons. No new file, no build step (the page already loads `core.js` by a script
tag, and the worker is untouched).

## 7. What the recipient sees

| State | Text |
|---|---|
| file list, idle | *This folder has 12 files* + **Download folder (12 files, 24.6 MiB)** |
| too big | button disabled, muted line: *This bundle is too large for one archive (over 128 MiB). Download files one by one.* |
| too many | *This bundle has too many files for one archive. Download files one by one.* |
| running | status *Fetching 3 of 12 (4.2 MiB of 24.6 MiB)*, button reads **Cancel** |
| done | *Saved kelp-ab12cd34.zip (24.6 MiB)* |
| cancelled | *Cancelled — nothing was saved.* |
| failed | *Could not build the archive: … Nothing was saved.* |

Wording follows the gateway's existing voice: plain, no jargon, and it says *saved* only after the
browser has been handed the file.

## 8. Verification

**Node, against real seals** (extend `core.test.cjs`, which already has 71 checks and a fetch shim
that can corrupt an object):

- CRC-32 vectors: `""` → 0, `"123456789"` → `0xCBF43926`, the pangram → `0x414FA339`;
- build the `page` and `folder` fixture bundles through `zipBundle`, write the archive into
  `test/.build/`, and check it with **real tools**: `unzip -t` (or `python3 -m zipfile -t`) reports
  no errors; `zipinfo -1` lists the expected names in the expected order; extracting it and hashing
  every file matches the fixture's own recorded `sha256` for each file;
- folder entries present; a safe-name table (absolute-looking, `..`, backslash, control characters,
  trailing dot, drive prefix, NFC, case collision, duplicate) produces exactly the expected names;
- the archive is named after the link's label: the fixture gains the **real link** that libkelp mints
  (one `Kelp::Link::encode` call), and `openLocations()` must return the same name and page flag
  libkelp sealed into `q=`; a link without `q=`, and a `q=` opened with the wrong bundle id, both fall
  back to `kelp-<8>` instead of failing the download;
- a file whose fetched bytes disagree with its manifest size aborts;
- the corrupt-object shim aborts the build and returns no archive;
- a bundle over either cap is refused **before the fetcher is called**;
- `cancelled()` stops between files, with no archive.

Existing checks and the relay harness must stay green (`71` checks today; the relay's 11
browser-shaped cases do not execute page JS, so they are unchanged).

**Live, in a real browser** (the emulator's `sailfish-browser`, as the earlier gateway runs used):
open a bundle's gateway link, press *Download folder*, then pull the downloaded file back to the host
and run `unzip -t` plus a byte-for-byte comparison against the sealed source. Also: Cancel halfway
(no file left behind), and a bundle above the cap (button disabled, sentence shown, per-file still
works). The click is the one thing Node cannot prove; the same blob-download path already works for
single files on device, so this is a confirmation, not a first outing.

## 9. Slices

- **S1 — the builder.** `crc32`, names, `zipWriter`, `zipBundle`, `walk().dirs`, exports; the Node
  tests and the real-tool checks. No visible change yet.
- **S2 — the button.** The file-list action, progress, Cancel, the caps' wording, disabling the
  other buttons; the live browser run; `PROGRESS` §3.x evidence.

Each slice keeps the suites green and the tree building; the design's §12.12 gets a *resolved* note
when S2 lands.

## 10. Traps

- **CRC over signed bytes**: JS `charCodeAt`/Uint8Array values are fine, but the running register must
  be kept unsigned (`>>> 0`) or every CRC above 2³¹ is wrong.
- **Blob lifetime**: revoke the object URL after the download has started, not before (the existing
  60-second timer is the pattern); too early and a large archive is truncated.
- **User gesture**: creating and clicking the link after async work is what the per-file button
  already does; it must be re-checked live for the archive path, since some browsers are stricter
  about programmatic downloads.
- **Memory**: the parts are held until `finish()`. Do not raise the cap without re-reading §4.
- **Names are a security boundary**, not cosmetics: dedupe case-insensitively, defuse `..`, `\`, and
  drive prefixes, and never let a sanitised name start with `/`.
- **File sizes come from two places** (the manifest and the fetched bytes); they must agree, or the
  archive's headers lie.
- **Empty files** (size 0) are legal entries: CRC 0, no data.
- **A folder and a file that sanitise to the same name** must not both be written; dedupe across
  the whole archive, not per folder.
- **The label is optional and sender-controlled**: a `q=` may be absent (an older link, a passphrase
  link that omitted its hints), may fail to open, and may be a strange string. All three end at the
  fallback name, never at a failed download — and the label is sanitised like a path.

## 11. What it does not do

- No compression, no ZIP64, no resume, no password on the archive (the bundle's encryption protects
  the transfer; the saved copy is plaintext, exactly like the app's own saves).
- No server-side archive, no new endpoint, no key or plaintext to the relay.
- No archive button on a bundle that is served as a page.
- No change to `/info`: the name comes from the link the browser already holds, not from the relay.

## 12. The two choices worth your nod

1. **Uncompressed (STORE) archives** — always readable, no dependency on `CompressionStream`, larger
   files. Recommended.
2. **A 128 MiB / 65 535-entry ceiling** with an honest sentence and per-file download above it, rather
   than trying and risking the browser. Recommended; the number is one constant if you want another.
