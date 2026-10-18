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

**Status.** K1 and K2 are built, tested and verified on the emulator (see each section's
"Built" note). **K3 has moved to the back of the queue, deliberately** — see its section: the
situation it fixes cannot arise yet, because nothing opens a session over a store that can be
incomplete. K4 is next.

**Order and reason.** K1 first because K2 has to read the toggles from somewhere durable. K2 next
because it is the half the user's option led with. K3 and K4 are independent of K1/K2 and of each
other, so their order is a matter of taste — K3 first because it is smaller and it is a real hole in
something already shipped.

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

**Deferred, 2026-10-18, on purpose — and here is the honest reason.** The seam is designed
below, but nothing in the app can reach the situation it fixes. A preview session is only ever
opened over a store that is complete:

- the publisher's own preview reads the app's object store, and `pruneStore()` will not delete
  an object that belongs to any share row (paused rows included);
- the view-only session (C4a.2) is opened only after `KelpClient` has fetched the whole bundle
  *and verified every object*, and a fetch that loses an object ends in `Failed`, which offers
  "Try again" rather than a page with a hole in it.

So a `KelpSource`-backed session would be a path with tests and no caller — worth writing the
day a session can be opened over an incomplete store, which is exactly what C4a.2's lazy-open
(skip the up-front fetch, pull objects as the browser asks) or §12.13's kept bundles would make
it. Building it before then would mean shipping code no user can reach, which is the kind of
thing this project writes down instead of doing.

The design stands as written for whoever picks it up. Its one open question is whether a lazy
*miss* may serve an object the 32 MiB page ceiling never counted — the answer above is yes, one
object at a time, which is the same "bounded work per request" the server already promises.

**The gap as designed (§5.1, and §11's item 4).** `KelpPreviewServer` resolves a browser's request
against a store: what is there is served, what is not is a `503`. The design says the server should
be "a resolver with the existing `KelpSource` fetching whatever the store lacks". That is the seam
this builds.

**What it is not.** It does not change the up-front fetch or the 32 MiB ceiling of a view-only
webpage link (C4a.2's D-decisions stand). "Lazy-open" — starting a view after fetching only the root
manifest and letting the browser pull the rest — is a *different* decision, deliberately not taken:
it trades a predictable bounded transfer for a first paint. The seam built here is what makes that
decision cheap to take later.

**The design.** `KelpPreviewServer::open()` gains an optional `KelpSource *source` (the class
already exists in `src/kelpfetch.h`; the server is app layer, so it may hold one). The session takes
ownership of it. Then:

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

**Who gets a source.** The view-only page session (C4a.2): `KelpClient` creates a source for the
link it is handling and hands it over, so a hole left by a partial fetch heals when the browser asks
for it. The publisher's own preview gets none — the objects are in its own store by construction, and
if they were pruned there is nothing to fetch them from.

**Tests** (`tst_kelppreview`, over a real socket as always): a session with a store that is missing
one object and a fake source that answers → the browser gets `200` and the exact bytes; the same
with no source → `503` (today's behaviour, unchanged); a source that fails → `503`; a source that
answers with bytes that do not hash to the id → `502`; two requests for the same missing object → one
fetch; and a parked request that nobody ever answers → `503` after the timeout.

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
  beside the secret it already holds). Re-minting a link after a restart therefore reproduces the
  **same** link text, rather than a fresh wrapping of the same secret: a link that changed on every
  restart would be worse than useless.
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
    int iterations = 0;
    QByteArray salt;
};

// decode() fills the fields but leaves `secret` empty for a #p= link.
bool decode(const QString &text, Bundle *out, QString *error);

// unlock() derives the key and unwraps; false with a reason on a wrong passphrase
// (the GCM tag fails) or a malformed blob.
bool unlock(Bundle *bundle, const QString &passphrase, QString *error);

bool needsPassphrase(const Bundle &bundle);
}}
```

`Kelp::Link::encode()` emits `#p=` when `wrappedSecret` is set, and takes the passphrase that
produced it so the UI can mint both halves at once.

### What each side does

- **Sharing.** `AddPage`'s webpage/folder/file flow gains one optional field on the *Share…* sheet:
  *Protect the link with a passphrase*. Filled in, the sealed bundle's link is minted as a
  passphrase link and the row records the wrapped blob, iterations and salt. Empty, nothing changes.
- **Opening.** `OpenLinkPage` sees `needsPassphrase` and shows one masked field and one button
  before it asks the sender for anything (so a wrong passphrase never costs a network round trip).
  Wrong passphrase → "that passphrase does not open this link". Once unlocked, the page is the page
  it always was — download, or view-only for a webpage share.
- **Everything downstream** takes the unlocked `secret` as it does today: `q=` decryption, the
  swarm token, the fetch, the preview, the row. No other component knows passphrase links exist.

### Failure modes

| Situation | What happens |
|---|---|
| Wrong passphrase | GCM tag fails; "that passphrase does not open this link"; nothing is fetched |
| A passphrase with a space or a non-ASCII character | Normalized (NFC) and otherwise used verbatim; the same passphrase typed the same way is the only requirement |
| Link damaged (a truncated `#p=`) | Decode fails: "this link is damaged" |
| Link hand-edited to move a `#p=` blob onto another bundle | Refused: the bundle id is authenticated data |
| An app version that predates `#p=` sees one | "This link needs a newer Kelp" — decode already refuses unknown fragments with a reason, and this is the same path |
| The user forgets the passphrase | The link is dead; nothing on any device can recover it (the bundle's own secret was never written down anywhere but the share row that minted it) |
| A passphrase-protected share is paused and resumed | Re-minted from the stored blob, so the wrapped secret and the address are the same as before — the old link still opens (the sealed `q=` blob is re-nonced, as for any link) |

### Tests

`tst_kelp` (libkelp): PBKDF2 against a published vector (checked against `openssl kdf` on the host
before pinning), wrap/unwrap round trips, wrong passphrase, AAD binding across bundles, malformed
blobs, `#k=`/`#p=` both-present refusal, encode→decode→unlock round trip, and
`needsPassphrase`. `tst_kelpclient` gains the unlocked-secret path. On the emulator, end to end:
share with a passphrase, fetch the link with the passphrase on a second store, then try without it
and watch it refuse. The wiring spec (`META-INF/kelp-wire-spec.md`) gets the `#p=` form, the blob
layout and the KDF parameters.

---

## What is deliberately not in this batch

- **C4b, the relay gateway** (§5.2, §12.12): a browser with no app. Big, and its own milestone.
- **Keeping received bundles** (§12.13): it interacts with the preview (a kept bundle is what makes
  K3's source interesting) and with §12.10's trust prompt. Designed, not built.
- **Peer swarm** (C7): `peerSwarm` is persisted but still does nothing; the Add page already says so.
- **An embedded WebView** (§12.9), **C6**, and the **anonymous-chat appendix**: unchanged, parked.
