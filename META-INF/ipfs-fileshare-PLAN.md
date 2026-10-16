# IPFS File-Sharing App for Sailfish OS — Implementation Plan

> Companion doc: **`kelp-design.md`** — a speculative "same features, no IPFS, tiny
> footprint" design, if the ~100 MB Kubo dependency proves unacceptable. The two share
> ~80% of their code (UI, links, encryption, gateway); only the transfer/address layer
> differs, and both sit behind the same `Backend` interface.

Working app name: **`harbour-ipfsdrop`** (placeholder — rename freely; the `harbour-`
prefix is kept so a future Jolla Store variant can reuse the name).
Target device: Jolla C2 (aarch64, Sailfish OS 5.x). Secondary: armv7hl.

---

## 1. Reality check — one of your choices can't be combined

Your four answers were:

1. Hosting: **phone-hosted Kubo node** (bundle the daemon on the phone)
2. Distribution: **Harbour-first** (Jolla Store)
3. Recipient access: **browser link via gateway**
4. Privacy: **both public and encrypted modes**

Choices 1 and 2 are, **as of today, mutually exclusive.** Harbour permits exactly
**one ELF binary** in the package, at `/usr/bin/<harbour-app>`, and `rpmvalidation.sh`
rejects any second executable (wrong location / "file must not be executable"). This
is not a soft rule — it's actively being contested in two Sept-2026 forum threads
where real apps (Piirit bundling `deltachat-rpc-server`, Ferry bundling `rclone`,
TimeLapse Tools bundling `ffmpeg`) were rejected for exactly this.
(Source: forum.sailfishos.org threads 33807 and 32476.)

A phone-hosted Kubo node means shipping a ~70–110 MB `ipfs` binary. Under the
current rules that **cannot ship through Harbour**. So:

| Combination | Viable? |
|---|---|
| Phone-hosted Kubo as a *separate* binary + Harbour | ❌ blocked by validator today |
| Phone-hosted Kubo + OpenRepos/Chum | ✅ |
| **IPFS compiled into the app's single binary** (Go `c-archive`) + Harbour | ⚠️ likely passes the automated validator; engineering + manual-review risk (§5 Option B) |
| IPFS as a private shared object (Go `c-shared`) + Harbour | ⚠️ a second ELF *file*, but in the one location Harbour tolerates (§5 Option B) |
| Remote pinning service (no daemon) + Harbour | ✅ (see §5 Option C) |

**Recommendation:** the "compile IPFS into our own binary" route (Option B, §5) should
be the **primary Harbour strategy**, pursued as a spike alongside the NAT spike. Keep
OpenRepos as the guaranteed fallback and Option C as the reliability fallback.
Structure the backend so all three are swappable.

Also note a **functional** risk that no store decision changes: a phone on a carrier
NAT is *not directly dialable*. Browser-gateway retrieval only works while the node
is online **and** reachable via a public relay (libp2p circuit v2). §3.1 and §6 cover
how we get there; §7 flags the failure modes honestly.

---

## 2. Recommended architecture (phone-hosted)

The app/UI/backend is identical across two *build variants* that differ only in how
the IPFS node is delivered:

- **Variant A — separate `kubo` package (OpenRepos/Chum).** Fastest to working
  software; two RPMs as drawn below.
- **Variant B — IPFS compiled into the app binary (Harbour candidate).** Same code,
  but the node comes from a Go `c-archive` statically linked into
  `harbour-ipfsdrop` — see §5 Option B. This is the variant that can plausibly ship
  to the Jolla Store.

Everything below applies to both; only §3.2 (lifecycle) and §3.7 (packaging) fork.

Two RPMs (Variant A), cleanly separated:

```
┌────────────────────────────────────────────────────────────┐
│  harbour-ipfsdrop  (Silica/QML UI)                          │
│    MainPage · AddFlow · ItemPage · SettingsPage · CoverPage │
│    C++ backend: IpfController (QProcess + QNetworkAccess)   │
└───────────────┬────────────────────────────┬────────────────┘
                │ spawns / attaches           │ HTTP RPC (127.0.0.1:5001)
                ▼                             ▼
┌────────────────────────────────────────────────────────────┐
│  ipfs  (Kubo daemon, upstream linux-arm64 static binary)    │
│    repo: app-private dir · API+Gateway on 127.0.0.1         │
│    Swarm: outbound + relay/circuit-v2 for inbound reach     │
└───────────────┬────────────────────────────────────────────┘
                │ bitswap / DHT / relay
                ▼
        Public IPFS network ── public HTTP gateway ── recipient browser
```

