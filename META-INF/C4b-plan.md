# C4b — the relay gateway (design §5.2, §5.3)

A recipient with **no app at all**: a browser opens a URL, and the page comes out. The relay
carries ciphertext; the browser holds the key, in the fragment, and decrypts locally.

This is the second half of "just send them a link". The first half (`§5.1`, `C4a`/`C4a.2`) is a
recipient with the app, whose own app previews the page on `127.0.0.1` and hands that URL to the
browser. Here there is nothing on the recipient's device but a browser, so the *relay* is the one
that hands out a URL, and the browser is the one that decrypts.

## Why this exists right now

- The relay is built (C3): one port, TLS 1.3, ALPN `kelp/1`, tunnels plus token-gated fetches.
- The preview convention is settled (§5.3): `index.html` at the root, else the shallowest, else
  the sole HTML file — nothing new on the wire (§5.5's `p` hint is the publisher's *kind*, and is
  what decides *whether* to offer the browser at all).
- What is missing is only the browser's door: an ALPN the browser speaks, a page to hand it, and
  the code that turns ciphertext into a page in a tab.

## The behaviour, restated as a checklist

1. The publisher shares a **webpage** (or any bundle). The app can show a second URL for it:

   ```
   https://<relay>/b/<bundle-id-b32>#k=<secret-b64url>
   ```

   Same relay the link's `r=` names; the fragment is the same secret the `kelp://` link carries.
2. A browser opens it. It must be a **secure context** — HTTPS with a certificate the browser
   accepts (loopback `http://localhost` during testing also qualifies).
3. The relay answers `/b/<id>` with a self-contained **loader** page. The loader has no key: the
   fragment is never sent to any server, by any browser (it is after `#`).
4. The loader derives `relay_token = HKDF-SHA256(secret, salt=∅, info="kelp/v1/relay")`, exactly as
   the app does, and fetches the root manifest and every object from the **existing** endpoints:

   ```
   GET /v1/relay/<relay_token_b64url>/bundle/<bundle_id_b32>/info
   GET /v1/relay/<relay_token_b64url>/bundle/<bundle_id_b32>/object/<object_id_b32>
   ```

   It verifies each object's id (SHA-256 of the bytes) *before* decrypting — the relay can
   withhold, never substitute (wire spec §12.1).
5. Objects are AES-256-GCM (`0x01 ‖ nonce[12] ‖ ct ‖ tag[16]`, AAD `kelp/v1/object`), and
   manifests are the canonical CBOR of §3. The loader does both in the browser (WebCrypto +
   ~150 lines of CBOR reader).
6. A bundle that has an entry document (§5.3) is served as a **page**: the loader registers a
   service worker over `/b/<id>/`, hands it the decrypted tree and the key, and the SW answers
   every request under that scope with the decrypted, verified object whose MIME the manifest
   recorded. Relative URLs, `fetch`, XHR and subresources all work, because the page's own URL
   space *is* the bundle's (§5.3).
7. A bundle with **no** HTML gets the loader's own **file list**: name, size, per-file download
   (the loader decrypts, makes a Blob, and downloads it). This is the floor §5.2 promised, so a
   gateway link to an ordinary folder is not a dead end.
8. Nothing is written to the relay. It holds only what C3 gave it — `relay_token`, object counts,
   and a live tunnel — never the content key.

## Decisions taken here (each is a knob)

**D1 — The loader reuses the token-gated endpoints; the gateway adds no data path.** The browser
derives `relay_token` from the fragment (WebCrypto HKDF-SHA256), so the only new URL is
`/b/<id>`, which serves a static page and nothing secret. The alternative — a token-less
`/b/<id>/o/<oid>` — would make every published bundle's ciphertext enumerable by id, and id
guessing is exactly what the token path refuses today. The gateway inherits that.

**D2 — The page's URL space is the bundle's path space**, under `/b/<id>/`. §5.3 already fixes the
web root and the URL space for the app's preview; the same rule with the relay's prefix is what
makes one page work in both places. `/b/<id>/` is the entry document, `/b/<id>/sub/x.png` a file,
`/b/<id>/sub/` serves `sub/index.html` when one exists.

**D3 — Service worker, not blob rewriting.** Rewriting HTML/CSS to `blob:` URLs breaks absolute
paths, dynamic fetches, and anything the page does at runtime. A SW intercepts all of it. Cost:
the first load is bootstrap → register → reload, and a SW without a key (a fresh tab, a browser
restart) serves the bootstrap again so the fragment can be re-read. The SW lives only in memory:
no key reaches disk.

**D4 — Passphrase links (`#p=`) are refused in v1** with a plain message: "this link is protected
by a passphrase — open it with the Kelp app". Unwrapping is PBKDF2-HMAC-SHA256 with the work
factor in the blob, which WebCrypto supports; but a browser cannot show *why* a link is safe, and
the app is the tool built for it. Adding it later needs no format change.

