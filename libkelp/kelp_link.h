#ifndef KELP_LINK_H
#define KELP_LINK_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace Kelp {
namespace Link {

/*
 * The kelp:// link (wire spec §5).
 *
 *   kelp://<bundle_id_b32>?r=<relay>[&r=…][&q=<blob>]#k=<secret>
 *   kelp://<bundle_id_b32>?r=<relay>[&r=…][&q=<blob>]#p=<blob>   passphrase link
 *
 * The bundle id is base32 — lowercase and unpadded, which is 52 characters for
 * 32 bytes — so a link survives case-insensitive handling and can be read out
 * loud. The secret is base64url. Everything after '#' is the fragment: a
 * fragment is never sent to a server, which is what makes the link a capability
 * rather than a request.
 *
 * `q=` is the publisher's location hints — its own host:port, the share's
 * label, and whether the share is a page — sealed with AES-256-GCM under
 * HKDF(secret, "kelp/v1/link"). The link text therefore does not say where the
 * bundle is served from, nor what kind of thing it is, to anything that sees it
 * without the fragment; the recipient, which holds the fragment, decrypts it and
 * dials it. A link minted before this carries the same hints in the clear as
 * `l=` and `n=`, and decode still accepts them, so links already out there keep
 * working.
 *
 * A *passphrase link* (design §12.3) has no `#k=` at all: the fragment holds
 * `#p=`, the master secret wrapped under a key derived from a passphrase the
 * link does not contain. The link alone therefore opens nothing, and the
 * address hints in `q=` — sealed under the master secret — cannot be read
 * either, so a passphrase link gives away nothing at all until it is unlocked.
 * `#k=` and `#p=` are mutually exclusive; a link carrying both is malformed.
 */

// The `#p=` blob:
//
//     version(1) || iterations(4, big-endian) || salt(16) || nonce(12) || ct+tag(48)
//
// 81 bytes, 108 base64url characters. Self-describing on purpose: a later change
// of KDF, of salt width, or of cost is a new version, not a new field, and the
// count travelling in the link is what lets the default be raised without
// breaking links already handed out.
enum {
    WrappedVersion = 1,
    WrappedHeaderSize = 1 + 4 + 16,
    WrappedSize = WrappedHeaderSize + 12 + 32 + 16,
    SaltSize = 16
};

// The cost a new link is minted with: about a third of a second on a desktop and
// well under a second on the C2, paid once per open. A link may name more.
enum { DefaultIterations = 600000 };

// What this build will honour. A link is untrusted input and the derivation is
// deliberately expensive, so an implausible count is refused rather than obeyed:
// a hostile link must not be able to hang the app for a minute.
enum { MaxIterations = 10000000 };

struct Bundle {
    QByteArray bundleId;    // 32 bytes
    QByteArray secret;      // 32 bytes, the master secret (empty until unlock()
                            // for a passphrase link)
    QStringList relays;     // tried in order
    QString lan;            // publisher host:port (direct fast path)
    QString name;           // optional label
    // The publisher shared this as a webpage (design §5.5): a recipient should
    // open it in a browser and leave nothing behind, not unpack it. A hint, not
    // enforcement — the bundle is an ordinary bundle either way.
    bool page;

    // Passphrase links. `wrappedSecret` is the raw `#p=` blob (not its base64url
    // text), non-empty exactly when this is a passphrase link. `sealedLocations`
    // keeps the `q=` text so unlock() can open it once the secret is derived —
    // decode() cannot, because it has no secret yet.
    QByteArray wrappedSecret;
    QString sealedLocations;

    Bundle() : page(false) {}
};

QString encode(const Bundle &bundle);

// Strict: the scheme must be right, the id must decode to 32 bytes; the fragment
// must hold a 32-byte `#k=` or a well-formed `#p=` (not both). For a `#p=` link
// nothing is opened: `secret` comes back empty and `lan`/`name`/`page` stay
// unset until unlock() succeeds.
bool decode(const QString &text, Bundle *out, QString *error);

// Whether this bundle needs a passphrase before it can be used at all.
bool needsPassphrase(const Bundle &bundle);

// Wraps `secret` (32 bytes) under `passphrase` for `bundleId`, producing the blob
// that goes in `#p=`. The bundle id is the authenticated data, so a blob cannot
// be moved onto another bundle's link. `iterations` <= 0 means DefaultIterations.
// Returns the raw blob, or empty with a reason.
QByteArray wrapSecret(const QByteArray &secret, const QByteArray &bundleId,
                      const QString &passphrase, int iterations, QString *error);

// Derives the key from `passphrase` and the blob's own parameters, unwraps the
// secret into `bundle->secret`, and then opens the `q=` hints that decode() had
// to leave closed. False with a reason on a wrong passphrase (the GCM tag is the
// only thing that can tell), a malformed blob, or an implausible cost. The
// passphrase is not kept anywhere.
bool unlock(Bundle *bundle, const QString &passphrase, QString *error);

// The cost and salt a blob records, so a caller can see what it will pay without
// deriving anything. False when the blob is malformed or its version unknown.
bool wrappedParameters(const QByteArray &blob, int *iterations, QByteArray *salt);

// RFC 4648. Encoding is lowercase/unpadded base32 and unpadded base64url;
// decoding accepts either case, and tolerates '='/standard base64 on the
// base64url side, since links get retyped by hand.
QByteArray base32Encode(const QByteArray &data);
bool base32Decode(const QString &text, QByteArray *out, QString *error);
QString base64UrlEncode(const QByteArray &data);
bool base64UrlDecode(const QString &text, QByteArray *out, QString *error);

} // namespace Link
} // namespace Kelp

#endif // KELP_LINK_H