- **Package `kubo`** — the upstream static binary (pinned version + SHA-256/SHA-512
  verified at build time), installed to `/usr/bin/ipfs` or app-private
  `/usr/share/harbour-ipfsdrop/bin/ipfs`. Keep it as its own RPM so users update
  Kubo independently and the app's `Requires: kubo` is honest.
- **Package `harbour-ipfsdrop`** — the Qt app; talks to Kubo via its HTTP RPC API
  on loopback, and launches it with `QProcess` when not already running.

### Data flow: "add a folder and share it"

```
pick folder  →  [optional] build ZIP + AES-256-GCM encrypt  →  ipfs add -r -q
   →  ipfs pin add  →  ipfs routing provide <cid>  (publish provider record NOW)
   →  build link  →  copy / Share / QR
```

Both public and encrypted modes drop out of the "optional encrypt" step (§3.3).

---

## 3. Detailed design

### 3.1 Kubo runtime & configuration

- **Repo location:** app-private data dir (writable inside Sailjail), e.g.
  `~/.local/share/<Org>/<App>/ipfs`. Set `IPFS_PATH`. Consequence: the repo is
  removed on uninstall (documented; originals stay where the user put them).
  If we later ship a systemd **user** service (un-sandboxed), it can own
  `~/.local/share/ipfs` instead — see §3.2.
- **Init:** `ipfs init --profile=lowpower` (exists upstream; cuts bitswap/DHT
  aggressiveness for constrained devices), then JSON config edits:
  - `Addresses.API` = `/ip4/127.0.0.1/tcp/5001` (loopback only)
  - `Addresses.Gateway` = `/ip4/127.0.0.1/tcp/8080` (local preview; not exposed)
  - `Swarm.DisableNatPortMap` = `true`
  - `Swarm.RelayClient.Enabled` = `true`, AutoRelay on → gives the phone a public
    `/p2p-circuit` address that gateways/peers **can** dial (key to NAT reachability)
  - `Routing.Type` = `auto` (DHT client + delegated routing)
  - `Datastore.StorageMax` = user-capped (default 4 GB) + `Datastore.GCPeriod`
  - `Pubsub.Enabled` = `false`; WebUI plugins none
  - `Reprovider.Interval` left default, but **we call `ipfs routing provide`
    explicitly right after `add`** so the link works immediately, not in up to 22 h.
- **Arch:** bundle per-arch binaries: `linux-arm64` for C2, `linux-arm` for
  armv7hl, **and `linux-amd64` for the i486 emulator** (the emulator is x86 — the
  arm64 binary will not run there). Select the right one via the spec `%ifarch`
  and/or `QSysInfo::currentCpuArchitecture()` at runtime.

### 3.2 Daemon lifecycle (the hard part)

- **v1 — in-app, sandboxed.** App spawns `ipfs daemon` as a child via `QProcess`
  and attaches if a daemon is already listening (poll RPC `/api/v0/id`; respect
  `repo.lock`). Child inherits the app's Sailjail sandbox. Stops when the app is
  killed. While the app is backgrounded, Sailfish keeps it alive (apps aren't
  frozen) — so content keeps serving, subject to keepalive/battery policy.
- **Keepalive policy.** Use `Nemo.KeepAlive` **only** while the user has "Sharing
  active" **and** (default) on Wi-Fi **and** battery > 20%. Auto-stop on low
  battery; resume on charge. Poll MCE/`/sys/class/power_supply/BAT0` (Base perms)
  slowly (slow `Timer`, never a live binding — ANR pitfall).
- **v2 — always-on seeding (OpenRepos only).** Ship a systemd user service so the
  node serves with the app closed and across reboots. The app detects an
  externally-managed daemon and switches to "attach" mode, hiding its own
  start/stop controls. This is the mode a "share and trust it's up" user expects,
  but it's a bigger battery/privacy commitment — off by default.

### 3.3 Encryption subsystem (public + private modes)

- **Public mode:** plain `ipfs add -r`. Link = `https://<gateway>/ipfs/<cid>`.
  Anyone with the CID can fetch. Warn clearly in the UI.
