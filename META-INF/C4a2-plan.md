# C4a.2 — view-only webpage links (design §5.5, the recipient half)

Implementation plan. The behaviour is already decided in `kelp-design.md` §5.5 and listed
as milestone C4a.2 in §11; this file is only *how*, and it is written before code on
purpose. Nothing here changes the security model: the loopback origin with its token, the
CSP, and "no key in any log" all stay exactly as §5.1 defines them.

> **Status: implemented and verified** (2026-10-18). The code, the two suites and the
> emulator run are written up in `PROGRESS-20261016.md` §3.22; this file remains as the
> plan it was built from, and the decisions below are what the code does.

## Why this exists right now

A webpage share already *says* it is a page: C5.1 puts `"p": true` in the link's sealed
`q=` block and `linkInfo` reports it. Nothing acts on it. So opening a webpage link on a
recipient's device runs the ordinary save path — it offers **Save to**, then downloads the
bundle into Downloads. That is the wrong behaviour by §5.5, and it is what a C2 build does
today: not a regression, just the half that is not built. The recipient half is this plan.

## The behaviour, restated as a checklist

For a link whose `p` hint is set:

1. The Open-link page is a **view, not a save**: no *Save to* section, no *Download*
   button, no folder choice, and nothing written to Downloads.
2. One action: **Open in browser**. It fetches the bundle, serves it from `127.0.0.1` on
   the same single `KelpPreviewServer` a share preview uses, and hands the URL to the
   browser.
3. The fetch lands in a **session-owned `Kelp::MemoryStore`**, never the app's object
   store. When the session ends the store is dropped — there is no cleanup step and
   nothing of the page is left on the device.
4. A **ceiling** refuses an over-large page *before* a byte of content is fetched, naming
   the sender-side workaround (share it as a folder). It is enforced again while storing,
   because the claimed size is the sender's word.
5. `KeepAlive` is held while the session is live (the browser owns the foreground, not
   Kelp).
6. A link with no `p` — every link already in the wild — behaves exactly as today: save,
   and §5.4's *Open in browser* afterwards if the fetched bundle happens to hold a page.

## Decisions taken here (each is a knob, not a foregone conclusion)

| # | Decision | Value | Why |
|---|---|---|---|
| D1 | The page ceiling | **32 MiB** (`kViewCeilingBytes`) | §5.5 says "tens of megabytes, enough for a document-shaped page and not for a gallery". One constant, logged when it refuses. Chosen by the user. |
| D2 | Who owns the session store | `KelpClient` creates it, per view; the **session shares it** (`QSharedPointer`) | It is the object that fetches, the one the QML page already talks to. Shared, not borrowed, because of D4: see below. |
| D3 | Who owns the listener | unchanged: `KelpPreviewServer`, one instance, owned by `harbour-kelp.cpp` | §5.5: "one listener, the same server". |
| D4 | Session lifetime | **the server's idle sweep only** (30 min), as the owner-side preview already works; app exit drops everything | Chosen by the user, over closing the session when the user leaves the page: less machinery, at the cost of the decrypted copy living up to half an hour in RAM. §5.5's "the user leaves the screen" is therefore *not* implemented as an explicit close; it is worth saying so in PROGRESS. |
| D4a | Consequence of D4, which is not optional | the store is a `QSharedPointer<Kelp::ObjectStore>`, held by the client while a fetch is in flight and by the session once it opens | With a borrowed raw pointer, opening a second page link while the first session is still live would free the first session's store under it — a use-after-free, reachable by pressing *Open another link* and opening another page (the first browser tab can still ask for the old URL). A shared pointer makes "no explicit close" safe: whoever is last frees it, and nobody dangles. |
| D5 | Re-pressing *Open in browser* in the Done panel | re-opens the **same** session (same URL) rather than fetching or minting a new one — **unless that session has gone** (the idle sweep of D4), when a fresh one is minted from the memory still held | It is one session per view; re-fetching would be surprising, and a new token would orphan the first. The caveat is D8: without it, the URL handed to the browser was one whose session the sweep had already dropped. |
| D6 | A `p` link whose bundle has no page | the fetch completes, then the view fails with §5.5's wording; no save is offered and the store is released | §5.5's failure table; the hint is a default, not enforcement, so a dishonest sender must not turn into a save. |
| D7 | New unit suite for the client | **yes**, `tst_kelpclient` (see Tests) | Ceiling, "no files written" and "app store untouched" are logic, and the device pass cannot repeat them cheaply. Chosen by the user. |
| D8 | Who a finished fetch belongs to (**added after the C2 report**, §3.25) | the **link** it was started from: `KelpClient` keeps `m_link` and answers `isHandling(link)`, and the QML page presents a finished or failed fetch only for *its own* link | D4/D4a let a session outlive the page that opened it, and the client outlives it too — so a page opened later found the last visit's `done` and treated its own link as already open (pressing *Open in browser* took the "already open" path and re-handed the swept session's URL). The link is the identity of everything the client holds for one fetch. This is the same rule D5's caveat needs. |

