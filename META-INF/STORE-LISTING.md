# Kelp 0.1.0 — Harbour / Jolla Store submission

Prepared 2026-10-27. Package `harbour-kelp`, release `0.1.0-1`, target
Sailfish OS 5.1.0.11.

## 1. Identity (dashboard fields)

| Field | Value |
|---|---|
| Title | **Kelp** |
| Package | `harbour-kelp` |
| Version | `0.1.0` |
| Release | `1` |
| Category | **Utilities** (matches the developer's previous Harbour app) |
| License | `GPL-3.0-only` |
| Source | <https://github.com/kapitaali/harbour-kelp> |
| Summary | Encrypted file sharing over your local network. |
| Website / support | <https://github.com/kapitaali/harbour-kelp> |
| Privacy policy | `PRIVACY.md` in the repo, <https://github.com/kapitaali/harbour-kelp/blob/main/PRIVACY.md> |

Kelp has no account system, no ads, no in-app purchases and no telemetry. It
shares on the local network first; a relay is used only as a fallback when a
recipient cannot reach your device directly.

## 2. Upload artifacts

Upload **both** architecture builds:

| Architecture | File | Size | SHA-256 |
|---|---|---|---|
| aarch64 | `/home/theb/Downloads/harbour-kelp-0.1.0-1.aarch64.rpm` | 331,171 B | `9d9cdfdc2bb74b607991569a7a45effd6da61729efe6d48cf54cff8845e9dce6` |
| armv7hl | `/home/theb/Downloads/harbour-kelp-0.1.0-1.armv7hl.rpm` | 302,839 B | `8bc2707f919aee0a84f7a1108f08941af6f95aadd52198f37bb3f17d57f78d76` |

The i486 emulator RPM (`RPMS/SailfishOS-5.1.0.11-i486/`) is for local testing
only and is **not** uploaded. No GPG signature is needed; Harbour accepts an
unsigned upload and signs on publish.

Both RPMs were built with `%define kelp_dev_extras 0`, so release builds do not
write `kelp://` links or keys to the log and do not offer the link-logging switch.

## 3. Store texts (paste-ready)

### Summary

Encrypted file sharing over your local network.

### Full description

Kelp shares a file, a folder, or a webpage from your phone as a `kelp://` link
that works on the same Wi-Fi. Everything is encrypted on the device before it is
served: the key travels in the link fragment, so a relay or another peer only ever
sees ciphertext. There is no account, no cloud and no tracking.

- **Share what you mean to share** — one file, one folder, or one webpage (the
  folder holding a site), each sealed into its own bundle with its own link.
- **Open a link on another device** — paste a link, choose where to save, and
  watch per-file progress. Every object is checked against its address, decrypted,
  and written as an ordinary file.
- **Protect a link with a passphrase** — the link alone then opens nothing; send
  the passphrase separately. Kelp stores only the wrapped key, never the
  passphrase, and there is no recovery if it is forgotten.
- **Hand it over without retyping** — copy the link, show it as a QR code, or tap
  a `kelp://` link in another app to open Kelp with the link already filled in.
- **Local network first** — shares are served directly from your phone to devices
  on the same network. When a recipient cannot reach your device directly (for
  example, behind carrier NAT), Kelp falls back to a relay that forwards only
  ciphertext; the key never leaves the link. *Wi-Fi only* prevents serving over
  mobile data, and serving pauses on low battery.
- **Stay in control** — pause, resume or stop a share; keep serving while Kelp is
  in the background; see what the object store uses and remove unused objects.
- **Private by construction** — AES-256-GCM encryption, content-addressed
  objects, no analytics and no third-party SDKs.

Both devices need Kelp installed. On the same network, no relay or third party
is involved. The peer-swarm switch is present but not active yet.

### What's new — 0.1.0-1

- First Harbour release.
- Share a file, folder or webpage as an encrypted `kelp://` link and serve it on
  the local network.
- Relay fallback for recipients outside the sender's network; the relay forwards
  only ciphertext and never sees the decryption key.
- Open links with passphrase unlock, save-folder choice, per-file progress and
  verified, decrypted downloads.
- Content-addressed, AES-256-GCM-sealed object store; keys stay in link fragments,
  and passphrase links wrap the key instead.
- LAN-only serving with Wi-Fi-only and low-battery policy, background keepalive,
  and a remembered share list with pause, resume and stop.
- Offline webpage preview for bundles published as a page.
- Storage, logging and local-address settings; About page.
- Sailjail permissions limited to `Internet` and `UserDirs`.

## 4. Graphics

All store images are in `~/Jolla/store-screenshots/harbour-kelp/`.

| File | Size | Content | Source |
|---|---|---|---|
| `01-main.png` | 1080×2378 | Bundle list, two shares seeding | Emulator, address rolled back to `10.0.2.15` |
| `02-share.png` | 1080×2378 | Share page, passphrase and share types | Phone |
| `03-receive.png` | 1080×2378 | Open a link page, empty field | Emulator, clean state |
| `04-settings.png` | 1080×2378 | Settings, sharing policy and storage | Phone |
| `cover-1080x540.png` | 1080×540 | Cover banner | Generated from the app icon |
| `cover-1080x540.jpg` | 1080×540 | Cover banner, JPEG copy | Generated from the app icon |
| `icon-512.png` | 512×512 | App icon | `harbour-kelp/icons/icon-512.png` |

Notes for the upload:

- Screenshots are normalized to **1080×2378** (above the 1080×1920 minimum) to
  match the developer's previously accepted store set. Device-native captures are
  1032×2272 (phone) and 720×1600 (emulator).
- Upload the cover only to the banner field and the four screenshots only to the
  screenshot field; do not cross-upload.
- The About screenshot is deliberately excluded: it displays the log path and a
  build id, and adds nothing the other four do not show.
- `03-receive.png` is the emulator fallback because the phone capture of that page
  retained a stale "Saved 0 file(s)" panel. If a clean phone capture is taken
  later, replace this file; no other asset depends on it.
- Each image was checked for link fragments, keys, gateway URLs, QR contents and
  share dialogs. The only address shown is the emulator's own NAT address
  (`10.0.2.15`), which is not reachable from the store viewer's network.

## 5. Privacy text for the form

Short version:

> Kelp shares files only on your local network. There is no account, no cloud
> service, no analytics and no third-party SDK. Shared content is stored encrypted
> on the device, and the key is carried in the `kelp://` link. A relay is used
> only as a fallback when a recipient cannot reach your device directly; the relay
> forwards only ciphertext and never sees the key. Kelp requests only `Internet`
> (to serve shares and fetch links on the LAN, and for the relay fallback) and
> `UserDirs` (to read the files you choose and write your downloads). Full policy:
> https://github.com/kapitaali/harbour-kelp/blob/main/PRIVACY.md

Full policy: `PRIVACY.md` in the repo root. It covers the encrypted object store,
settings and share list, the diagnostic log, downloads, LAN-only networking,
passphrase links, clipboard use, QR codes, permissions, and what uninstalling
leaves behind.

## 6. Permissions and packaging

`harbour-kelp.desktop` declares:

```ini
Exec=harbour-kelp %u
MimeType=x-scheme-handler/kelp;

[X-Sailjail]
Permissions=UserDirs;Internet
OrganizationName=harbour.kelp
ApplicationName=harbour-kelp
```

- `UserDirs` — file/folder pickers, measuring and sealing a share, and writing
  downloads into *Downloads* or a chosen folder.
- `Internet` — serving objects on the LAN and fetching a link. Sailfish has no
  separate local-network permission. Also covers the relay fallback when a
  recipient cannot reach the sender's LAN address.
- `Exec=harbour-kelp %u` gives both warm and cold `kelp://` handoff; verified with
  `gio open kelp://test` after a clean install.
- No RPM `%post`/`%postun` scriptlets. The platform repopulates
  `/usr/share/applications/mimeinfo.cache` on install, so scheme registration still
  works and the validator no longer sees `/bin/sh`.
- The folder picker uses Harbour-allowed `Qt.labs.folderlistmodel 2.1`; the
  packaged `Nemo.FileManager` import is not used.
- `License: GPL-3.0-only` and a repo-root `LICENSE` file are present. The
  license is not installed into the RPM: `/usr/share/licenses` is not an allowed
  Harbour install path, and the validator rejects it there.

## 7. Validation evidence

Run 2026-10-27 with Sailfish SDK 3.13.5, target SailfishOS-5.1.0.11:

- `sfdk check -l static -s harbour` on the **aarch64** RPM:
  **Validation succeeded**, 18/18 sections PASSED.
- `sfdk check -l static -s harbour` on the **armv7hl** RPM:
  **Validation succeeded**, 18/18 sections PASSED.
- QML smoke test: **9/9 pages Component.Ready** (i486 install).
- Folder picker opened in the emulator and listed home directories.
- Warm and cold `kelp://` handoff verified after a clean install.
- No `Nemo.FileManager`, no `%post`/`%postun`, no `/bin/sh` dependency, no
  unstripped-binary warning.

Logs: `/tmp/opencode/harbour-check-aarch64-2.log`,
`/tmp/opencode/harbour-check-armv7hl3.log`.

## 8. Submission steps

1. Sign in to <https://harbour.jolla.com> and create the app entry for
   `harbour-kelp`.
2. Paste the summary and description from §3; choose **Utilities**.
3. Upload both RPMs from §2 and verify the SHA-256 values after upload.
4. Upload the cover to the banner field and `01`–`04` to the screenshot field.
5. Link the privacy policy from §5.
6. Declare `Internet` and `UserDirs` in the permission notes, with the reasons in
   §6.
7. Submit for review. Expect an automated validator pass followed by manual review.

## 9. After acceptance

- Updates go through the same dashboard. Harbour treats a release bump as an
  update only with a new **Version**; bump `Version` in the spec for the next
  submission.
- Rebuild both architectures from a clean tree (`make distclean` equivalent
  between targets) and re-run the validator before every upload.
- The i486 build is for the emulator only and must never be uploaded.
- The GitHub push is pending rotation of the exposed PAT. Do not reuse that token.
