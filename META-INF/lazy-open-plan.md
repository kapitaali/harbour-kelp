# Lazy-open a received webpage (design §5.1's last open item)

Implementation plan. `kelp-design.md` §5.1 and the K3 seam already say *what* this is:
a view-only page should paint after its structure is here, not after every byte of the
bundle. This file is only *how*, and it is written before code.

> **Status: implemented 2026-10-21; live browser pass completed 2026-10-22.** Code, tests and
> docs are in the tree; the i486 RPM was installed in the emulator and the QML smoke passed
> 9/9. The C++ suites pass in both dev-extras modes (`libkelp` 528, `tst_kelppreview` 281,
> `tst_kelpclient` 189, full suite 1422). The first live large-share test exposed and removed
> the old `/info` total-size gate (see below); the aarch64 release is rebuilt from the
> corrected tree. The live pass found one more page-loading bug: root-absolute asset and
> subpage URLs do not carry the preview token, so the page response now sets a host-only
> loopback cookie (L9). The large-share page and a root-absolute subpage both render in the
> emulator browser; see PROGRESS §3.45. K3 remains the server seam this builds on (`b42262c`,
> tests in `e8a5139`).

## The split

A bundle has two layers:

- **manifests** — the root, directory manifests and file manifests. They name every
  path, size, MIME type and chunk id, and they are what `Web::page`, `Web::resolve`
  and the preview server need before a browser can ask for anything. They are small
  and must all be present before the session opens, because a missing manifest is a
  `503`, not a fetchable hole (K3).
- **data** — the encrypted chunks. A browser asks for only the ones it needs, and K3
  can fetch each one on demand.

Lazy-open is therefore: fetch the manifests, open the session, let the browser drive
the data. There is **no background fill** of the rest of the bundle; that would be the
old eager fetch with extra steps, and it would compete with the browser for the same
source.

## Decisions

| # | Decision | Value | Why |
|---|---|---|---|
| L1 | What `fetchForViewing()` fetches | manifests only | It must resolve the page and answer paths; data is demand-driven. |
| L2 | When the view is `Done` | when the manifest walk is empty, before any content arrives | The QML already opens the browser on `done`; the session then owns the source and fetches what the browser asks for. |
| L3 | The 32 MiB ceiling | still absolute for the whole view store, but enforced against **what is actually stored**: the client caps the manifest index, the **server** caps on-demand content | The client no longer sees the content arrive. The sender's `/info` total is *not* a gate: a large bundle can have a small page, and the browser asks only for the assets it needs. |
| L4 | A view that exceeds the ceiling on demand | stop that session's fetches and answer every parked request `503` with a "too large" body; no data is written | The view store is the only memory that can grow. An honest page that needs less than 32 MiB never reaches this; a page that needs more must not grow memory without bound. |
| L5 | What drives the data fetch | the browser only | No speculative or background transfer; a page that asks for two assets costs two assets. |
| L6 | Source ownership | unchanged from K3: the session takes the source when it opens; the client keeps the decoded link for a fresh source after a sweep | Re-pressing *Open in browser* after the idle sweep must still work without another passphrase. |
| L7 | Owner-side previews | unchanged: no source, no limit, read from the app's store | The owner's objects are on disk and are not a received page. |
| L8 | `libkelp` | one new pure function, `missingManifests()` | The manifest-only walk is content-layer logic and belongs beside `missingObjects()`, not in QML or the client loop. |
| L9 | A page's root-absolute URLs (`/style.css`, `/sub/`) | every served file sets a host-only loopback cookie `kelp_preview=<token>` (`Path=/`, `HttpOnly`, `SameSite=Strict`); `handle()` accepts it when the first path segment is not a known token | Rewriting HTML, CSS and JavaScript URL construction is brittle. The cookie keeps arbitrary assets, CSS `url()`, JavaScript fetches and navigation working; it is the same read capability, scoped to loopback and the browser session, and never logged. A 64-hex first segment is still always treated as a token, so an unknown token is a 404 even with a valid cookie. |

**Live correction (2026-10-21).** The first implementation kept the eager flow's
`/info` total-size gate: a sender claiming more than 32 MiB was refused before any
object. A user test with a large share showed why that contradicts lazy-open: the
index page may need only a small part of a large bundle. The total claim is now
ignored for views. The manifest index is capped by the client as it arrives, and the
session caps the content it stores on demand; those are the only bytes the view keeps.
`KelpClient` still records the claimed total only for progress. A large bundle with a
small page opens; a page that actually needs more than 32 MiB is stopped at the line.

