# Tapping a `kelp://` link — scheme activation

A `kelp://` link that arrives in another app (chat, mail, notes) should reach Kelp and appear on
the **Open a link** page. Before this work it did not: the app already accepted a link as its first
argument (or in `KELP_INITIAL_LINK`), but nothing told the system that Kelp was the handler for the
scheme, and the app could only run one instance. **Built and device-tested** — the evidence is
under *Tests* at the end.

This is deliberately *not* "typing `kelp://` into a browser": browsers do not speak the scheme,
and the browser's door stays the `https://<relay>/b/<id>#k=<secret>` gateway URL (C4b). What this
is for is the tap in a chat: the system hands the URL to its registered handler, and the handler is
Kelp.

## What the user sees

1. A `kelp://…` link is tapped in another app. If Kelp is not running it starts and comes
   forward with **Open a link** showing that link, exactly as if it had been pasted. If it is
   already running, the link is delivered to it and the page is waiting there — the platform
   does not let a background app raise itself (see *Raising the window*).
2. Nothing is fetched or opened by itself. The page's **Open** / **Save** button is still the only
   thing that starts anything.
3. A running Kelp keeps serving while the link arrives; nothing is restarted and no share is
   disturbed.
4. If the page is already open, a second tap replaces the link on it rather than stacking pages.

## The plumbing

**Registration.** `harbour-kelp.desktop` gains the scheme and passes the URL through the launcher:

```
Exec=/usr/bin/invoker --type=silica-qt5 -- /usr/bin/harbour-kelp %u
MimeType=x-scheme-handler/kelp;
```

`invoker` is not optional: it is what applies the Sailjail sandbox (`-I` is guessed from the binary
name), so a scheme launch runs in exactly the sandbox a launcher launch does. `%u` is the single
URL. No `-s`: `invoker --single-instance` drops the new URL, which is the one thing this feature
needs (and on this platform it does not raise a PIE app either), so a scheme activation always
starts a short-lived second process instead. `update-desktop-database` has to run after install
(`%post` and `%postun`), or GIO will not find the handler.

**Handoff.** The second process is a forwarder, not an app:

```
argv[1]  ->  validate  ->  connect to the running instance  ->  send the link  ->  wait for "ok"
                          (a private unix socket in the app's data dir)        ->  exit 0
```

If nothing answers, the process becomes the instance: it binds the socket, then builds the UI. The
socket is `QLocalSocket`/`QLocalServer` (QtNetwork, already linked), path
`<data>/link.sock`, `0600`, and carries one line: the link.

The protocol is deliberately tiny: one line, `ok` or `no`, then the connection closes. No request
ids, no replies, no files.

**UI.** `OpenLinkPage` is already driven by its `link` property (`onLinkChanged` resets the
passphrase state), so a running instance pushes it with `{ link: link }`, or assigns `.link` when it
is the current page. The startup argument path is the same function, so there is one way in.

**Raising the window.** This was tested on the device rather than assumed, and the answer
is *no*: when Kelp is already running in the background, the compositor does not bring it
forward. `QQuickView::requestActivate()` is ignored (Qt 5.6 on Sailfish has no activation
protocol a background client may use), `invoker --single-instance` does not raise a PIE
app, and Lipstick's own `WindowModel.launchProcess` answers "not in privileged group" to
an app. A notification is not a way around it either: Lipstick's notification UI opens
the app only when the notification carries a "default" action, and such an action would
still have to raise the window itself.

So the link is delivered and the page is waiting; the user brings Kelp forward the
ordinary way (tapping its icon raises it, which is Sailfish's own single-instance
behaviour). The cold case — Kelp not running — does come forward, because a new window is
mapped. This is reported as the platform behaviour it is, not glossed over.

## Security

This feature turns "any process that can run Kelp" into "any process that can make Kelp show a
page". The rules that keep that from being a hole:

**1. Activation is not consent to fetch.** The link is only *prefilled*. Nothing dials a relay, no
page is opened, no passphrase is tried, no file is written, until the user presses the page's own
button. Otherwise a link activation would be a way to make this device contact an attacker's relay
(an IP disclosure), start a large download, or probe which bundle ids exist. The existing
`OpenLinkPage` is the consent step, and it is unchanged.

**2. The key is in `argv`, and that is stated, not hidden.** Desktop-file activation passes the URL
as a command-line argument, so for the lifetime of the forwarder process the whole link — fragment
included — is readable in `/proc/<pid>/cmdline` by same-UID processes, and in `ps`. A plain
desktop-file handler has no way around that. D-Bus activation would keep the URL out of `argv`,
but it adds a bus name another local process can squat and a service interface the app does not
otherwise have; this design stays with the launcher and states the cost. What it does about it:

- the forwarder is as short-lived as the design allows: validate, send, wait for the ack, exit;
- the link is never logged, never put in an error message, never echoed in a toast or a window
  title, and never written to disk;
- the instance's log line says only *that* a link arrived from another app.

The residual exposure is real and is the cost of the feature: a link tapped from a chat is already
visible to that chat app, and now briefly to anything that can read this user's `/proc`. On a
Sailfish device the sandbox narrows "anything", but does not make it nothing. That is the trade,
and it is worth it only because the alternative is the recipient copying a 700-character URL by
hand.

