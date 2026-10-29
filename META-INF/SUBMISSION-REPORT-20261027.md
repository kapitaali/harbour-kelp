# Kelp 0.1.0 — final submission report

**Status:** ready to upload to Jolla Harbour. Both architectures are built,
validated and copied out. The only remaining steps are the authenticated GitHub
push (after PAT rotation) and the Harbour dashboard upload itself.

**Prepared:** 2026-10-28. Target: Sailfish OS 5.1.0.11, SDK 3.13.5.

## 1. Identity

| Field | Value |
|---|---|
| Title | **Kelp** |
| Package | `harbour-kelp` |
| Version / release | `0.1.0-1` |
| Category | **Utilities** |
| License | `GPL-3.0-only` |
| Source | <https://github.com/kapitaali/harbour-kelp> |
| Summary | Encrypted file sharing over your local network. |
| Privacy policy | <https://github.com/kapitaali/harbour-kelp/blob/main/PRIVACY.md> |

Paste-ready description and changelog: `META-INF/STORE-LISTING.md` §3.

## 2. Upload these exact files

| Arch | Path | Size | SHA-256 |
|---|---|---|---|
| aarch64 | `/home/theb/Downloads/harbour-kelp-0.1.0-1.aarch64.rpm` | 331,171 B | `9d9cdfdc2bb74b607991569a7a45effd6da61729efe6d48cf54cff8845e9dce6` |
| armv7hl | `/home/theb/Downloads/harbour-kelp-0.1.0-1.armv7hl.rpm` | 302,839 B | `8bc2707f919aee0a84f7a1108f08941af6f95aadd52198f37bb3f17d57f78d76` |

Do **not** upload the i486 emulator build from `RPMS/SailfishOS-5.1.0.11-i486/`.

## 3. Validation

```text
sfdk check -l static -s harbour, aarch64  : Validation succeeded, 18/18 PASSED
sfdk check -l static -s harbour, armv7hl  : Validation succeeded, 18/18 PASSED
QML smoke                                 : 9/9 pages Component.Ready
```

Logs: `/tmp/opencode/harbour-check-aarch64-2.log`,
`/tmp/opencode/harbour-check-armv7hl3.log`.

Also verified: clean install registers `x-scheme-handler/kelp`; warm and cold
`kelp://` handoff; folder picker lists home directories; post-reboot launch
restores and serves saved shares; a real link was downloaded into the emulator's
Downloads (1 PDF, 2,463,198 bytes, 12 objects).

## 4. Store graphics

Directory: `/home/theb/Jolla/store-screenshots/harbour-kelp/`

| File | Use |
|---|---|
| `cover-1080x540.png` or `.jpg` | Banner field only |
| `01-main.png` | Screenshot 1: bundle list |
| `02-share.png` | Screenshot 2: Share page |
| `03-receive.png` | Screenshot 3: Open a link |
| `04-settings.png` | Screenshot 4: Settings |
| `icon-512.png` | Icon reference |

All four screenshots are 1080x2378. The About screenshot is intentionally
excluded. `03-receive.png` is the clean emulator fallback; the phone capture of
that page retained a stale Done panel. No screenshot contains a link fragment,
key, QR payload, share dialog or real phone address.

## 5. Privacy and permissions

- **UserDirs** — file/folder pickers, reading the chosen share, writing downloads.
- **Internet** — serving objects on the LAN and fetching links; Sailfish has no
  separate local-network permission. Also covers the relay fallback when a
  recipient cannot reach the sender's LAN address.
- No account, analytics, ads or third-party SDK. A relay at `82.70.43.26:9443`
  is used only as a fallback when a recipient cannot reach the sender's LAN
  address; the relay forwards only ciphertext and never sees the decryption key.
  On the same network, no relay is contacted.
- Full policy: `PRIVACY.md`.

## 6. Packaging fixes included

- `Qt.labs.folderlistmodel 2.1` instead of disallowed `Nemo.FileManager`.
- `Exec=harbour-kelp %u` instead of the `invoker` form.
- No `%post`/`%postun`; no `/bin/sh` dependency.
- Repo-root `LICENSE` (not installed; `/usr/share/licenses` is rejected).
- Populated RPM changelog for the first release.
- `%define kelp_dev_extras 0`.
- Relay support: LAN-first with relay fallback for off-network recipients.

## 7. Dashboard checklist

1. Create/select `harbour-kelp` at <https://harbour.jolla.com>.
2. Paste summary and description from `META-INF/STORE-LISTING.md` §3.
3. Choose **Utilities**.
4. Upload both RPMs from §2; re-check their SHA-256 after upload.
5. Upload the cover to the banner field and `01`–`04` to screenshots.
6. Link the privacy policy.
7. Note permissions and reasons from §5.
8. Submit for review.

## 8. Remaining external actions

- **GitHub push:** four commits are waiting on `origin/master` (`a4f4c95`,
  `f11af25` plus the two handoff commits). Rotate the exposed PAT first; never
  reuse it. Then `git push origin master`.
- **Harbour upload:** requires the owner's account; not performed here.
- **Optional:** replace `03-receive.png` with a clean phone capture after
  restarting Kelp to clear the Done panel. Not a blocker.

No `kelp://` link, fragment, key, QR content or phone LAN address is recorded in
this report.
