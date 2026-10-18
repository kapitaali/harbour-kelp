#include "kelp_link.h"

#include <QPair>
#include <QUrl>
#include <QVector>

#include "kelp_cbor.h"
#include "kelp_crypto.h"

namespace Kelp {
namespace Link {

namespace {

const char kBase32Alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
const char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const char kScheme[] = "kelp://";

// The key the link's own location hints are sealed under. It is an independent
// HKDF output, so a link address key is useless for reading content or joining a
// swarm — the same separation content/swarm/relay already keep.
QByteArray linkKey(const QByteArray &secret)
{
    QByteArray out;
    if (!Crypto::hkdfSha256(secret, QByteArray(Crypto::Sha256Size, '\0'),
                            QByteArrayLiteral("kelp/v1/link"), Crypto::KeySize, &out))
        return QByteArray();
    return out;
}

// {l: address, n: label, p: is-a-page} as canonical CBOR, sealed into
// nonce || ciphertext || tag and base64url'd. Returns an empty string when there
// is nothing to seal or the secret cannot key the seal.
QString sealLocations(const Bundle &bundle)
{
    const QByteArray key = linkKey(bundle.secret);
    if (key.size() != Crypto::KeySize)
        return QString();

    QVector<QPair<QByteArray, QByteArray> > entries;
    if (!bundle.lan.isEmpty())
        entries.append(qMakePair(Cbor::encodeText(QStringLiteral("l")),
                                 Cbor::encodeText(bundle.lan)));
    if (!bundle.name.isEmpty())
        entries.append(qMakePair(Cbor::encodeText(QStringLiteral("n")),
                                 Cbor::encodeText(bundle.name)));
    // Sent as `true` only, never `false`: absence means "an ordinary share", so
    // a page share is still a page share when a relay is the only way to reach
    // it (no address to seal, but the block is minted anyway).
    if (bundle.page)
        entries.append(qMakePair(Cbor::encodeText(QStringLiteral("p")),
                                 Cbor::encodeBool(true)));
    const QByteArray payload = Cbor::encodeMap(entries);
    if (payload.isEmpty())
        return QString();

    const QByteArray nonce = Crypto::randomBytes(Crypto::NonceSize);
    if (nonce.size() != Crypto::NonceSize)
        return QString();

    QByteArray ciphertext;
    // The bundle id is the associated data, so a block cannot be lifted out of
    // one link and pasted into another.
    if (!Crypto::aes256GcmEncrypt(key, nonce, payload, bundle.bundleId, &ciphertext))
        return QString();

    return base64UrlEncode(nonce + ciphertext);
}

bool openLocations(const QByteArray &secret, const QByteArray &bundleId,
                   const QString &blob, QString *lan, QString *name, bool *page,
                   QString *error)
{
    QByteArray raw;
    if (!base64UrlDecode(blob, &raw, error))
        return false;
    if (raw.size() < Crypto::NonceSize + Crypto::TagSize) {
        *error = QStringLiteral("the link's address block is too short");
        return false;
    }

    const QByteArray key = linkKey(secret);
    if (key.size() != Crypto::KeySize) {
        *error = QStringLiteral("cannot derive the address key for this link");
        return false;
    }

    QByteArray payload;
    if (!Crypto::aes256GcmDecrypt(key, raw.left(Crypto::NonceSize),
                                  raw.mid(Crypto::NonceSize), bundleId, &payload)) {
        *error = QStringLiteral("the link's address block did not decrypt");
        return false;
    }

    Cbor::Value value;
    if (!Cbor::decode(payload, &value, error))
        return false;
    if (!value.isMap()) {
        *error = QStringLiteral("the link's address block is not a map");
        return false;
    }

    if (const Cbor::Value *l = value.member("l")) {
        if (l->type != Cbor::Value::Text) {
            *error = QStringLiteral("the link's address is not text");
            return false;
        }
        *lan = l->text;
    }
    if (const Cbor::Value *n = value.member("n")) {
        if (n->type != Cbor::Value::Text) {
            *error = QStringLiteral("the link's label is not text");
            return false;
        }
        *name = n->text;
    }
    if (const Cbor::Value *p = value.member("p")) {
        if (p->type != Cbor::Value::Bool) {
            *error = QStringLiteral("the link's page hint is not a boolean");
            return false;
        }
        *page = p->boolValue;
    }
    return true;
}

// Passphrases are Unicode text, so the same passphrase typed on two devices has
// to reach the KDF as the same bytes. NFC is the only normalization done — the
// passphrase is otherwise used exactly as typed, spaces and all.
QByteArray passphraseBytes(const QString &passphrase)
{
    return passphrase.normalized(QString::NormalizationForm_C).toUtf8();
}

} // namespace

QByteArray base32Encode(const QByteArray &data)
{
    QByteArray out;
    out.reserve((data.size() * 8 + 4) / 5);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < data.size(); ++i) {
        buffer = (buffer << 8) | quint32(quint8(data.at(i)));
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.append(kBase32Alphabet[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0)
        out.append(kBase32Alphabet[(buffer << (5 - bits)) & 0x1F]);
    return out;
}

bool base32Decode(const QString &text, QByteArray *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    out->clear();
    out->reserve(text.size() * 5 / 8);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        int value = -1;
        if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            value = c.unicode() - 'a';
        else if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            value = c.unicode() - 'A';
        else if (c >= QLatin1Char('2') && c <= QLatin1Char('7'))
            value = 26 + (c.unicode() - '2');
        if (value < 0) {
            *error = QStringLiteral("'%1' is not a base32 character").arg(c);
            return false;
        }

        buffer = (buffer << 5) | quint32(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out->append(char((buffer >> bits) & 0xFF));
        }
    }

    // The leftover bits of an unpadded encoding must be zero, or two different
    // strings would decode to the same bytes.
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        *error = QStringLiteral("base32 input has non-zero padding bits");
        return false;
    }
    return true;
}

QString base64UrlEncode(const QByteArray &data)
{
    QByteArray out;
    out.reserve((data.size() * 4 + 2) / 3);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < data.size(); ++i) {
        buffer = (buffer << 8) | quint32(quint8(data.at(i)));
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.append(kBase64UrlAlphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0)
        out.append(kBase64UrlAlphabet[(buffer << (6 - bits)) & 0x3F]);
    return QString::fromLatin1(out);
}

bool base64UrlDecode(const QString &text, QByteArray *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    out->clear();
    out->reserve(text.size() * 3 / 4);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        int value = -1;
        if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            value = c.unicode() - 'A';
        else if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            value = 26 + (c.unicode() - 'a');
        else if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            value = 52 + (c.unicode() - '0');
        else if (c == QLatin1Char('-') || c == QLatin1Char('+'))
            value = 62;
        else if (c == QLatin1Char('_') || c == QLatin1Char('/'))
            value = 63;
        else if (c == QLatin1Char('='))
            continue;                      // padding is optional here
        else {
            *error = QStringLiteral("'%1' is not a base64url character").arg(c);
            return false;
        }

        buffer = (buffer << 6) | quint32(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->append(char((buffer >> bits) & 0xFF));
        }
    }