**D5 — No new ALPN, one new accepted protocol.** The relay keeps `kelp/1` for tunnels and peers,
and additionally accepts `http/1.1` so a browser can connect. The tunnel role still demands
`kelp/1`; a connection that negotiated `http/1.1` is a recipient and may only use the HTTP paths
it already could. The app is unaffected.

**D6 — The gateway is honest about what it is.** The design says it plainly (§5.2): the loader is
served *by the relay*, so a malicious relay can serve a loader that steals the fragment. The page
says so in one line, with the app's route named as the safer one. `Content-Security-Policy` on the
loader forbids anything external (`default-src 'self'`), which stops a *third* party, not the
relay.

**D7 — Non-page bundles get a list, not a ZIP**, in v1. A ZIP of a large bundle is a second
memory copy of everything; the list downloads one file at a time.

## The seams, exactly

### `src/kelptls.{h,cpp}` — one server, two ALPNs

```cpp
// Server only; before start(). Defaults to { "kelp/1" }.
void setAlpnProtocols(const QStringList &protocols);
```

`alpnSelect()` currently refuses anything but `kelp/1`. It becomes a list match against the
configured set, and the post-handshake check accepts any negotiated value in that set (the client
end keeps offering, and requiring, `kelp/1` alone). `alpnProtocol()` already reports what was
chosen; `KelpTunnel` and the recipient code are untouched.

### `relay/relay.{h,cpp}` — the loader is the only new endpoint

After the role decision, a request whose path begins `/b/` is served from the embedded loader;
everything else is unchanged. Only **two** files are needed:

- `GET /b/<id>` (and `/b/<id>/`) → `loader.html`, `text/html; charset=utf-8`, with a CSP header.
  `<id>` must decode as a 32-byte bundle id; otherwise 404 (`/b/` is not an enumeration oracle
  either — the *page* is public, and says nothing about whether a bundle exists).
- `GET /b/<id>/sw.js` → the worker script, `text/javascript`. A SW script must be a real response
  with a JS MIME type and must not be the bootstrap page, so it is a separate path.

The loader derives the token; every data request goes to the C3 paths already implemented. A
bundle with no live tunnel answers exactly as it does today (404 from the object path).

### `relay/gateway/{loader.html,kelp-loader.js,kelp-sw.js}` — the client half

Compiled into the relay with a `.qrc` (`relay/gateway.qrc`), so a relay binary is one file and
nothing is read from disk at runtime.

- `loader.html` — the bootstrap: reads `location.hash`, refuses `#p=` (D4), computes the token,
  fetches `/info` and the root manifest, and then either registers the SW and reloads (a page) or
  renders the file list. The tree/key handover to the SW is a `postMessage`.
- `kelp-loader.js` — the core, written as plain functions with no DOM dependency: CBOR reader,
  `openObject(key, bytes)`, `sha256(bytes)`, `walk(root)` → `{ files: {path → {id, size, mime}},
  entry, webRoot }`, plus the DOM parts behind a thin edge. The split exists so the core can be
  tested in Node against fixtures the C++ side writes (see Tests).
- `kelp-sw.js` — `install`/`activate` + `fetch`: for a request under the scope, look up the path
  (resolving `/sub/` to `sub/index.html`), fetch the object by id, verify, decrypt, and answer with
  the recorded MIME. No key yet → serve the bootstrap (network fetch of `/b/<id>`) so the fragment
  can be read again. The key stays in the worker's memory and dies with it.

### The app, later and small

`BundlePage` gains **Copy browser link** for a webpage share when Settings → Relay names one: it
is `<relay>/b/<bundleId>#k=<secret>`. Same rule as everywhere else: the fragment is never logged.
Not in the first slice — the emulator test constructs the URL by hand.

## Failure modes, each mapped to the code that answers it

| Situation | What happens |
|---|---|
| Not a secure context (self-signed `https://`, plain `http://` not loopback) | `navigator.serviceWorker`/`crypto.subtle` are absent; the bootstrap says the gateway needs HTTPS |
| No key in the fragment | bootstrap asks for a link, says the `#k=` part is missing |
| `#p=` (passphrase) fragment | D4 message; the app is named |
| `#k=` present but wrong/foreign | every object fails its id check; the loader reports "the relay sent something that does not match" |
| No live publisher tunnel | `/info` 404s; "the sender is not online" |
| A webpage's subresource is missing | recorded MIME is served only for objects that verify; a missing object answers 404, as on the app path |
| A bundle with no HTML | file list (D7) |
| A hostile relay substitutes an object | id check fails before decryption |
| A hostile relay serves a hostile loader | inherent (§5.2); stated on the page, D6 |

## Tests

### `relay/test` (the existing harness) — the new endpoints and the new ALPN