## The seams, exactly

### `src/kelppreviewserver.{h,cpp} — a session becomes a store plus a key`

The bullet in §11: "whose `open()` takes the store, so a session becomes a store plus a
key, and one listener still serves shares and received pages alike."

```cpp
// ctor: the store goes away — a session names its own.
explicit KelpPreviewServer(QObject *parent = nullptr);

// Borrowed by the app for its own store, owned by the client for a view store (D4a).
using StorePtr = QSharedPointer<Kelp::ObjectStore>;

Opened open(const StorePtr &store, const QString &bundleId, const QString &secret,
            const QString &name);
Opened openEntry(const StorePtr &store, const QString &bundleId, const QString &secret,
                 const QString &name, const QString &entry);
```

- `Session` gains `StorePtr store`; `resolve()`/`serve()`/the 503 "not held yet" path read
  `session->store` instead of `m_store`. The reference makes the store outlive the session
  by construction — the sweeper can drop a session whenever it likes and the last
  reference, not the clock, decides when the memory goes.
- `KelpBundles::previewPage()` passes the app's store as a borrowed shared pointer
  (`StorePtr(m_store, [](Kelp::ObjectStore *) {})` — the app owns it; the no-op deleter
  says so). Behaviour of the owner-side preview is otherwise byte-for-byte unchanged.
- `sessionsChanged()` keeps its meaning, so the app's `previewSessions` count now covers
  owner previews and received pages alike with no change in `harbour-kelp.qml`.

### `src/kelpclient.{h,cpp} — a second store, and a mode`

```cpp
// New, alongside fetch():
Q_INVOKABLE bool fetchForViewing(const QString &link);   // same walk, memory store, no files
Q_INVOKABLE QVariantMap openPageView();                  // page -> preview session; {ok,url,entry,candidates,error}

Q_PROPERTY(bool viewing READ viewing NOTIFY changed)

void setPreviewServer(KelpPreviewServer *preview);       // wired in harbour-kelp.cpp
```

Inside:

- `KelpPreviewServer::StorePtr m_viewStore;`, re-created per view (D4a), plus `bool
  m_viewing`, the `m_viewToken`/`m_viewUrl` of the session this view opened, `m_viewSecret`
  (the base64url string, so `openPageView()` needs nothing from QML) and `m_viewBytes`.
- `Kelp::ObjectStore *activeStore()` = `m_viewing ? m_viewStore.data() : m_store`. **Every**
  use of the store in the walk goes through it: `missingObjects()`, `storeReceived()` in
  `pump()`, and the `Web::page()` call. The app store is therefore never written in view
  mode — which is the point, and is verified.
- `fetchForViewing()` replaces `m_viewStore` with a fresh `MemoryStore` (an earlier
  session, if any, keeps its own reference and is untouched — D4a), sets `m_viewing =
  true`, and runs the same path as `fetch()`. A second entry point rather than a mode flag
  on `fetch()` so the QML reads "this is a view" from the call it made.