    if (bits >= 6) {
        *error = QStringLiteral("base64url input has a dangling group");
        return false;
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        *error = QStringLiteral("base64url input has non-zero padding bits");
        return false;
    }
    return true;
}

QString encode(const Bundle &bundle)
{
    QString text = QString::fromLatin1(kScheme);
    text += QString::fromLatin1(base32Encode(bundle.bundleId));

    QStringList query;
    for (int i = 0; i < bundle.relays.size(); ++i) {
        query.append(QStringLiteral("r=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(bundle.relays.at(i))));
    }

    // The address, the label and whether this is a page are the link's only
    // plaintext metadata, so they go into one opaque `q=` block. A reader without
    // the fragment then sees a bundle id, some relay names, and random-looking
    // bytes — not where the bundle is being served from, nor what it is.
    if (!bundle.lan.isEmpty() || !bundle.name.isEmpty() || bundle.page) {
        const QString sealed = sealLocations(bundle);
        if (!sealed.isEmpty()) {
            query.append(QStringLiteral("q=") + sealed);
        } else if (bundle.wrappedSecret.isEmpty()) {
            // No usable secret to seal under — an internal-invariant breach
            // (decode would reject such a link anyway). Keep the address rather
            // than mint a link that reaches nothing; the page hint cannot be
            // carried in the clear, so such a link would read as an ordinary
            // share, which is the safe way to fail.
            if (!bundle.lan.isEmpty())
                query.append(QStringLiteral("l=")
                             + QString::fromLatin1(QUrl::toPercentEncoding(bundle.lan)));
            if (!bundle.name.isEmpty())
                query.append(QStringLiteral("n=")
                             + QString::fromLatin1(QUrl::toPercentEncoding(bundle.name)));
        }
        // For a passphrase link there is no fallback: the whole point of one is
        // that the link text gives nothing away, so a link whose hints could not
        // be sealed simply carries no hints, and fails closed.
    }

    if (!query.isEmpty())
        text += QLatin1Char('?') + query.join(QLatin1Char('&'));

    if (!bundle.wrappedSecret.isEmpty())
        text += QStringLiteral("#p=") + base64UrlEncode(bundle.wrappedSecret);
    else
        text += QStringLiteral("#k=") + base64UrlEncode(bundle.secret);
    return text;
}

bool decode(const QString &text, Bundle *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!out) {
        *error = QStringLiteral("no destination for the link");
        return false;
    }

    QString rest = text.trimmed();
    if (!rest.startsWith(QString::fromLatin1(kScheme), Qt::CaseInsensitive)) {
        *error = QStringLiteral("link does not start with kelp://");
        return false;
    }
    rest = rest.mid(int(sizeof(kScheme) - 1));

    QString fragment;
    const int hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        fragment = rest.mid(hash + 1);
        rest = rest.left(hash);
    }

    QString query;
    const int question = rest.indexOf(QLatin1Char('?'));
    if (question >= 0) {
        query = rest.mid(question + 1);
        rest = rest.left(question);
    }

    Bundle bundle;
    if (!base32Decode(rest, &bundle.bundleId, error))
        return false;
    if (bundle.bundleId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id is not 32 bytes");
        return false;
    }

    bool haveSecret = false;
    bool haveWrapped = false;
    const QStringList fragmentParts = fragment.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < fragmentParts.size(); ++i) {
        const QString &part = fragmentParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = part.left(equals);
        if (key == QLatin1String("k")) {
            if (haveSecret) {
                *error = QStringLiteral("link has more than one key fragment");
                return false;
            }
            if (!base64UrlDecode(part.mid(equals + 1), &bundle.secret, error))
                return false;
            haveSecret = true;
        } else if (key == QLatin1String("p")) {
            if (haveWrapped) {
                *error = QStringLiteral("link has more than one passphrase fragment");
                return false;
            }
            if (!base64UrlDecode(part.mid(equals + 1), &bundle.wrappedSecret, error))
                return false;
            haveWrapped = true;
        }
    }
    if (haveSecret && haveWrapped) {
        *error = QStringLiteral("link has both a key and a passphrase fragment");
        return false;
    }
    if (!haveSecret && !haveWrapped) {
        *error = QStringLiteral("link has no key and no passphrase fragment");
        return false;
    }
    if (haveWrapped) {
        // The blob is opaque here: all this build can check without a passphrase
        // is that it is the shape it knows, which is enough to say "damaged"
        // rather than "wrong passphrase" later.
        int iterations = 0;
        if (bundle.wrappedSecret.size() != WrappedSize
            || !wrappedParameters(bundle.wrappedSecret, &iterations, 0)) {
            *error = QStringLiteral("the passphrase block in this link is not one this version knows");
            return false;
        }
    } else if (bundle.secret.size() != Crypto::KeySize) {
        *error = QStringLiteral("secret is not 32 bytes");
        return false;
    }

    QString sealedLocations;
    const QStringList queryParts = query.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < queryParts.size(); ++i) {
        const QString &part = queryParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = part.left(equals);
        if (key == QLatin1String("q")) {
            sealedLocations = part.mid(equals + 1);   // base64url: no escapes
            continue;
        }
        const QString value =
            QUrl::fromPercentEncoding(part.mid(equals + 1).toUtf8());
        if (key == QLatin1String("r"))
            bundle.relays.append(value);
        else if (key == QLatin1String("l"))
            bundle.lan = value;
        else if (key == QLatin1String("n"))
            bundle.name = value;
    }

    // The sealed block is authoritative — it is what this build mints, and the
    // recipient needs the address in it to reach the publisher at all. A
    // passphrase link cannot open it yet: the block is sealed under the secret
    // the passphrase unwraps, so it stays closed until unlock().
    if (!sealedLocations.isEmpty()) {
        if (bundle.secret.isEmpty()) {
            bundle.sealedLocations = sealedLocations;
        } else if (!openLocations(bundle.secret, bundle.bundleId, sealedLocations,
                                  &bundle.lan, &bundle.name, &bundle.page, error)) {
            return false;
        }
    }

    *out = bundle;
    return true;
}