- **Private mode:** build a single archive (ZIP, store method) of the folder, then
  encrypt with **AES-256-GCM** using a random 256-bit key, chunked with a
  per-chunk IV + counter (so there's no 64 GB single-message limit and the browser
  can decrypt chunk-by-chunk). Store the container with a small magic/version
  header. On-device crypto via **OpenSSL 3 `libcrypto`** (`EVP_*`) — allow-listed.
  Generate the key with a CSPRNG; keep it in **`Sailfish.Secrets`**, not plain JSON.
- **Key transport:** the key rides in the **URL fragment**
  (`https://<gateway>/ipfs/<decryptor-cid>#blob=<blob-cid>&k=<key>`), which browsers
  **never send to the server** — so the gateway/CDN sees only ciphertext and the
  blob CID, never the key.
- **Decryptor page:** a self-contained HTML+JS (WebCrypto AES-GCM + a small unzip
  routine) that we also publish to IPFS; the link points at it. It fetches the blob
  from the gateway, decrypts in-browser, and offers per-file and "download all".
  Also mirror it on a static host so the link works even if the decryptor CID isn't
  resolvable yet.
- **Honest limitation to document:** anyone who gets the full link (including the
  fragment) can decrypt; nothing stops the recipient forwarding it. Fragments leak
  via copy/paste logs, screenshots, etc. Say so in the UI.

### 3.4 Share links, gateways, QR, sharing out

- **Gateway:** user-selectable list — `dweb.link`, `ipfs.io`, `cf-ipfs.com`,
  `4everland`, or a **LAN gateway** (the phone's own `http://<phone-lan-ip>:8080`,
  same-network only). Default `dweb.link`.
- **Also expose** the raw `ipfs://<cid>` for power users.
- **QR:** compile in Nayuki's `QR-Code-generator` (MIT, header-only-ish) into the
  C++ backend and expose a `QQuickImageProvider` — no extra RPM dependency, so it
  stays Harbour-clean for the Option-B/C future.
- **Share out:** `Sailfish.Share 1.0` (allow-listed) to hand the link to
  SMS/email/other apps; plus copy-to-clipboard via Qt Quick `Clipboard`.
- **Optional later:** IPNS (`ipns://<name>`) for a "living folder" that keeps a
  stable link as contents change. Deferred (adds key-management + resolution lag).

### 3.5 UI (Silica) — pages and cover

- **MainPage** — `SilicaListView` of shared items: name, size, kind (file/folder),
  pinned, "online" state, last-provided time. `PullDownMenu`: Add files / Add
  folder / Daemon status / Settings. `ContextMenu` on long-press: Copy link, Show
  QR, Stop sharing (unpin + GC), Re-announce now.
- **AddFlow** — `Sailfish.Pickers` `FilePicker` for files. **Folders are a gap:**
  Sailjail's pickers have no native folder picker. Use `Qt.labs.folderlistmodel 2.1`
  (allow-listed) or a C++ `QDir`-backed model, scoped to the media dirs the app is
  granted. This is a known UX task, not a one-liner.
- **ItemPage** — folder tree (expandable), file sizes, CID, Copy, QR, Share,
  encryption badge, "keep online" toggle, and short recipient instructions.
- **SettingsPage** — default gateway, max repo size + current usage, background
  seeding toggle, Wi-Fi-only seeding, default privacy mode, re-announce.
- **CoverPage** — daemon on/off, peer count, up/down bandwidth, #shared items, a
  quick seeding `CoverAction` (all first-class Sailfish, and Harbour-review relevant).
- Use `Theme.*` throughout (C2 is 720×1600 20:9), pause timers/animations via
  `Qt.application.state`.

### 3.6 Permissions (`.desktop`)

```ini
[Desktop Entry]
Type=Application
Name=IPFSDrop
Exec=harbour-ipfsdrop
Icon=harbour-ipfsdrop

[X-Sailjail]
Permissions=Internet;Documents;Downloads;Pictures;Music;Videos
OrganizationName=org.example
ApplicationName=IpfsDrop
```

- `Internet` is what grants the inet/inet6/netlink sockets Kubo needs (per the
  device-API matrix). Media perms cover file/folder selection.