- a browser-shaped client (ALPN `http/1.1`, `GET /b/<known id>`) gets the loader page, and its own
  ALPN is reported as `http/1.1`;
- `GET /b/<unknown id>` gets the page too — the loader is public (D1) — while
  `GET /v1/relay/…/<bundle id>/info` with a browser's ALPN still answers 401/404 exactly as the
  `kelp/1` path does;
- `/b/<id>/sw.js` is served as JavaScript, and `/b/<malformed>` is a 404;
- the tunnel cases are unchanged: `kelp/1` still required for REGISTER/WANT/DATA.

### Gateway fixtures → Node (`relay/gateway/test`)

The JS core is tested against **real seals**, not hand-written vectors: a case in
`tst_kelpbundles` seals a small known tree (a file, a subfolder, and an `index.html`), writes
`KELP_FIXTURE_OUT` JSON — bundle id, secret, every object base64 — and `run-gateway-test.sh` runs
the C++ suite inside the SDK, then Node on the host against that file:

- CBOR reader: the exact manifestations `Manifest::encode*` produces (canonical key order, no
  indefinite lengths);
- `openObject`: decrypts what `Kelp::Object::seal` sealed, and refuses a flipped byte;
- `walk`: entry selection per §5.3 for the three shapes (root `index.html`, nested, sole HTML);
- path resolution: `/`, `/sub/x.png`, `/sub/` and directory-vs-file decisions;
- a hostile-object case: an object whose id does not match is rejected before decryption.

### Device verification (the part that is evidence)

The relay runs **in the guest** (so the browser can reach it on a secure context, and the app can
dial it as a relay). Publisher: the app, sharing a webpage with Settings → Relay set to that
relay. Browser: the emulator's own browser opening `https://127.0.0.1:<port>/b/<id>#k=…`.

The certificate is the one real prerequisite: a self-signed certificate's click-through page is
*not* a secure context, so WebCrypto and the SW are unavailable. The test therefore generates a
local CA, signs the relay's certificate with it, installs the CA in the guest's trust store
(`/etc/pki/ca-trust/source/anchors` + `update-ca-trust`), and verifies the loader and its crypto
are available. If the guest browser will not trust a local CA, the honest fallback is stated in
§9: endpoints verified by script and the core by Node, browser check pending a real certificate.

## Out of scope, stated so it is not quietly assumed

- **Relay-side storage or caching.** The gateway is a pass-through plus a loader; a relay that
  caches is a different feature (§12.13's neighbourhood).
- **Range requests / streaming.** Whole objects only; the manifest is walked whole.
- **ZIP download** of a non-page bundle (D7).
- **Passphrase links at the gateway** (D4).
- **Swarm at the gateway.** The recipient path is a tunnel; peer swarms are C7.
- **The app's Copy browser link** button (the seam above is written down, not built).
- **h2/HTTP/2.** A browser offering `h2,http/1.1` is given `http/1.1`.

## Risks and traps

- **The secure-context prerequisite is not a detail.** Without a browser-trusted certificate the
  gateway does not work at all, and that is a deployment question (a domain, a real cert), not a
  code one. Whoever runs a public gateway needs both. This is the single biggest thing to say out
  loud about C4b.
- **The SW lifecycle.** `register()` resolves before the worker *controls* the page; the bootstrap
  must wait for `navigator.serviceWorker.controller` (or `clients.claim()` in `activate`) and
  reload exactly once, or the first load shows the bootstrap as if it were the page.
- **MIME types decide what the browser does with the bytes.** A page's CSS/JS must come back with
  the manifest's recorded MIME; when a file has none, the loader maps a small extension table and
  defaults to `application/octet-stream` — a wrong guess is a broken page, not a security hole.
- **Memory.** A big bundle in a SW means big buffers; objects are fetched per request and dropped
  after answering (the tree, not the bytes, is what the worker keeps).
- **CSP on the loader page** must allow the SW and blobs but nothing external (`default-src 'self'`),
  and must not block the fragment (it never leaves the page).
- **`/b/` and the tunnel path share one port and one TLS server.** The role decision is by first
  byte, as today; the only new thing is that a connection may negotiate `http/1.1`, after which
  only HTTP is possible on it.

## Size

| Piece | Lines (est.) |
|---|---|
| `kelptls` ALPN list | ~40 |
| `relay` loader endpoint + qrc | ~80 |
| `loader.html` + CSS | ~120 |
| `kelp-loader.js` (CBOR, crypto, walk, DOM, file list) | ~420 |
| `kelp-sw.js` | ~120 |
| relay harness cases | ~120 |
| fixture emission in `tst_kelpbundles` + Node tests | ~200 |
| docs | ~100 |

Roughly a week of careful work, in four slices: (1) ALPN + loader endpoints with relay tests,
(2) the core JS with Node fixtures, (3) the SW and the page/file-list UI, (4) the emulator
browser test and the app's Copy browser link.