## Seams

### `libkelp/kelp_bundle.{h,cpp}`

```cpp
// The manifest-only sibling of missingObjects(): every directory and file
// manifest the walk cannot read yet, and none of the data chunks they name.
// A receiver can open the shape and let the browser pull content on demand.
bool missingManifests(ObjectStore *store, const QByteArray &key,
                      const QByteArray &rootId, QVector<QByteArray> *missing,
                      QString *error);
```

The existing `Missing` walker gains one flag. `visitFile()` still reads and decodes the
file manifest (so a corrupt one is an error, not a hole), but only notes chunk ids when
the flag is off. `missingObjects()` keeps its current behaviour byte for byte.

### `src/kelpclient.cpp`

- `walk()` chooses `missingManifests()` in view mode and `missingObjects()` otherwise.
  The same queue, deadline, inflight bound and `storeReceived()` verification are used.
- When the view's manifest walk is empty, `finishViewing()` sets `Done` as today. The
  log says the **index** is here and content is fetched by the browser, not that the
  whole store is here.
- `viewBytes()` reports the memory store's actual size (`MemoryStore::byteCount()`),
  so it includes objects the session fetched after the client's own work. The client's
  own `m_viewBytes` counter remains the manifest-side ceiling check.
- `openPageView()` passes the view ceiling to the server. If the page is absent, it
  releases the source too (there will be no session to own it).
- No QML API change: `fetchForViewing()` still ends at `done`, and the page still calls
  `openPageView()` from `onPhaseChanged`.

### `src/kelppreviewserver.{h,cpp}`

- `open()` and `openEntry()` gain an optional absolute `storeLimit` (0 means no limit).
  A session remembers it.
- `onObject()` checks the store's current byte count plus the arriving object before
  storing. If that would pass the limit, the session's fetches stop and all waiters get
  `503` with a short "too large" body; a new `Tally::overLimit` records it. The source
  is stopped, because no further object can fit.
- `Kelp::ObjectStore` gains `virtual qint64 byteCount() const`, returning `-1` when a
  store does not know. `MemoryStore` overrides it with the existing sum. A store that
  answers `-1` (the on-disk app store) is only used by unlimited owner previews, so no
  limit is silently invented for it.

### `qml/pages/OpenLinkPage.qml`

Only the busy wording changes: a webpage view is preparing its page, not downloading
the whole bundle, so the `%1 of %2 objects` line would be misleading. It says
"Preparing the page…" while fetching. The Done panel and the button are unchanged.

## Tests

- `libkelp`: an empty store names only the root manifest; after the root, the walk names
  manifests but no chunks; when the manifests are complete it is empty while
  `missingObjects()` still names chunks; the wrong key and a short id still fail.
- `tst_kelppreview`: a session with a tight `storeLimit` refuses an object that would
  pass it, answers the parked request `503`, stops the source and records `overLimit`;
  a page over a real socket sets the host-only cookie, serves root-absolute paths and a
  subdirectory page through it, still rejects them without it, and does not treat an
  unknown 64-hex token path as a file.
- `tst_kelpclient`:
  - `fetchForViewing()` requests fewer objects than the bundle holds and `viewBytes()`
    is the manifest size, not the bundle size;
  - opening the session and asking for `/` fetches the page object on demand; asking
    for a CSS/image asset fetches that object, and the bytes are exact;
  - a 64 MB total claim with a small page still reaches `Done` and opens;
  - the manifest index itself is bounded when the ceiling is tiny;
  - a content object that would cross the ceiling is refused by the session;
  - a page-less bundle still fails at `openPageView()` and releases everything;
  - two live sessions, a swept re-mint, and the link-identity checks still pass with
    only manifests fetched up front.
- Full suite both dev-extras modes, then the emulator smoke; the browser pass was done on
  the emulator (front page plus a root-absolute subpage, 2026-10-22). The user's C2 pass
  remains the device check.

## Deliberately out of scope

- Keeping a received bundle (§12.13), a per-bundle confirmation (§12.10), or saving a
  viewed page.
- Background prefetch, a disk cache, or changing the 32 MiB ceiling.
- Any relay work: this is the same LAN source the view already uses.
