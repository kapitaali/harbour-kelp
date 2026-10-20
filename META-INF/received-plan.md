# Keeping a bundle you receive — plan (design §12.13)

**Status: proposed, nothing built.** Written before any code, as this project requires. It answers
the question design §12.13 left leaning ("keep received bundles as rows that can be paused and
removed and are marked as received") *after* the two decisions that narrowed it: an ordinary share
only saves (§3.26), and a webpage link is held in memory for the session (§5.5).

## 1. The gap, as it stands today

A successful fetch writes ordinary files into Downloads and keeps **no key and no row**. The
objects stay in the store, but nothing points at them, so *Settings → Storage → Remove unused
objects* eventually frees them. Three things therefore cannot be done:

- **open a received page again** — a webpage link is viewed from the session's memory, and after
  the idle sweep or an app restart the page is gone; the link is the only way back;
- **save another copy** of a received bundle without the link (it is only in Downloads now);
- **seed** a bundle you received.

What §12.13 originally proposed — previewing someone else's page — was withdrawn for ordinary
links (§3.26: a folder share only saves). A **webpage** link is the only thing you can receive that
has a page, and it is deliberately memory-only. So the feature, narrowed by those decisions, is:

> **Keep the bundle a link gave you — if you ask — so its page can be opened again (webpage links),
> another copy can be saved (file/folder links), and it can be seeded (later, also on request).**

## 2. The consent rule

Nothing is kept unless the person asks for *that bundle*, at the end of *that fetch*:

- the Done panel for the current link gains one button, **Keep it on this device**;
- there is no "keep everything" switch in this plan, and nothing is kept silently;
- keeping stores the bundle's **key** in `shares.json` (already owner-only, `0600`). That is the one
  real privacy change, and the button says so in one line;
- the key is never logged, and neither is the link: `addBundle` already logs metadata only
  (name, kind, counts, path, page), which stays true for received rows.

## 3. What is stored, and what is not

**Stored** (one row, the same shape as a share): `name`, `kind`, `files`, `size`, `bytes`,
`objects`, `bundleId`, `secret`, `page`, plus **`received: true`**. Later slices add `trusted`
(§12.10) and `seed` (S4).

**Not stored**: the link (its address is ephemeral), any address, the passphrase, the destination
folder, the original file names beyond what the bundle itself carries.

**Why one model and one file, not a second "received" list:** `SettingsPage.shareRows()` builds the
in-use set for *Storage* from the single `bundlesModel`. A second list or file would have to be
merged in the app and in `storeUsage`/`pruneStore`, and forgetting it in one place would delete a
kept bundle's objects. One list, one `shares.json`, one tag.

Received rows are **marked, not separated**: a second page or a filter is another place to look,
and this app's list already answers "what is on this device".

## 4. Scope decisions

- **Both kinds are keepable.** A *save* link keeps files; a *webpage* link keeps a page. The page is
  the only case where "open it again later" exists, so excluding webpage links would remove the
  feature's main point.
- **A passphrase link is not keepable in this plan.** After unlocking, `KelpClient` holds the
  *unwrapped* master key; storing it would silently defeat the passphrase for anyone who later reads
  the file. The button is absent, with one sentence saying why. (A later slice could store the `#p=`
  blob and re-prompt for the passphrase instead — it keeps the wrap intact.)
- **A kept bundle does not serve by default.** It has no link and no address until the person starts
  seeding it (S4). Seeding is bandwidth and re-exposure, so it is opt-in per row.
- **No *Copy link* on a received bundle in S1–S3.** Re-sharing what someone sent you is a separate,
  explicit decision; the machinery (a republish) is already there for the day it is wanted.
- **Dedupe by bundle id.** Keeping one already in the list (your own share, or an earlier keep)
  reports *already in your list* and adds nothing.
- **Removing a received row never deletes files.** They were saved into Downloads as ordinary
  files; only the row and (eventually, through Storage) the cached objects go.

## 5. Where the key comes from (the only C++ seam)

`KelpClient` already holds everything at `Done`: `m_key` (the master secret), `m_bundleId` /
`m_bundleIdText`, `m_name`, `m_files`, `m_bytes`, `m_viewing`, and for a view the whole bundle in
`m_viewStore`. One new method:

```
Q_INVOKABLE QVariantMap keepFetched();   // {ok, error, bundleId, secret, name, kind,
                                         //  files, bytes, objects, page, viewing}
```

- refuses unless `phase == Done` **and** the fetch is the link this page is showing
  (`isHandling`), so a stale panel cannot keep someone else's transfer;
- refuses a passphrase fetch, with the sentence above;
- **save**: the objects are already in the app store; the row's counts come from the client's own
  bookkeeping (it must also remember whether the root was a file or a folder — the save path knows
  this and should not re-open a large tree just to count);
- **view**: copies the bundle's object graph from the in-memory view store into the app store —
  `Kelp::bundleObjects()` names the objects, `get` + `put` copies them, and `put` re-verifies every
  id, so a copy can never launder different bytes. Then it opens the tree once (the view ceiling is
  32 MiB) to count files/bytes and learn the kind. That is also why **K3 stays deferred**: keeping a
  page copies after the fact, so no lazy preview is needed for it;
- returns `secret` as the same base64url text `seal()` already returns, so QML appends the row with
  no new encoding rules.

**Consequence to state plainly:** a viewed page can only be kept while it is the client's *current*
fetch — the Done panel is showing. Once another link is opened, the memory is gone and the option
with it. Re-opening the link and keeping it at the end is the way back.

## 6. The UI (no new menus, no new permissions)

- **Done panel** (`OpenLinkPage.qml`, the `doneHere` column): one button, *Keep it on this device*,
  and one muted line: *"Stores this bundle's key here, so you can open it again and save another
  copy without the link."* On tap: `keepFetched()` → `addBundle(info)` with `received = true` →
  toast *Kept in Kelp*; the button becomes a disabled *Kept*. Hidden for a passphrase fetch.
- **Bundles list** (`BundlesPage.qml`): the state label for a received, not-seeding row reads
  **Received** (secondary colour) instead of *Offline*; the address line reads *on this device*
  instead of *not serving*. Once seeding (S4) the usual *Seeding* + address appear.
- **The existing context menu** becomes role-dependent — the same menu, not a new one:
  - received: *Open* · *Save a copy…* (S3, file/folder) · *Open in browser* (S2, webpage) ·
    *Start seeding* (S4) · *Remove from this device* (Remorse; `unshare` + drop the row; the objects
    become unused and *Storage* frees them, exactly the contract *Stop sharing* has today);
  - own shares keep today's four items unchanged.
- **Bundle page** (`BundlePage.qml`): Location reads *Received from a link*; Status reads
  *Received — not sharing* (or *Seeding*); the whole **Share link** section is hidden for a received
  row that is not seeding — an empty link is not a broken link, and `linkInfo("")` must not be
  allowed to print the "cannot be read" sentence. *Open in browser* shows for kept webpages (S2);
  *Save a copy…* for kept files/folders (S3).
- **The §12.10 prompt** arrives with S2, the first time a page is someone else's: *"This page came
  from someone else. It is served from this device and cannot reach the network."* with *Open* and
  *Always for this bundle*; remembered as `trusted` on the row, which is durable because the bundle
  id is the immutable content.

## 7. Slices, each shippable on its own

- **S1 — keep, list, remove.** `keepFetched()` for save links; `received` in the row and in
  `shares.json`; list label, BundlePage wording, *Remove from this device*; restore leaves the row
  not serving. No preview, no seeding, no re-share.
- **S2 — keep a viewed page**, the copy into the store, *Open in browser* again from the row, and
  the §12.10 prompt + `trusted`.
- **S3 — Save a copy…** from the local store, no network: `Kelp::open` + the existing
  `writeChildren` + the folder picker; a name collision gets the usual *"(2)"*.
- **S4 — Seed it.** `seed` persisted, *Start/Stop seeding*, policy interaction, the cover's
  pause-all / resume-all rules, link/address on the row. Re-sharing (*Copy link*) stays a separate
  decision after that.

Each slice: suites green in both dev-extras modes, QML smoke 9/9, and a live emulator pass. Fixes
and findings go in `PROGRESS-<date>.md` as §3.x, the design in §12.13 gets a "resolved" note when
the last slice lands.

## 8. Tests

- `tst_kelpclient`: `keepFetched()` refuses when idle, failed, or asked for a link the client is not
  handling; returns id/secret/name/kind/counts for a save; for a view copies exactly the bundle's
  objects into the app store and `Kelp::open` then succeeds; refuses a passphrase fetch.
- `tst_kelpshares`: a row with `received: true` round-trips; an older file without it still loads
  (migration is "missing means false").
- `tst_kelpbundles` (S3): a save-copy of a sealed bundle is byte-identical to the source
  (`diff -r` in the test), and a store with one object removed fails loudly instead of writing a
  short tree.
- **Live, emulator:** fetch a save link → keep → the list says *Received* → restart → row still
  there, nothing served → *Storage* shows all its objects **in use** → remove → *Storage* shows them
  unused. S2: view a page link → keep → restart → *Open in browser* serves it. S3: *Save a copy* →
  `diff -r` against the original folder. S4: *Start seeding* → a link is minted → a second fetch or
  the probe reads it.
- The buttons themselves are covered by the QML smoke (pages compile) and a hook that drives the two
  Done-panel states; hooks are never committed.

## 9. Traps and risks (each one is a real way to ship this wrong)

- **`saveShares()` writes `paused` from the state.** A received row in state `received` would be
  saved as *not paused*, and the next restore would start serving it. S1 must write `paused: true`
  for received rows (`received` + `paused` = "kept, not seeding"); S4 replaces this with the `seed`
  field and its own rule.
- **`restoreShares()` republishes every non-paused row.** Received rows must skip that in S1.
- **Prune correctness.** `shareRows()` comes from the one model, so received rows are counted
  automatically — and a test must pin it, because the failure mode is silent deletion of a kept
  bundle's objects.
- **`linkInfo("")` is not ok.** BundlePage must *hide* the share-link section for a received row
  rather than print the "cannot be read" sentence.
- **The view store dies with the client's next reset.** Keep is offered only while the view is
  current; the note says so.
- **One row per bundle id.** A link to your own share, received back, must not create a second row.
- **Counters.** `servedShares` counts rows with an address, and `pausedShares` counts state
  `paused`; a received row must not inflate either when it is not serving.
- **`addBundle` logs metadata only.** Keep it that way; no secret, no link, ever.
- **QML `secret` role.** It already reaches QML for own shares (`seal()` returns it), so the
  received path adds no new exposure — but it is the reason the row file must stay `0600` and why
  the button has to be explicit.

## 10. What this changes elsewhere

- **§12.13** is answered by this plan: kept bundles, marked received, consent-first, preview-later
  only for webpage links.
- **§5.5**'s promise becomes *"nothing is saved on this device unless you keep it"* — and the page's
  Done panel is where that sentence is true.
- **§12.10** (preview trust) becomes reachable in S2 and is scheduled there.
- **K3** (lazy preview) stays deferred: keeping copies after the fact, so no incomplete-store
  preview is needed.
- Permissions are unchanged: `Internet` + `UserDirs`. Seeding (S4) uses the same LAN/relay path a
  share already uses; nothing new is opened.

## 11. The four questions this plan puts to you

1. **Pages too?** Keep webpage links as well as file/folder links (recommended — it is the only
   "open it again" the app can offer), or keep only what is saved to Downloads?
2. **Passphrase links:** leave them un-keepable for now (recommended), or store the `#p=` blob and
   ask for the passphrase again on each use?
3. **Seeding:** per row, off by default (recommended), or a global switch once it exists?
4. **Re-sharing a received bundle:** leave *Copy link* off (recommended) until you ask for it?