**3. The socket is a capability, so it is fenced.** It lives in the app's own data directory, mode
`0600`, and accepts exactly one thing: a `kelp://` link, 1–4096 bytes, with no control characters
and no newline. Anything else is refused with `no` and the connection closed; the refusal logs the
fact, never the bytes. Under Sailjail another app cannot see into that directory at all, so the
set of processes that can reach the socket is the set that can already read `shares.json` — which
is every secret Kelp holds. No new privilege is created.

**4. Nothing is written.** The link exists in two places only: the forwarder's memory and the
instance's memory (until the page is dismissed). A mailbox *file* was rejected for exactly this
reason: a key-bearing link left in the data directory is a secret at rest that nobody asked for.

**5. One instance, always.** Two instances would each load `shares.json` and each write it back —
the file that holds every share's secret and the passphrase blob. The socket is what enforces the
single instance, and the takeover rule is deliberately conservative:

| `connect` result | Meaning | What happens |
|---|---|---|
| connects, `ok` | a live instance took it | forwarder exits 0 |
| `ServerNotFoundError` / `ConnectionRefusedError` | no listener (stale file after a crash) | remove the stale socket and become the instance |
| timeout | something is there but did not answer | **do not** take over; log and exit 1 |

A hung instance is therefore not silently duplicated — it is left alone, which is the failure mode
that cannot corrupt the share file.

**6. The scheme can be squatted.** Another app could declare `x-scheme-handler/kelp` and be handed
the next link, key and all. Nothing in Kelp can prevent that; it is a property of the platform's
scheme registry. It is written down here so it is not discovered later. (Harbour would reject such
an app; the OS does not.)

**7. A browser still cannot open `kelp://`.** Registration changes nothing there, and it must not:
the browser has no key derivation for this scheme. The two doors stay separate — `kelp://` for the
app, `https://<relay>/b/…` for the browser — and the app never registers `http`/`https`.

**8. The passphrase case is unchanged.** A `#p=` link prefills like any other; the page asks for
the passphrase, and the fragment is the wrapped key either way. The handoff adds no path that
bypasses `unlockLink`.

## Failure modes

| Situation | What happens |
|---|---|
| No handler registered (e.g. an old build) | The system says it cannot open the link; nothing happens |
| App not running | The launch becomes the instance and shows the page |
| App running in the background | The link is handed over; the window is asked to come forward (the compositor ignores it — see above); the page waits regardless |
| App running, socket not yet bound (mid-startup) | The forwarder retries once after `removeServer`; if it loses the race it forwards to the winner and exits |
| A flood of activations | Each replaces the page's `link`; nothing is queued and nothing accumulates |
| Garbage on the socket | `no`, connection closed, one log line, no content |
| Stale socket after a crash | Removed (the connect error says there is no listener) and the app becomes the instance |
| Hung instance | Not taken over — exit 1, logged |

## Tests

**Unit (`tst_kelphandoff`, new suite).** `linkFromArguments` accepts a clean link and
case-insensitive `KELP://`, picks it out of other arguments, and refuses `http(s)://`, an empty
argument, a link over the cap, and one containing a newline or a control character. Over a real
socket: a round trip delivers exactly the text that was sent and answers `ok`; `https://…` answers
`no` and emits nothing; a 5000-byte line is refused; a second listener on the same path fails
(which is the single-instance guarantee), and it fails even when it asks to replace a stale socket
— the path is checked again at bind time, so a live instance that started in between is not
displaced; the socket is `0600`; a plain file left at the path is recognised as stale and replaced.

**Device.** With the scheme installed: `gio open kelp://…` with Kelp stopped (it starts,
the page is shown, and the new window comes to the front), with Kelp running and in the
background (the page receives the link on the running instance, exactly one process
remains, and the compositor leaves Kelp where it was — see above), and from the app
grid (the icon still starts Kelp, sandboxed once through `invoker` → firejail, with the
new `Exec`; and tapping the icon while Kelp runs raises the same window — same PID — so the
launcher's single-instance behaviour is untouched). `gio mime x-scheme-handler/kelp` names
the handler; the socket is `0600`; a
socket left behind by a killed instance is replaced by the next launch; two links in a
row do not grow the stack (`pageStack.depth` stays at 2 — the bundle list and the one page — and
that page's link is replaced). Then a
screenshot of the page, and no real secret typed anywhere: the test links are synthetic
(`kelp://aaaa…`), and the fragment rule applies as always.

## Out of scope

- Auto-opening or auto-fetching a link (rule 1 — this is the security boundary, not an omission).
- A D-Bus service, or `invoker -s` (drops the URL).
- Handling any scheme other than `kelp` (including `http`/`https`).
- The browser: `kelp://` stays unopenable there.
- Keeping a history of received links.

## Size

| Piece | Lines |
|---|---|
| `src/kelphandoff.{h,cpp}` | 299 |
| `src/harbour-kelp.cpp` glue | 82 |
| QML `openLink()` and the page handoff | 25 |
| desktop file + spec scriptlets | 11 |
| `tst_kelphandoff` + runner | 291 |
| docs (`scheme-plan.md`, design §2.1, progress) | ~140 |

The tests and the device work are most of it.