bool needsPassphrase(const Bundle &bundle)
{
    return bundle.secret.isEmpty() && !bundle.wrappedSecret.isEmpty();
}

bool wrappedParameters(const QByteArray &blob, int *iterations, QByteArray *salt)
{
    if (blob.size() != WrappedSize)
        return false;
    if (quint8(blob.at(0)) != quint8(WrappedVersion))
        return false;
    const quint32 count = (quint32(quint8(blob.at(1))) << 24)
        | (quint32(quint8(blob.at(2))) << 16)
        | (quint32(quint8(blob.at(3))) << 8)
        | quint32(quint8(blob.at(4)));
    if (count == 0)
        return false;
    if (iterations)
        *iterations = int(count);
    if (salt)
        *salt = blob.mid(5, SaltSize);
    return true;
}

QByteArray wrapSecret(const QByteArray &secret, const QByteArray &bundleId,
                      const QString &passphrase, int iterations, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (secret.size() != Crypto::KeySize) {
        *error = QStringLiteral("the key to wrap is not 32 bytes");
        return QByteArray();
    }
    if (bundleId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("the bundle id is not 32 bytes");
        return QByteArray();
    }
    if (iterations <= 0)
        iterations = DefaultIterations;
    if (iterations > MaxIterations) {
        *error = QStringLiteral("that cost is above what this version mints");
        return QByteArray();
    }

    const QByteArray salt = Crypto::randomBytes(SaltSize);
    const QByteArray nonce = Crypto::randomBytes(Crypto::NonceSize);
    if (salt.size() != SaltSize || nonce.size() != Crypto::NonceSize) {
        *error = QStringLiteral("no randomness available");
        return QByteArray();
    }

    QByteArray key;
    if (!Crypto::pbkdf2Sha256(passphraseBytes(passphrase), salt, iterations,
                              Crypto::KeySize, &key)) {
        *error = QStringLiteral("the passphrase could not be used");
        return QByteArray();
    }

    QByteArray wrapped;
    // The bundle id is the authenticated data, so a wrapped key cannot be moved
    // onto another bundle's link.
    if (!Crypto::aes256GcmEncrypt(key, nonce, secret, bundleId, &wrapped)) {
        *error = QStringLiteral("the key could not be wrapped");
        return QByteArray();
    }

    QByteArray blob;
    blob.append(char(WrappedVersion));
    blob.append(char((iterations >> 24) & 0xFF));
    blob.append(char((iterations >> 16) & 0xFF));
    blob.append(char((iterations >> 8) & 0xFF));
    blob.append(char(iterations & 0xFF));
    blob.append(salt);
    blob.append(nonce);
    blob.append(wrapped);
    return blob;
}