- Optional `Connman` (Wi-Fi state) is **not** Harbour-allowed — only add it if we
  commit to the OpenRepos-only path and actually use it.
- First launch with non-empty permissions shows a system grant dialog (the user
  taps Allow once); keep the set minimal and each entry mapped to a visible feature.

### 3.7 Packaging

- **`.pro`:** `CONFIG += sailfishapp`; a `scripts/fetch-kubo.sh` downloads the
  pinned tarball(s) and verifies checksums into the source tree (never at RPM
  runtime). Both `armv7hl` and `aarch64` targets built (`make distclean` between
  arch switches — see SDK ref).
- **`kubo.spec`** (separate package): installs `ipfs` + licenses; `License:` a valid
  SPDX id (Kubo is dual Apache-2.0/MIT).
- **`harbour-ipfsdrop.spec`:** `Requires: kubo`; installs app binary, QML, icons,
  `.desktop`, and the Kudo **licenses** (`%license`) since we redistribute the
  binary. Changelog generated at build time (never hand-written).
- **Size warning:** the RPM will be tens of MB; call it out in the store listing.
- **Chum caveat:** OBS builds are controlled/offline, so it can't fetch a prebuilt
  binary. Chum would require **building Kubo from Go source on OBS** as its own
  package. Budget for that or ship via OpenRepos initially.

---

## 4. What the backend looks like (C++ ↔ QML)

- `IpfController : QObject` exposed to QML: `Q_PROPERTY`s for daemon state, peer
  count, bandwidth, storage usage, item list; `Q_INVOKABLE` for add/cancel/copy/
  provide/unpin.
- **Add** streams `ipfs add -r --progress -q <path>` via `QProcess`, parsing
  progress and taking the final line as the directory CID — far more robust than
  reimplementing chunking over RPC.
- **Status** via HTTP RPC (`QNetworkAccessManager`): `/api/v0/id`,
  `/api/v0/swarm/peers`, `/api/v0/stats/bw`, `/api/v0/repo/stat`,
  `/api/v0/pin/ls?type=recursive`.
- **Metadata** (item → CID, path, encrypted?, created, pinned, last-provided):
  `QtSql` SQLite or JSON in the app data dir; **keys in `Sailfish.Secrets`**.
- Python/`PyOtherSide` is a viable lighter backend (allow-listed) if we want faster
  iteration, but `QProcess` + multipart/progress work is more natural in C++.

---

## 5. Harbour paths (for later, because they change the backend)