- `onInfo(objects, files, bytes)`: in view mode, **before** `walk()`, refuse when the
  claimed `bytes > kViewCeilingBytes` — nothing has been stored and no object has been
  requested. Message (§5.5's wording, with the workaround):
  `"This page is 48.2 MB — more than the 32 MB a shared page can be viewed with. Ask the
  sender to share it as a folder instead."`
- `pump()`: in view mode add each stored object's size to `m_viewBytes` and fail with the
  same wording once it passes the ceiling (an `/info` that lied is caught here).
- At "all objects are here", view mode goes straight to `Done` (files/bytes stay as they
  are; `destination` stays empty); it does **not** call `writeFiles()`.
- `openPageView()`: requires phase `Done` and `m_viewing`; if `m_viewToken` is already
  set, return the same URL (D5). Otherwise: `m_preview->start()`, `Kelp::Web::page(
  m_viewStore.data(), m_key, m_bundleId, &page, &error)`, and
  - no page → `{ ok: false, error: "This link says it is a page, but there is no web page
    in it." }`, and the store is released (D6) — `m_viewStore.clear()` is safe now, because
    no session holds a reference to it;
  - page → `m_preview->open(m_viewStore, m_bundleIdText, m_viewSecret, m_name)`, remember
    token/URL, and log the URL **only** under the same gate `previewPage` uses
    (`linksInLog`), never the key. The session now holds the store; the client's own
    reference is simply the current view's.
  QML calls `Qt.openUrlExternally(url)` and logs the boolean, the way `BundlePage` does
  (`true` and nothing opening is the emulator's documented case, §12.9).
- There is **no explicit close** (D4): a session and its memory end with the sweep or the
  app. `reset()` clears the client's reference to the current view store, which is safe by
  construction and releases the memory as soon as no session is still using it.
- Logging, all without the secret: `viewing <id> from <source> (a page; nothing will be
  written)`, the ceiling refusal, `view store: N objects, M bytes in memory`, and the
  session URL under the link gate.

### `src/harbour-kelp.cpp`

```cpp
KelpPreviewServer previewServer;              // was previewServer(&objectStore)
KelpClient kelpClient(&objectStore);          // unchanged
kelpClient.setPreviewServer(&previewServer);  // new line
```

### `qml/pages/OpenLinkPage.qml` — the branch

- `parsed` gains `"page": info.page === true` (`linkInfo` already reports it).
- `property bool viewOnly: parsed !== null && parsed.ok && parsed.page`.
- When `viewOnly`:
  - the *Save to* section and the *Download* button are hidden, as is the "what arrived
    before that is in …" line;
  - the action button reads **Open in browser**, is enabled when the link names a source
    (the existing `canDownload` condition, renamed `canFetch`), and runs
    `fetchForViewing(link)`, then on `phase === "done"` calls `openPageView()` and
    `Qt.openUrlExternally(url)`;
  - the busy row reuses the existing strings ("Asking the sender…", "Fetched %1 of %2
    objects") — no "Saving the files…", which cannot happen here;
  - the Done panel says **Opened in browser**, "Nothing was saved — a page shared with you
    is only looked at", adds "This bundle holds N pages; the one shown is
    <entry>" when there was a choice, keeps *Open in browser* (D5) and *Open another
    link*, which resets the page as it does today (no explicit close — D4).
- When not `viewOnly`: nothing changes, save path untouched.

### `qml/harbour-kelp.qml` — the keep-alive rule §5.4 asked for

`enabled:` becomes `kelp.previewSessions > 0 || (kelp.seedingEnabled && kelp.servedShares
> 0)`. Today the recipient case would be dropped: a recipient never turned *Share in
background* on, and has nothing in the list being served, while the foreground belongs to
the browser. The log line keeps both halves spelled out.

## Failure modes, each mapped to the code that answers it

| §5.5 situation | What happens |
|---|---|
| Folder has no HTML (publisher) | already refused in C5.1; unchanged |
| A `p` link whose bundle has no page | fetch completes, then `openPageView()` returns §5.5's wording; store dropped; no save |
| A link with no `p` | ordinary save, and nothing else: no browser button. C4a.1, which would have added one for a page inside the folder, was dropped by the user (2026-10-18, design §5.4) |
| `p` but no address and no relay | **Open in browser** is disabled; the page shows the same reason the save path already gives |
| Sender goes offline mid-page | the browser's requests fail like any preview's; nothing saved to fall back on, by design |
| Page larger than the ceiling | refused from `/info` before fetching; re-checked while storing |
| Recipient leaves the screen | **nothing happens, by choice (D4)**: the session and its memory live until the sweep, exactly as the owner-side preview does; app exit drops them |
| Recipient's device has no browser | `Qt.openUrlExternally` returns `true` and nothing opens; the session exists, so a `curl` from inside the guest is the proof (this is the emulator case) |
| Objects missing from the store mid-session | `503` from the preview server, as for any preview |

## Tests

### `tst_kelppreview` (202 checks today) — adapted, plus the new shape

- Mechanical: every `KelpPreviewServer server(&store)` becomes `server` plus
  `server.open(&store, …)` wrapped in a `StorePtr` (a helper in the test keeps that
  readable); the `openEntry` call sites likewise.
- New checks: two sessions over two different `MemoryStore`s with different keys are served
  from their *own* store — a request into session A's token for an object only store B
  holds is a 404/503, and vice versa; closing A leaves B answering; and a session whose
  opener has released its reference keeps answering until the session itself is dropped,
  then the store is freed (the D4a property, tested directly).

### `tst_kelpclient` — new, small, and the reason D7 exists

A stub `QTcpServer` on `127.0.0.1` answering `/info` and `/object/<id>` out of a store that
already holds a sealed bundle (the same house style `tst_kelpfetch`/`tst_kelppreview`
already use; the bearer header is ignored). Checks:

- a page link fetches into the client's **memory** store, writes **no file** into a temp
  download directory, and leaves the app store it was constructed with **empty**;
- `openPageView()` returns a URL whose path is the session's token, and the preview server
  answers over that URL from the memory store;
- a bundle whose `/info` claims more than the ceiling is refused **after `/info` alone** —
  the stub counts object requests and must have seen none;
- a bundle whose `/info` under-claims but whose objects exceed the ceiling fails while
  storing, with the ceiling wording;
- a `p` bundle with no page gives §5.5's "no web page in it" answer.

A hook-free, browser-free suite: no Qt GUI, no emulator, so it runs in the normal
`run-all-tests.sh` pass alongside the rest.

### Device verification (the part that is evidence, not logic)

On the emulator, against a real i486 build, with a temporary startup hook standing in for
taps the emulator cannot receive (as in C4a/C5.1):

1. publish a page share in the app (or reuse the row), take its link;
2. drive the Open-link page's own functions with the hook: `parsed.page === true`,
   no Save-to section, the button label, `fetchForViewing`, then `openPageView`;
3. `curl` the session URL from inside the guest and check the page HTML arrives — the
   browser-less proof §12.9 names;
4. check the two things that define the feature: **the app's object store did not grow**
   (count before/after) and **Downloads did not gain a file**;
5. check the log's view lines (store size, drop) and that the KeepAlive line shows
   `1 previewing`;
6. screenshots of the view-only page and its Done panel;
7. then the same link on the C2 with a real browser, and a check that nothing was saved;
8. leave the device clean: no demo folders, no hook, one app process.

## Out of scope, stated so it is not quietly assumed

- Keeping the row (id + key) — §12.13, unchanged.
- A remembered per-bundle consent — §12.10, parked; every view is a fresh press.
- Choosing among several HTML files — the §5.3 pick is served.
- Serving a received page to anyone else, re-seeding, or re-sharing it.
- Lazy fetch (which is what would lift the ceiling) — §5.1's open item.
- Letting the recipient override the kind and save anyway — a webpage link is view-only.

## Risks and traps

- **Store lifetime** is the one real hazard, and D4a is the answer: the session holds a
  reference, so the client can replace or clear its own without dangling. What must *not*
  creep in is a bare raw pointer handed to `open()` "because the tests are easier" — that
  is exactly the shape D4 makes unsafe.
- The ceiling is the **sender's** claim until objects arrive; the storing-time check is not
  optional.
- `linkInfo` must actually fill `page` (C5.1 added it) — the QML branch is dead code if it
  silently returns nothing, so the hook prints `parsed.page` for a real link.
- The KeepAlive change touches the owner's path too (a preview without seeding now holds
  the request) — that is §5.4's rule, but it is a behaviour change to state in PROGRESS.
- `Qt.openUrlExternally` reporting `true` while nothing opens is expected on the emulator;
  do not read it as success without the `curl`.
- The view store is memory only: `MemoryStore::byteCount()` is the honest number to log,
  and there is deliberately no disk cache to reuse between views.
- D4 means a viewed page stays in RAM until the 30-minute sweep. That is the user's
  choice, not an oversight — but it belongs in the README's "what this does not do", next
  to serving stopping when the app closes.
- **Added with D8 (§3.25):** a QML *change handler* runs before a derived binding has been
  re-evaluated, so reading a property like `viewOpen` inside `onPhaseChanged` answers for
  the state *before* the change. The imperative paths must ask `KelpClient` directly
  (`viewIsOpenNow()`); the emulator probe caught exactly this — `phase=done viewOpen=false
  viewIsOpenNow=true`. A trap that would have re-introduced the bug in a different mask.

## Size

Roughly: `kelppreviewserver` signature + session/store plumbing; `kelpclient` mode, store,
ceiling, open/end; the QML branch; `harbour-kelp.cpp` wiring; the two test changes; docs
(design §11 C4a.2 → built, PROGRESS §3.22, README, BUILD.md counts). Expected to touch one
existing suite and add one.
