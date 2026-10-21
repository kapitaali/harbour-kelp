# A browser door for a folder — plan (design §5.2, §12.12)

**Status: withdrawn 2026-10-21.** The relay address field was removed from the app, and
with it *Copy browser link* and the folder door: the app shares and fetches on the local
network only, so it cannot mint a browser address. This plan is history. The gateway and
its ZIP view remain separate for a sender that has a relay.

> The earlier status was *built 2026-10-21 — placement A, QML only*; evidence is in
> `PROGRESS-20261016.md` §3.41. The rest of this document records that plan, not current
> behaviour.

## 1. The gap

§12.12 gives a bundle with **no page** a browser view: its file list, per-file downloads and
*Download folder* as one ZIP. That view lives at `https://<relay>/b/<id>#q=…&k=…`, and the only
thing in the app that mints such an address is **Copy browser link** on a share's page.

But that button is offered only when the bundle is a **webpage**:

- `BundlePage.qml` — `browserLinkOffered` starts with `isWebpage && …`;
- the section header *Web page — shared as one*, the *Copy browser link* button and its note are
  all `visible: bundlePage.isWebpage`;
- the comment there says it outright: *"A folder share holding a page gets no section and no
  button: its files are what a recipient gets, and what to do with them is theirs."*

So a folder share has **no door** to the view §12.12 built for it. The phone run in §3.40 used a
URL assembled from the gateway fixture, not one the app could have produced. In production a
recipient without Kelp would have to edit a `kelp://` link by hand — which is not a flow.

The two decisions crossed: 2026-10-18 dropped the *preview* for folders (correct — there is no
page to run), and 2026-10-20 then gave folders a browser *download* view, without revisiting the
gate where the URL is minted.

**What is not in question:** *Open in browser* (the app's own preview) stays webpage-only. A
folder has nothing to run, and the 2026-10-18 decision stands.

## 2. What exists, and why the change is small

- `KelpBundles::browserUrl(bundleId, secret, relay, link)` takes any share, not just a webpage. It
  reuses the link's `q=` block only when the link decodes to that very bundle id and secret, and
  otherwise mints `#k=` alone. A folder's link carries `q=` with `p:false` and its label, so the
  same call already yields the right URL, and the gateway names the ZIP from it.
- The gateway needs no change: a bundle with no page already gets the file list and the ZIP
  (S1/S2, §3.39/§3.40).
- The relay needs no change: same endpoints, same ciphertext.

So the work is the page's offering, its copy, and the tests — no protocol, no relay, no
permission, no new menu.

## 3. Proposal (placement A, recommended)

Generalise the one existing browser section instead of adding a second one:

- Header: **In a browser** (was *Web page — shared as one*), visible when the share is a webpage
  **or** a browser link can be offered.
- *Open in browser* and the page description stay `visible: isWebpage` — unchanged behaviour for
  webpages, absent for folders.
- *Copy browser link* is shown for both kinds, enabled by:

  ```
  browserLinkOffered = !paused && linkParts && linkParts.ok
                       && !linkParts.needsPassphrase
                       && linkParts.relays.length > 0
  ```

  (`isWebpage` drops out; the passphrase case stays excluded until the separate `#p=` slice.)
- The note branches by kind. Webpage: exactly today's text. Folder:

  > Copy this for someone who has no Kelp: their browser fetches the folder from the relay and
  > decrypts it there — the files one at a time, or all of them as one ZIP. It works only while
  > this share is being served, and it carries the same key as the link above.

- A `browserSectionShown` property (`isWebpage || browserLinkOffered`) drives the header, the
  note and the button block, so a folder with no address has no section at all.

- A paused share, an unreadable link or a missing relay keeps the existing explanations. For a
  folder with no relay the section can stay hidden (the app-to-app link still works); for a
  webpage it must stay visible, because that is where the "needs a relay" sentence lives.

Placement B, if preferred: leave the webpage section untouched and add a second section *In a
browser* below it, visible for non-webpage shares with a relay, containing only the button and the
folder note. It is a little more UI for the same result; A keeps one place where a browser is
discussed.

## 4. Copy that changes

- `browserLinkNote()` gains the folder sentence above; the webpage sentence is untouched.
- The comment above `browserLinkOffered` stops saying a browser link is webpage-only, and says why
  the *preview* still is.
- The section comment stops claiming no folder gets a section.

No other wording changes: the button stays *Copy browser link*, the URL is still copied and never
logged, and the passphrase sentence (*A browser cannot ask for the passphrase…*) remains true until
`#p=` is built.

## 5. Tests

- `tst_kelpbundles`: already covers the URL shape — the four-case section seals a folder
  (`page=false`) and checks `#q=…&k=…`, the `#k=` shape without a link, and the foreign-block
  fallback. No new unit case is needed unless the guard changes.
- QML smoke: every page still `Ready` (9/9).
- Emulator/device: make a folder share, confirm the section and a live button, copy the link, open
  it in a browser against the gateway tree (recipe §4.8) and check the list and the ZIP; confirm no
  *Open in browser* appears for the folder; confirm a passphrase share still refuses.
- Nothing on the relay; gateway suite unchanged.

## 6. Acceptance

1. A folder share's page offers **Copy browser link**; the URL opens the folder's file list and
   *Download folder* in a browser.
2. A webpage share's page is visually and behaviourally unchanged.
3. A passphrase share still offers no browser link; a passphrase *webpage* keeps its preview and
   the existing sentence, while a passphrase *folder* has no browser section.
4. A paused share or one without a relay explains itself as today.
5. No *Open in browser* on a folder; no new menu; nothing logged.

**As built (2026-10-21).** Placement A, QML only, in `qml/pages/BundlePage.qml`. Both kinds are
checked on the emulator's real page in `PROGRESS-20261016.md` §3.41, together with the passphrase
and no-relay states. One consequence of the rule above: a passphrase **folder** has no browser
section at all (there is no door to explain), while a passphrase **webpage** keeps the existing
sentence beside its preview. A folder with no relay is likewise hidden; the app-to-app link still
works.

## 7. The decision

The 2026-10-18 note ("a folder share gets no section and no button") was about **previews**, and it
stands. §12.12 then built a folder **download** view with no door. Should the door be added?

- **Yes, placement A** (one section, *In a browser*, *Open in browser* still webpage-only) —
  recommended.
- **Yes, placement B** (a separate folder section).
- **No** — keep *Copy browser link* webpage-only; then §12.12's folder view stays reachable only by
  a hand-made URL, and that should be written down as the product's answer.