bool unlock(Bundle *bundle, const QString &passphrase, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!bundle) {
        *error = QStringLiteral("no link to unlock");
        return false;
    }
    if (bundle->wrappedSecret.isEmpty()) {
        if (!bundle->secret.isEmpty())
            return true;                    // an ordinary link is already open
        *error = QStringLiteral("this link has no key to unlock");
        return false;
    }

    int iterations = 0;
    QByteArray salt;
    if (!wrappedParameters(bundle->wrappedSecret, &iterations, &salt)) {
        *error = QStringLiteral("the passphrase block in this link is not one this version knows");
        return false;
    }
    // A link is untrusted input and the derivation is meant to be slow, so the
    // cost is bounded: a hostile link must not be able to hang the app.
    if (iterations > MaxIterations) {
        *error = QStringLiteral("this link asks for more work than is allowed");
        return false;
    }

    QByteArray key;
    if (!Crypto::pbkdf2Sha256(passphraseBytes(passphrase), salt, iterations,
                              Crypto::KeySize, &key)) {
        *error = QStringLiteral("the passphrase could not be used");
        return false;
    }

    const QByteArray nonce = bundle->wrappedSecret.mid(WrappedHeaderSize, Crypto::NonceSize);
    const QByteArray body = bundle->wrappedSecret.mid(WrappedHeaderSize + Crypto::NonceSize);
    QByteArray secret;
    if (!Crypto::aes256GcmDecrypt(key, nonce, body, bundle->bundleId, &secret)) {
        // The tag is the only thing that can tell: a wrong passphrase and a
        // blob moved onto another bundle's link both land here.
        *error = QStringLiteral("that passphrase does not open this link");
        return false;
    }
    if (secret.size() != Crypto::KeySize) {
        *error = QStringLiteral("the unwrapped key is not 32 bytes");
        return false;
    }
    bundle->secret = secret;

    // Now that the secret is known, the hints decode() could not read.
    if (!bundle->sealedLocations.isEmpty()
        && !openLocations(bundle->secret, bundle->bundleId, bundle->sealedLocations,
                          &bundle->lan, &bundle->name, &bundle->page, error))
        return false;
    return true;
}

} // namespace Link
} // namespace Kelp
