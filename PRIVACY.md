# Kelp privacy policy

Kelp shares files and folders directly between devices on the same local network.
It has no account, no cloud service, no analytics and no telemetry. Nothing about
what you share or open is sent to the developers or to any third party.

This policy describes Kelp 0.1.0 for Sailfish OS.

## What stays on your device

- **Shared content.** When you share a file, folder or webpage, Kelp seals it into
  chunks in its own object store and serves those chunks while the share is on.
  The store holds encrypted objects; the plaintext key travels in the `kelp://`
  link, not in the store (a passphrase-protected share keeps only the
  passphrase-wrapped key in the share list). Objects are kept so a share can be
  served again after a restart and are removed when you stop a share or use
  *Settings → Storage → Remove unused objects*.
- **Settings and share list.** Your choices (for example whether links include this
  device's local address, whether sharing runs in the background, and the folder
  downloads are saved into) and the list of shares are stored in a small file in
  the app's sandboxed data directory.
- **Log.** Kelp writes a diagnostic log to
  `~/.local/share/harbour.kelp/harbour-kelp/kelp.log`. In a release build it does
  not contain `kelp://` links or their keys, and file contents are never logged.
  *Settings → Clear log* deletes it; *Settings → Include links in the log* is not
  offered in a release build.
- **Downloads.** A bundle you open is verified, decrypted and written as ordinary
  files into the folder you choose — *Downloads* by default. Those files are then
  outside the app and are yours to keep, move or delete.

The object store, settings and log live under
`~/.local/share/harbour.kelp/harbour-kelp/`, the directory Sailjail provisions for
the app. Other apps cannot read it. Uninstalling Kelp follows standard Sailfish OS
behaviour and may leave this directory behind; remove it, or use the in-app storage
actions first, if you want the data gone.

## Network use

- Kelp serves shares directly between devices on the same local network. When
  both devices are on the same network, **no relay or third party is involved**:
  the recipient connects straight to your device and all traffic stays on the
  LAN.
- When a recipient cannot reach your device directly (for example, you are on
  mobile data or behind carrier NAT), Kelp can fall back to a relay at
  `82.70.43.26:9443`. The relay only forwards ciphertext: the decryption key
  travels in the `kelp://` link fragment and is never sent to the relay. The
  relay cannot read, modify or store the content of your shares.
- The relay is used only as a fallback. If the recipient is on the same network
  as you, the relay is never contacted and no data leaves the LAN.
- Opening a `kelp://` link connects to the address carried inside that link —
  your device on the same network, or the relay as a fallback — and fetches
  the bundle's objects from it. No other host is contacted.
- The link contains the key needed to decrypt the bundle, so treat a link as a
  secret. Anyone who has the link can read the share. A passphrase-protected
  link carries the key wrapped under the passphrase; the passphrase itself is
  never stored or sent anywhere.
- The optional *Wi-Fi only* setting (on by default) stops serving over mobile
  data, and serving also stops on low battery while unplugged.

## Permissions

Kelp declares two Sailjail permissions, both used only for the features above:

- **UserDirs** — to read the files and folders you choose to share, and to write
  downloads into *Downloads* or another folder you pick.
- **Internet** — to serve shares on the local network and to fetch a bundle when
  you open a link. Sailfish OS has no separate local-network permission, so
  `Internet` is what enables the app's network access.

Kelp does not request Contacts, Calendar, Location, Camera, Microphone, Sensors or
notifications.

## Clipboard and QR codes

Kelp writes a `kelp://` link to the system clipboard only when you choose *Copy
link*. It reads the clipboard only when you press *Paste from clipboard* on the
Open-a-link page. The QR code shown on a bundle page is generated on the device
and contains the same link, including its key; displaying it shares the capability
with anyone who can see the screen.

## Changes

If a future version changes how Kelp handles data or network access, this policy
will be updated in the source repository before that version is published:
<https://github.com/kapitaali/harbour-kelp>.

_Last updated: 2026-10-28._
