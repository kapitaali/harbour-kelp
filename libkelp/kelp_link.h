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
 */

struct Bundle {
    QByteArray bundleId;    // 32 bytes
    QByteArray secret;      // 32 bytes, the master secret
    QStringList relays;     // tried in order
    QString lan;            // publisher host:port (direct fast path)
    QString name;           // optional label
    // The publisher shared this as a webpage (design §5.5): a recipient should
    // open it in a browser and leave nothing behind, not unpack it. A hint, not
    // enforcement — the bundle is an ordinary bundle either way.
    bool page;

    Bundle() : page(false) {}
};

QString encode(const Bundle &bundle);

// Strict: the scheme must be right, the id must decode to 32 bytes and the
// secret must be present and 32 bytes.
bool decode(const QString &text, Bundle *out, QString *error);

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
