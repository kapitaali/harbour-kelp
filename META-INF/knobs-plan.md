# The parked knobs, the preview gap, and passphrase links — the plan

This is the design for the next batch of work, written out before any code (the standing rule).
The user picked this bucket over C4b and §12.13, with two decisions taken up front:

- **Passphrase links use PBKDF2-HMAC-SHA256** (zero new dependencies; the app already drives
  OpenSSL for AES-GCM and HKDF). Argon2id was offered and declined.
- **Low battery stops serving until charging** — not merely "stop asking to stay alive".

Four pieces, in the order they will be built. Each one is a commit with its own tests and its own
on-device verification; nothing here is a rewrite of something that already works.

| # | Piece | Size | Existing gap it closes |
|---|---|---|---|
| K1 | Persist the three settings toggles | small | Two of the three switches are forgotten on every restart, and one (`swarm`) is a lie today |
| K2 | A real serving policy: Wi-Fi/mobile data + low battery | medium | "Never seed over mobile data" does nothing; there is no battery policy at all |
| K3 | The lazy fetch in the preview (`KelpSource` behind a session) | medium | §5.1's stated gap: a missing object is a `503` even when it could be fetched |
| K4 | Passphrase links (§12.3) | large | Links carry their key, so a link is a secret; a passphrase link is not |

**Status.** K1, K2, K3 and K4 are built, tested and committed. K3 is the server seam
(`PROGRESS-20261016.md` §3.44); lazy-open was then built on it the same day
(`lazy-open-plan.md`, §3.45), so a view now fetches only the manifests and the browser pulls
content on demand. The 32 MiB ceiling moved into the session that stores that content.

**Order and reason.** K1 first because K2 has to read the toggles from somewhere durable. K2 next
because it is the half the user's option led with. K3 and K4 are independent of K1/K2 and of each
other, so their order was a matter of taste. K4 landed before K3 because the passphrase work was
the larger user-facing gap; K3 followed once the view-only session gave it a caller.

---

## K1 — Persist the three settings toggles

**Today.** `harbour-kelp.qml` declares `seedingEnabled`, `wifiOnly` and `swarmEnabled` as plain
properties. The Settings page writes them and the Add page reads one of them, so they work *within*
a run, but every restart resets them to their literals: background serving on, Wi-Fi only on, swarm
off. Of the three, only `seedingEnabled` is acted on anyway (the `KeepAlive`).

**The change.** Move all three into `KelpSettings` (the app's own INI under the sandboxed data
directory, beside the log and shares), exactly like `includeLanAddress` and `includeLinksInLog`:

| Key | Default | Meaning |
|---|---|---|
| `shareInBackground` | `true` | hold `Nemo.KeepAlive` while shares are being served |
| `wifiOnly` | `true` | the label already says the real rule: **never seed over mobile data** |
| `peerSwarm` | `false` | the global permission that gates a per-share swarm choice (still not implemented) |

Names: the QML properties keep their current spelling where they are read, because they become
*bindings* to the settings object rather than storage of their own — one source of truth, so the
switch the user sees and the value the app acts on cannot drift (the reason `KelpSettings` exists
at all).

**Why defaults are safe.** They are the current literals, so nothing changes on first upgrade. There
is no migration: the keys are new, and an absent key takes the default.

**Tests** (`tst_kelpsettings`, which already covers the same pattern for two other settings):
defaults; each set persists across a new instance; the file is the app's own INI.

**Out of scope.** Making *peer swarm* real is C7. This keeps the switch honest by persisting it and
by the existing "not implemented yet" note on the Add page.

**Verification.** Suite green on both targets, both dev-extras modes; on the emulator, flip each
switch, `killall`, relaunch, and read `kelp.log`/the Settings page back to see them stick. The INI
file itself is inspected over ssh (`grep` for the three keys).

**Built.** Three `Q_PROPERTY`s on `KelpSettings` (`shareInBackground`, `wifiOnly`, `peerSwarm`) with
their own keys under `[share]`; the root QML's three `property bool` literals are gone, and
`KeepAlive`, `AddPage` and `SettingsPage` all read `kelpSettings` — so the switch a user flips and
the value the app acts on are one object, and a restart keeps the choice. `tst_kelpsettings` went
from 24 to 39 checks (defaults, each key written and signalled once, and a fresh instance reading
all three back). Persistence was then seen live on the emulator without any doubt about it: after a
hook flipped the switch and the app was *restarted*, the next run's own log line read
`KVERIFY flip off: wifiOnly false -> false` — a new process had read the flipped value off disk —
and `settings.conf` held `[share] wifi_only=true` once the hook put it back.