- **Option B — compile IPFS into the app's single binary (primary Harbour plan).**
  This is the "our own IPFS client" route done the *right* way: **reuse the real IPFS
  code, don't reimplement it.** Concretely:

  1. Write a small **Go wrapper package** (`main` with `//export` functions) that
     imports Kubo's own packages — `github.com/ipfs/kubo/core` (`core.NewNode`) and
     `github.com/ipfs/kubo/core/coreapi`, which upstream documents as "the public
     interface you should use" when embedding IPFS in your own program. The wrapper
     exposes `ipfs_start`, `ipfs_add_path`, `ipfs_pin`, `ipfs_provide`,
     `ipfs_status`, `ipfs_stop`, etc.
  2. Build it with **`go build -buildmode=c-archive`** → `libipfs.a` + `libipfs.h`.
     A `.a` is an `ar` archive, **not an ELF file** — it is a *build intermediate*,
     never installed.
  3. Link `libipfs.a` into the C++/qmake app. The shipped RPM then contains
     **exactly one ELF**: `/usr/bin/harbour-ipfsdrop`, with the Go runtime and the
     entire IPFS node inside it. This satisfies Harbour's one-binary rule in letter
     and, arguably, in spirit (there is no second deliverable).
  4. Result: the same UI/backend, no `Requires: kubo` problem, and the emulator
     story gets *easier* (cross-compile the wrapper for amd64 too — one binary per
     arch, no staged kubo daemon).

  Why `c-archive` and not `c-shared`: `c-shared` emits a `.so` (a second ELF *file*,
  allowed only under `/usr/share/<app>/lib/`). `c-archive` is the only mode that
  yields literally one executable.

  **What this costs / the real risks (a genuine spike, not a weekend):**
  - **Not a supported embedded API.** `core`/`coreapi` are "relatively stable,"
    not frozen. Vendor + pin a Kubo version; expect to adapt on upgrades.
  - **`NewNode` is not `the daemon`.** The swarm listeners, DHT client, AutoRelay,
    reprovider, bitswap and GC are assembled by `cmd/ipfs`'s daemon machinery, not
    handed to you by `NewNode`. Wiring those subsystems from library packages is the
    bulk of the work and where surprises live (API churn, missing options).
  - **CGO + cross-compile.** `c-archive` needs `CGO_ENABLED=1` and a cross C
    compiler for aarch64/armv7hl (the SDK provides these), plus a Go toolchain
    installed *into* the build engine (the SDK does not ship Go — add it via
    `sfdk build-shell --maintain`). Pin Go to the version the vendored Kubo needs.
  - **Go runtime + Qt in one process — the main technical risk.** With `c-archive`
    Go's runtime init runs before C `main` (the good case), but Go claims
    `SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGPIPE` and runs its own GC/scheduler threads;
    any non-Go handler for those signals must chain to Go's handler, and mixed-runtime
    GC/signal interactions are a documented real-world crash source (e.g. go#78883).
    Needs deliberate testing — this is the spike's go/no-go gate.
  - **Size.** One ~100 MB ELF. Call it out on the store listing; expect slow deploys
    and notable RAM use (Kubo is memory-hungry — upstream suggests ~1 GiB per 20M
    pinned items for reproviding).
  - **Manual review still applies.** The automated validator only sees one ELF, but a
    human will see an app that boots a full IPFS node. Jolla is *currently debating*
    this exact boundary (the Piirit/rclone threads), and one author explicitly worried
    about submissions that "look like a rule circumvention." The automated pass is not
    approval — treat Option B as *plausible*, not *promised*.
  - **It does not fix the hard problems.** NAT reachability (§1) and battery (§7) are
    unchanged by embedding. This is orthogonal to them.

  **Promotion rule:** Option B becomes the shipping design only after the M0b spike
  passes its gates (§6); until then, Variant A is what actually ships.
- **Option C — remote pinning service (no daemon).** App uploads over HTTPS to a
  pinning service (Pinata/Storacha/web3.storage); links persist even when the phone
  is offline. Uses only `libcurl`/QtNetwork + `Internet` — **cleanly Harbour-safe**,
  and structurally the same UI/links. This is the natural "Harbour edition" and
  also the reliability fallback for §7's NAT problem.
- **Don't: hand-write an IPFS client in C++.** libp2p + Kademlia DHT + Bitswap +
  UnixFS + CIDs/multiformats/multibase is a multi-year reimplementation with
  guaranteed interoperability bugs. Reuse the real code; only the *delivery* of that
  code (embedded vs packaged) is the innovation here.
- Keep the backend behind an interface (`Backend { EmbeddedNode | ExternalNode |
  RemotePinning }`) so all three are additions, not rewrites.

---

## 6. Milestones

- **M0 — De-risk spike (do this first).** On a real C2, under `sailjail`: install
  upstream Kubo, `init --profile=lowpower`, start the daemon with `Internet` granted,
  confirm (a) it binds loopback, (b) it reaches the DHT, (c) `ipfs routing provide`
  + a public gateway can fetch a test CID **from a different network**, (d) whether
  firejail lets it listen on the LAN, and (e) real battery/RAM cost over an hour.
  **If (c) or (e) fails, the whole phone-hosted premise needs rework — find out now,
  for the cost of an afternoon, not after M4.**
- **M0b — Embedding spike (parallel to M0; gates the Harbour plan).** In the SDK
  build engine: install a pinned Go toolchain, write the minimal Go wrapper around
  `kubo/core` + `coreapi`, build `-buildmode=c-archive` for aarch64, link it into a
  throwaway C++ binary, and prove the single ELF can (a) boot a node, (b) add a file,
  (c) `provide` + serve a CID, and (d) survive an hour of add/serve + GC + network
  churn **without signal/GC crashes**. Then run the Harbour RPM validator against the
  one-ELF RPM. Go/no-go: if (d) fails, fall back to `c-shared` (private `.so`) or
  Variant A / Option C.
- **M1 — Scaffold.** `sfdk`-generated project, `.pro`/`.spec`/`.desktop`, backend
  skeleton (daemon lifecycle + RPC status), MainPage + Add-a-file + ItemPage CID.
- **M2 — Core sharing.** Directory add with progress, pin, immediate provide, link
  builder, Copy, `Sailfish.Share`, QR.
- **M3 — Persistence & policy.** SQLite metadata, Settings, CoverPage, keepalive/
  battery policy, storage cap + GC.
- **M4 — Encryption.** Public + private modes, AES-GCM container, decryptor page
  published to IPFS, keys in Sailfish.Secrets.
- **M5 — Packaging & hardening.** `kubo` + app RPMs for armv7hl/aarch64 + amd64
  (emulator), on-device soak test under `sailjail`, uninstall/upgrade behaviour.
- **M6 — Distribute.** OpenRepos release (both arches); decide on Chum (build Kubo
  on OBS) and/or start the Option-B c-shared spike for a future Harbour edition.

---

## 7. Risks & mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| Harbour rejects a bundled second binary | **High** | Ship via OpenRepos; pursue Option B (embedded `.so`) or C (pinning) for the Store |
| Phone unreachable behind carrier NAT → gateway can't fetch | **High** | Relay client + AutoRelay (circuit v2) so the node gets a dialable public address; optional pinning fallback; document "content is live only while sharing" |
| Recipient fetches while phone asleep/offline → timeout/404 | **High** | Keepalive policy + clear "sharing active" indicator; or remote pin for important shares |
| Battery drain / heat from a full node | Medium | `lowpower` profile, Wi-Fi-only seeding, connection caps, auto-stop on low battery via MCE |
| Sailjail/firejail blocks swarm listening / inbound | Medium | Prove in M0; fall back to relay-only (outbound) mode |
| Storage growth on the device | Medium | `Datastore.StorageMax` cap + GC; show usage; per-share size limits |
| No native folder picker under Sailjail | Medium | `Qt.labs.folderlistmodel` + media-dir permissions; purpose-built picker |
| Emulator (x86) can't run the arm64 daemon | Low | Also stage `linux-amd64` Kubo for i486 target |
| Uninstall deletes the repo | Low | Document; require originals retained; never the only copy |
| IPFS content is public by default (privacy surprise) | Medium | Make private/encrypted the *default* for anything flagged sensitive; explicit warnings |
| Go runtime + Qt in one process: signal/GC crashes | Medium (Option B) | M0b gate; chain signal handlers; pin versions; fall back to c-shared or Variant A |
| Embedded IPFS node rejected in Harbour *manual* review despite one-ELF validator pass | Medium (Option B) | Keep Option C (pinning, no local node) as the guaranteed Harbour edition; don't over-rely on the packaging trick |
| Embedded-Kubo library API churn across Kubo releases | Low | Vendor + pin a Kubo version; isolate all Kubo calls behind the Go wrapper's C API |

---

## 8. Testing plan

- **Unit (host/build-engine):** AES-GCM container round-trip, link builder,
  gateway URL assembly, QR output, spec/desktop parsing.
- **Emulator (i486):** UI/QML logic only, using the staged amd64 Kubo. Note: modem/
  sensors absent, and `sfdk deploy` topology quirks apply.
- **On-device (C2, aarch64) under `sailjail`:** the real test — add a folder,
  provide, and fetch the link **from a second network** (a phone on mobile data is
  the honest test). Watch battery/thermal over an hour.
- **Adversarial:** kill the daemon mid-add; storage-cap hit; gateway down; huge
  folder; filename edge cases; decryptor page across Chrome/Firefox/Safari mobile.
- **Logs** go to journald (`journalctl --user -f`), not a redirected file — plan
  diagnostics around that; `devel-su` needs a tty.

---

## 9. Open questions for you

1. **Store + embedding:** shall I write the M0b spike (Go `c-archive` wrapper around
   `kubo/core`, linked into one aarch64 binary + validator run) in parallel with the
   M0 NAT spike? That's the gate on whether Harbour is reachable at all.
2. **Always-on seeding:** in-app only (battery-friendly, serves while app alive) or
   a background systemd user service that serves 24/7?
3. **Reliability:** willing to rely on a public gateway + relays, or should an
   optional remote pinning service be in scope so links survive the phone going
   away?
4. **Naming:** happy with `harbour-ipfsdrop`, or you have a name in mind?
5. **Scope of "folder":** how deep/large (whole photo library? a few GB?), and do
   you want resume/partial-add for interruptions?