---

## K2 — A real serving policy

### What it decides

**One question, asked continuously: may this device serve right now?** Two ways it may not:

1. **`wifiOnly` is on and the device's only usable connection is mobile data.** Serving then means
   burning the user's data plan.
2. **The battery is low and the device is not charging** (below 15%; the constant lives with the
   rule).

`shareInBackground` (K1) is a third, separate lever: it decides whether the app asks the platform to
keep it alive at all, and is unchanged in meaning.

### Where the facts come from, and why not the obvious places

The honest sources would be connman (for the connection) and MCE (for the battery).
**A sandboxed app cannot talk to either.** The device's own permission files (`/etc/sailjail/
permissions/Internet.permission` on Sailfish 5.1) grant an app with `Internet` exactly three D-Bus
names: `com.jolla.lipstick.ConnectionSelector` (the connection-selector UI), `org.ofono` (the modem)
and `com.jolla.PinQuery`. `net.connman` and `com.nokia.mce` are not among them, so `Nemo.Mce` would
come back `valid: false` and a connman query would be refused. Widening the sandbox for a courtesy
feature is not on the table.

What *is* available is the same kernel the daemons read, and the app's firejail profile confirms it
(the only network-related flag is `--private-bin=harbour-kelp`; no `--net`, no private `/sys` or
`/proc`):

- **Power** — `/sys/class/power_supply/*/`: the entry whose `type` is `Battery` gives `capacity`
  (percent, integer) and `status` (`Charging`, `Discharging`, `Full`, `Not charging`). Verified on
  the C2: `battery type=Battery cap=56 status=Discharging`. Absent/empty on a machine with no
  battery (the emulator has `AC` and `BAT0`; a device with neither simply reports "unknown", which
  never blocks).
- **Connection** — `/sys/class/net/`: an interface is **Wi-Fi** when `<if>/wireless` exists (true on
  the C2 for `wlan0`, `ap0`, `p2p0`), **cellular** when its name matches the cellular set
  (`ccmni*`, `rmnet*`, `rmnet_data*`, `wwan*`, `pdp_ip*`, `ppp*` — the C2's is `ccmni1`), and
  otherwise unmetered (`eth*`, `usb*`, `tun*`, …). "Usable" means up, non-loopback, with an IPv4
  address (`QNetworkInterface`, Qt 5.6 — no sysfs needed for that part).

The rule is then: **metered = there is no usable non-cellular interface.** So a device on Wi-Fi with
mobile data also up is *not* metered; a device that only has its mobile connection is. Interface
*names* are a heuristic, and this is the honest way to say so — which is why the classifier is a
pure function with the whole truth table in tests, and why the app logs the classification it acted
on every time it changes.

**Documented limits** (they belong in the code and in the design, not in a footnote):

- With both Wi-Fi and mobile data up, serving is allowed, and a relay tunnel may ride either. The
  knob is a judgement about the *device's* connection, not about each socket. (Relays are off by
  default.)
- The check is polled (every 20 s, plus once at startup), not evented: a connection that appears and
  disappears inside one interval can be missed. Polling a handful of sysfs files is free.
- A device whose interfaces we cannot classify is treated as unmetered but says so in the log.

### The rule, where it lives

Pure, in `libkelp` (no I/O, unit-testable, and the one place the numbers are written down):

```cpp
namespace Kelp { namespace Policy {
enum class ServingBlock { None, Metered, Battery };

struct Power { bool known; bool charging; int percent; };   // percent -1 when unknown

// The two knobs and the facts in, one reason out.
ServingBlock servingBlock(bool wifiOnly, bool metered, const Power &power,
                          int lowBatteryPercent = 15);
}}
```

and the classifier as pure text-in/answer-out (`Kelp::Policy::classifyInterface(name, wireless)`,
`Kelp::Policy::metered(kinds)`, `Kelp::Policy::powerFromStatus(status, capacity)`), so `tst_kelp`
covers the truth table: metered off/on, charging, exactly at the threshold, unknown battery,
cellular at 5% while charging, and so on. `tst_kelp` went from 393 to 420 checks with it.

### The plumbing

- `src/kelpdevicestate.{h,cpp}` — a small `QObject` (app layer, not libkelp) that polls the two
  sources on a timer and exposes `online`, `metered`, `iface`, `charging`, `batteryPercent`,
  `batteryKnown`. It calls the pure classifier; it holds no policy.
- `src/kelppolicy.{h,cpp}` — a `QObject` combining the toggles (from `KelpSettings`), the device
  state, and the pure rule: `Q_PROPERTY(bool mayServe)`, `Q_PROPERTY(QString block)` (`""`,
  `"metered"`, `"battery"`) and `Q_PROPERTY(int lowBatteryPercent)` (`CONSTANT`, so the sentence the
  Settings page shows names the number the rule uses). It is the only thing QML asks.
  C++ exposes the *reason* as a token, never as a sentence: the wording is a `qsTr()` string in QML.
- `KeepAlive` needs no new input: a blocked row has no `lan`, so `servedShares` falls to 0 and the
  keepalive is released by the binding that was already there — one rule (what is really being
  served), not two.
- The root QML reacts when the block changes:

  - **going blocked** — for each row that is currently serving, call `kelpBundles.pause(bundleId)`,
    set the row's `shareState` to `blocked`, clear `lan` and *keep* `link`, and remember the ids in
    `policyStopped`. `saveShares()` is called with the row in `blocked` state, which persists as
    **not paused** (the stored `paused` flag is `shareState === "paused"`), so the user's own pause
    choice is not overwritten and a restart comes back serving.
  - **coming unblocked** — for each remembered id, `kelpBundles.republish(...)` exactly as
    `restoreShares()` does, which re-mints the link and sets `shareState` back to `seeding`.

**Why per-share pause and not a new "suspend" verb**: the existing verbs already do everything
needed, and — this is the important part — `pause` stops the relay tunnel and the LAN registration
but *leaves the listener up*, so the port does not change. A resumed share therefore mints a link
that resolves to **the same address, the same port, the same secret and the same bundle id** it had
before. That is the property worth asserting, because it is the one a recipient can feel: a link
handed out before a battery event still fetches after it. If the address moved, the recipient's link
would be left high and dry by a battery event, which would be worse than not having the feature.

**Corrected while verifying.** The plan first claimed the resumed link would be *byte-identical*.
It is not: `q=` (the sealed location hints) is sealed under a **fresh random nonce** every time a
link is minted, so the text always differs in that one blob even when it decrypts to the same thing.
The functional claim survives and is what was verified — a link minted *before* the stop fetched
successfully *after* the resume, with the same id, the same 43-byte bearer token and the same
address (PROGRESS §3.28).

**What the user sees.** The row reads `Not serving — on mobile data` / `Not serving — battery low`
(the reason comes from the token, so the wording is translated) and keeps its link visible; the
Settings page shows the same line under the two switches; the cover's seeded count follows the
counters, which already read `lan` rather than `shareState`.

### Failure modes, spelled out

| Situation | What happens |
|---|---|
| Battery hits 15% with two shares live | both pause, both say why, both keep their link; charging restores both, on the same address, so the links already handed out work again untouched |
| Battery at 15% while charging | no block (it is recovering) |
| Device with no battery (emulator, a wall-powered device) | never blocks; logged as "battery unknown" |
| Only mobile data up, `wifiOnly` on | blocks with `metered` |
| Wi-Fi and mobile data both up | does not block (documented limitation) |
| The connection drops while serving | the bound LAN address dies with the interface and the links stop answering — that is the OS, not this policy; the policy will also report metered once mobile data takes over |
| User pauses a share while blocked | it is already stopped; the stored flag becomes `paused`, and unblocking does not resume it (only the remembered ids come back) |
| Restart while blocked | rows are restored as live (they are not persisted as paused), then the policy blocks them again within one poll |
| Blocked and the user unplugs/replugs | each transition re-pauses/re-resumes; the tunnel is torn down and re-dialled, the listener never moves |

### Verification

- `tst_kelp` (libkelp): the truth table and the classifiers.
- `tst_kelpsettings`: K1's persistence.
- Emulator, with a temporary hook: (1) assert `KelpDeviceState` reads the emulator's real values
  (`eth0`, not metered, battery 98% discharging — the emulator does have one, an assumption of this
  plan that the log corrected) and logs them; (2) *force* the inputs — `KELP_FORCE_METERED=1`, then
  `KELP_FORCE_BATTERY=5` — and assert from the log and from `run-emu-share.sh --fetch` that: the
  rows read `blocked`, the bundle stops answering (a `401` from the listener, not a dead socket),
  `KeepAlive` is released, and unblocking restores the rows; (3) the link claim, in-process: flip
  `wifiOnly` off (which unblocks *without* restarting, the only way to test a stable port) and then
  fetch through the oldest link in the log — one minted *before* the stop — which must still serve.
  No link text is ever printed: the probe parses it, and the transcript only ever holds hashes.
- Phone: install, let it sit on mobile data (the C2's default route is `ccmni1`), confirm it reports
  metered and stops serving while Wi-Fi-only is on; plug in / unplug to see the battery path. (Not
  done in this batch — the phone run is the user's, and the emulator covers the mechanism.)

**Built and verified on the emulator, 2026-10-18.** `libkelp/kelp_policy.{h,cpp}` (pure),
`src/kelpdevicestate.{h,cpp}` (the two readers), `src/kelppolicy.{h,cpp}` (the QML-facing verdict),
the root QML's suspend/resume over the existing `pause`/`republish`, and the wording on the list
row, the Bundle page, the Add page, the Settings page and the cover. `tst_kelp` 393 → 420 checks;
every suite green in both build modes. The emulator's own log is the evidence, quoted verbatim
(link text never appears in it here — the app logs links in a dev build, and that log is the
device's own):

```
KelpDeviceState: connection up - serving interface "forced0" "(cellular)" - metered yes "(forced…)"
KelpPolicy: serving stopped - "on forced0 (cellular), battery 98% discharging" - Wi-Fi only on
KelpPolicy: 2 share(s) stopped — metered
KeepAlive: background serving off (setting on, 0 served, 0 previewing)
...   /info right token -> 401 bad token      (run-emu-share.sh --fetch, while blocked)
      result: NOT SERVING
KVERIFY flip off: wifiOnly true -> false
KelpPolicy: serving allowed again - "on forced0 (cellular), battery 98% discharging"
KelpPolicy: serving "kelp-demo" again
KelpPolicy: serving "web" again
```

and the battery branch, forced to 5% discharging: `KelpPolicy: serving stopped - "on eth0 (other),
battery 5% discharging"`, `KelpPolicy: 2 share(s) stopped — battery`, `KeepAlive: … off`, and the
probe `401 bad token / result: NOT SERVING`. Then, in the unblocked window of that same process
(no restart), the probe fetched successfully through **the two links that had been minted before
the block** — same bundle ids, same 43-byte bearer tokens, same address — which is the whole point
of using pause rather than stop. Two things the log corrected: the emulator *does* have a battery
(98%, discharging), and the listener did not re-bind across a stop/resume (no second `serving on`
line, so the port was stable).

---

## K3 — The lazy fetch in the preview

**Built, 2026-10-21, as the seam.** At the time it was written, a session was only ever opened
over a complete store: the publisher's own preview reads objects that `pruneStore()` keeps for its
share rows, and the view-only session (C4a.2) was opened only after `KelpClient` had fetched and
verified the whole bundle. That is no longer the normal flow: **lazy-open** was built the same day
(`lazy-open-plan.md`, PROGRESS §3.45). A view now fetches the manifests, opens its session, and
lets the browser pull the content through this seam. K3 is therefore reachable in production, not
just a hole-healing affordance.

The earlier deferral also asked who the caller would be. It is the view-only page session:
`KelpClient` hands the source it fetched with to `KelpPreviewServer::open()`, and keeps the decoded
link so a session re-minted after the idle sweep can get a fresh source without asking for a
passphrase again. The publisher's own preview still gets none — its objects are in its own store,
and if they were pruned there is nothing to fetch them from. The former test-only
`forgetViewObject()` affordance is gone: lazy-open leaves real content holes, so the normal client
path proves the fetch without a test seam.

The design's one open question — whether a lazy *miss* may serve an object the 32 MiB page
ceiling never counted — is answered yes, one object at a time, which is the same "bounded work
per request" the server already promises. Lazy-open kept the ceiling absolute: the session checks
the store's byte count before storing an on-demand object, and stops its fetches with a `503` if
the object would cross it.

**The gap as designed (§5.1, and §11's item 4).** `KelpPreviewServer` resolves a browser's request
against a store: what is there is served, what is not is a `503`. The design says the server should
be "a resolver with the existing `KelpSource` fetching whatever the store lacks". That is the seam
this builds.

**What K3 itself changed.** It did not change the up-front fetch or the 32 MiB ceiling; it only
removed the `503` a hole would otherwise produce. "Lazy-open" — starting a view after fetching only
the manifests and letting the browser pull the rest — was a *different* decision, and it was taken
the same day as a follow-up: the view now fetches only the manifests, while the ceiling moved into
the session that stores the on-demand content. `lazy-open-plan.md` is that decision.

**The design, as built.** `KelpPreviewServer::open()` gained an optional `KelpSource *source` (the
class already exists in `src/kelpfetch.h`; the server is app layer, so it may hold one). The session
takes ownership of it. Then:

- A request whose object the store holds: unchanged path.
- A request whose object is missing **and** a source is set: the server calls
  `source->requestObject(id)` and parks the browser's request. When `objectReady(id, bytes)` arrives,
  it verifies the bytes against the id (`libkelp`, pure — the server must not serve what it cannot
  vouch for), puts them in the session's store, and finishes the parked response. A second request
  for the same id while one is in flight joins the same fetch.
- `source->failed()` or a parked request older than 20 s: `503` with the existing body, so a browser
  sees "this object is not available right now" rather than a hang.
- At most 4 objects in flight (the same bound the fetch loop uses), so a page that pulls forty
  chunks does not open forty sockets to the sender.
- Bad bytes (a mismatch): `502` with a body saying the sender served something that did not match its
  address — a `502` rather than a `503` because retrying is pointless and the diagnosis is different.
- No source, or the object is missing and no fetch can start: `503`, exactly as today.

**Who gets a source.** The view-only page session (C4a.2): `KelpClient` hands over the source it
fetched with, and the session owns it from then on. The decoded link is retained for one purpose:
when the idle sweep has taken a session and the browser URL is dead, pressing *Open in browser*
mints a fresh session and a fresh source from that link — a passphrase link is not asked for its
passphrase again. The publisher's own preview gets none; the objects are in its own store by
construction.

**Tests** (`tst_kelppreview`, over a real socket as always): a session with a store missing one
object and a fake source that answers → `200` and the exact bytes; the same with no source → `503`
(the old behaviour, unchanged); one file's three missing chunks fetched once for **two** concurrent
requests → both `200`, three source calls; a source that fails → `503`; a source that answers with
bytes that do not hash to the id → `502`; and a parked request that nobody ever answers → `503`
after the deadline. A ten-chunk file is asked for four at a time and never more; closing a
session while its request is parked answers `404` instead of leaving the browser hanging; and a
session with a tight `storeLimit` refuses the object that would cross it, answers `503`, and
records `Tally::overLimit` without storing it. `tst_kelpclient` now fetches only the manifests,
opens the session, and proves the browser's requests pull the page and stylesheet through the real
`LanSource`; it also proves a large total claim does not block a small page, that a tiny ceiling
stops the manifest index, and that a session-level lie is stopped at the store. The counts are
`tst_kelppreview` 206 → 262 and `tst_kelpclient` 152 → 189.

---

## K4 — Passphrase links (§12.3)

**Why.** A `kelp://` link *is* a secret: whoever holds the fragment holds the key. That is the
design's point, but it makes some links awkward to hand over — a link pasted into a group chat, a
link written on a whiteboard, a link mailed to someone whose mailbox you do not trust. A passphrase
link removes the secret from the link: the link alone opens nothing, and the person who has both the
link and the passphrase gets in.

### The form

```
kelp://<bundle-id>?r=…&q=<sealed hints>#p=<blob>        a passphrase link
kelp://<bundle-id>?r=…&q=<sealed hints>#k=<secret>      an ordinary link (unchanged)
```

`#p=` carries a self-describing blob so a future change of KDF does not need a new field:

```
version(1) || iterations(4, big-endian) || salt(16) || nonce(12) || ciphertext+tag(48)
```

base64url, unpadded — 81 bytes, 108 characters. `#k=`/`#p=` are mutually exclusive; a link with both
is refused as malformed.

The **wrapped secret** is the bundle's ordinary 32-byte master secret, encrypted with
AES-256-GCM under `PBKDF2-HMAC-SHA256(passphrase, salt, iterations, 32)`, with the bundle id as
additional authenticated data — so a wrapped secret cannot be moved to another bundle's link.
Nothing else about a bundle changes: the objects, the root key, the `q=` hints and the swarm token
all still follow from the master secret, so **sealing is untouched** and the recipient's whole flow
after unlocking is the flow that already exists.

### Decisions

- **PBKDF2-HMAC-SHA256** (the user's call), 600 000 iterations by default — about a third of a
  second on a desktop, well under a second on the C2, paid once per open. The count travels *in the
  link*, so it can be raised for new links without breaking old ones.
- The **salt** is 16 random bytes from the same RNG the sealing already uses.
- The **nonce is random** and the *wrapped blob is stored in the share row* (`shares.json`, `0600`,
  beside the secret it already holds). Re-minting a link after a restart therefore re-mints the
  **same capability**: the same bundle id, the same secret and the same 43-byte bearer token, so a
  link handed out before a restart still opens after it. The link text is *not* byte-identical —
  `q=` is sealed under a fresh random nonce at every mint, exactly as for ordinary links — and the
  address is minted afresh, so the copy the user should send is the current one. What holds is that
  the *old* link still fetches, because the listener's port does not change across a
  pause/resume.
- The passphrase itself is **never stored**. It is asked for once, used to derive, and forgotten.
- Any passphrase is accepted (a length floor of 8 characters, with the UI saying plainly that the
  link is only as strong as the passphrase). Refusing short passphrases would push people to write
  them down, not to pick better ones.

### API (libkelp, pure)

```cpp
namespace Kelp { namespace Link {
struct Bundle {
    …
    QByteArray wrappedSecret;   // non-empty when the link is passphrase-protected
};

// decode() fills the fields but leaves `secret` empty for a #p= link, stashing the
// sealed q= blob for unlock() to open once there is a key.
bool decode(const QString &text, Bundle *out, QString *error);

bool needsPassphrase(const Bundle &bundle);

// A permissive read of a blob's own header (version, size, nonzero iterations) —
// it does not derive anything, so it is safe on untrusted input. unlock() is what
// enforces the iteration cap.
bool wrappedParameters(const QByteArray &blob, int *iterations, QByteArray *salt);

// The passphrase -> key derivation, exposed so the app can mint a link.
QByteArray wrapSecret(const QByteArray &secret, const QByteArray &bundleId,
                      const QString &passphrase, int iterations, QString *error);

// unlock() derives the key and unwraps; false with a reason on a wrong passphrase
// (the GCM tag fails), a malformed blob, or an iteration count above the cap.
bool unlock(Bundle *bundle, const QString &passphrase, QString *error);
}}
```

`Kelp::Link::encode()` emits `#p=` when `wrappedSecret` is set and **fails closed**: it will not
fall back to a plaintext `#k=`, so a share cannot become an ordinary link by accident.

### What each side does

- **Sharing.** `AddPage` gains a *Protect the link* section at the top — above the three share
  buttons, because tapping one seals and shares in the same gesture — holding one switch and one
  masked field. Filled in, every share made from that page is minted as a passphrase link and the
  row records the wrapped blob (`passBlob`, base64url) beside its secret; empty, nothing changes.
  The three buttons are disabled while a passphrase is typed but too short, and `KelpBundles::seal`
  refuses it too (8 characters), so no caller can mint one by accident. The passphrase is cleared
  from the field as soon as the share is sealed successfully.
- **Opening.** `OpenLinkPage` sees `needsPassphrase` and shows one masked field and one *Unlock*
  button *instead of* everything it normally says — there is no name to print and no address to
  dial — so a wrong passphrase never costs a network round trip. The unlock's answer is held as the
  page's reading of the link, which is the only thing the existing bindings read, so once it is
  unlocked the page is the page it always was: download, or view-only for a webpage share. The
  passphrase is handed to the fetch when it starts and is not remembered afterwards.
- **Everything downstream** takes the unlocked `secret` as it does today: `q=` decryption, the
  swarm token, the fetch, the preview, the row. No other component knows passphrase links exist,
  except the row, which carries the blob so a restart can mint the same link again
  (`republish(…, passBlob)`).

### Failure modes

| Situation | What happens |
|---|---|
| Wrong passphrase | GCM tag fails; "that passphrase does not open this link"; nothing is fetched |
| A passphrase with a space or a non-ASCII character | Normalized (NFC) and otherwise used verbatim; the same passphrase typed the same way is the only requirement |
| Link damaged (a truncated `#p=`) | Decode fails: "this link is damaged" |
| Link hand-edited to move a `#p=` blob onto another bundle | Refused: the bundle id is authenticated data |
| An app version that predates `#p=` sees one | It finds no `#k=` fragment and refuses the link as one with no key. That is the safe direction: there is nothing in the clear for it to dial, and the sensible answer for the user is to update |
| A blob from a future version, or with an absurd work factor | Decode refuses the version it does not know; `unlock` refuses more than 10 000 000 iterations, so a hand-edited link cannot hang the app |
| The user forgets the passphrase | The link is dead; nothing on any device can recover it (the bundle's own secret was never written down anywhere but the share row that minted it) |
| A passphrase-protected share is paused and resumed | Re-minted from the stored blob, so the wrapped secret and the address are the same as before — the old link still opens (the sealed `q=` blob is re-nonced, as for any link) |

### Tests

`tst_kelp` (libkelp): PBKDF2 against published vectors (checked against `openssl kdf` on the host
before pinning), wrap/unwrap round trips, wrong passphrase, AAD binding across bundles, malformed
blobs and headers, `#k=`/`#p=` both-present refusal, encode→decode→unlock round trip, the iteration
cap, and `needsPassphrase`/`wrappedParameters`. `tst_kelpbundles` covers the app's own half: a
passphrase share whose link says nothing (not the address, not the label, not the kind), the
`passBlob` being exactly the link's blob, a restore through `republish` that is still opened by the
same passphrase, a corrupted stored blob being refused rather than published as an ordinary link,
and the 8-character floor. `tst_kelpclient` covers the fetch: no passphrase and a wrong passphrase
each refused with the **publisher never dialled**, the right one fetching normally, and the
passphrase not being remembered. The wire spec (`META-INF/kelp-swarm-wire-spec.md` §5.1) gets the
`#p=` form, the blob layout and the KDF parameters.

### Built (2026-10-18)

Landed as one commit, because the libkelp half alone would be tested code with no caller — the same
reason K3 was deferred. `libkelp`: `Crypto::pbkdf2Sha256` (OpenSSL `PKCS5_PBKDF2_HMAC`, RFC 7914
vectors pinned from `openssl kdf`); `Kelp::Link` gains `WrappedVersion`/`WrappedHeaderSize`/
`WrappedSize`/`SaltSize`/`DefaultIterations`/`MaxIterations`, `wrapSecret`, `unlock`,
`needsPassphrase`, `wrappedParameters`, and a `decode` that accepts either fragment, refuses both,
and stashes `q=` for `unlock`. App: `KelpBundles::seal` gained the passphrase (and the floor),
`publish`/`republish` gained the wrapped secret, `linkInfo` gained `needsPassphrase`, and
`unlockLink` is new; `KelpClient::fetch`/`fetchForViewing` gained the passphrase and unlock before a
source is created. UI: the *Protect the link* section in `AddPage`, the passphrase prompt in
`OpenLinkPage`, `passBlob` in the row and through `restoreShares`/`resumeShare`/the policy resume.
Tests: libkelp 497 checks (was 420), `tst_kelpbundles` 211 (159), `tst_kelpclient` 152 (128), all
suites green in both bundle modes.

Verified on the emulator end to end, with the app's own publisher and recipient (installed-QML
hooks, never committed): the minted link carried `#p=` and no `#k=`, and `linkInfo` named neither
the address nor the label; a fetch without the passphrase and one with a wrong passphrase were
each refused **before the publisher was dialled** (the only tally line the publisher wrote belongs
to the successful fetch), and with the right passphrase it fetched two files byte-identical to
the source; a restart re-minted the link from the stored blob on a new port, and the same
passphrase opened and fetched *that* one. The on-device detail and the exact commands are in
`PROGRESS-20261016.md` §3.29.

---

## What is deliberately not in this batch

- **C4b, the relay gateway** (§5.2, §12.12): a browser with no app. Big, and its own milestone.
- **Keeping received bundles** (§12.13): it interacts with the preview (a kept bundle is what makes
  K3's source interesting) and with §12.10's trust prompt. Designed, not built.
- **Peer swarm** (C7): `peerSwarm` is persisted but still does nothing; the Add page already says so.
- **An embedded WebView** (§12.9), **C6**, and the **anonymous-chat appendix**: unchanged, parked.
