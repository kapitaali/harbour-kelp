#include "kelp_channel.h"

#include <limits>

#include <QPair>
#include <QVector>

#include "kelp_cbor.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_object.h"

#include <QUrl>

namespace Kelp {
namespace Channel {

namespace {

typedef QPair<QByteArray, QByteArray> CborPair;

CborPair textPair(const char *key, const QString &value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeText(value));
}

CborPair uintPair(const char *key, quint64 value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeUnsigned(value));
}

CborPair intPair(const char *key, qint64 value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeInt(value));
}

CborPair bytesPair(const char *key, const QByteArray &value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeBytes(value));
}

bool integerMember(const Cbor::Value &map, const char *key, quint64 *out,
                   QString *error)
{
    const Cbor::Value *value = map.member(key);
    if (!value || value->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("channel pointer has no unsigned \"%1\"")
                     .arg(QString::fromLatin1(key));
        return false;
    }
    *out = value->unsignedValue;
    return true;
}

bool bytesMember(const Cbor::Value &map, const char *key, int expectedSize,
                 QByteArray *out, QString *error)
{
    const Cbor::Value *value = map.member(key);
    if (!value || value->type != Cbor::Value::Bytes
        || value->bytes.size() != expectedSize) {
        *error = QStringLiteral("channel pointer has no %1-byte \"%2\"")
                     .arg(expectedSize)
                     .arg(QString::fromLatin1(key));
        return false;
    }
    *out = value->bytes;
    return true;
}

// The pointer must be exactly the canonical map for version 1. Unknown keys are
// refused, not ignored: the signature covers the whole body, so accepting a key
// this build does not understand would mean either re-encoding different bytes
// (and failing every valid signature) or trusting unverified fields.
bool checkKeys(const Cbor::Value &map, const char *const *keys, int keyCount,
               bool signedMessage, const char *what, QString *error)
{
    for (int i = 0; i < map.map.size(); ++i) {
        const Cbor::Value &key = map.map.at(i).first;
        if (key.type != Cbor::Value::Text) {
            *error = QStringLiteral("%1 has a non-text key")
                         .arg(QString::fromLatin1(what));
            return false;
        }
        bool known = false;
        for (int j = 0; j < keyCount && !known; ++j)
            known = key.text == QString::fromLatin1(keys[j]);
        if (!known)
            known = signedMessage && key.text == QStringLiteral("sig");
        if (!known) {
            *error = QStringLiteral("%1 has an unknown key \"%2\"")
                         .arg(QString::fromLatin1(what), key.text);
            return false;
        }
    }
    return true;
}

} // namespace

bool generateKeyPair(KeyPair *keys, QString *error)
{
    if (!keys) {
        if (error)
            *error = QStringLiteral("no key pair to fill");
        return false;
    }
    if (!Crypto::ed25519Generate(&keys->publicKey, &keys->privateKey)) {
        if (error)
            *error = QStringLiteral("could not generate an Ed25519 key pair");
        return false;
    }
    return true;
}

QByteArray id(const QByteArray &publicKey)
{
    if (publicKey.size() != PublicKeySize)
        return QByteArray();
    return Crypto::sha256(publicKey);
}

QByteArray encodeBody(const Pointer &pointer)
{
    if (pointer.bundleId.size() != Crypto::Sha256Size
        || pointer.bundleSecret.size() != Crypto::KeySize
        || pointer.publicKey.size() != PublicKeySize
        || pointer.version != Version
        || pointer.sequence < 1
        || pointer.timestamp < 0)
        return QByteArray();

    QVector<CborPair> entries;
    entries.append(uintPair("v", pointer.version));
    entries.append(textPair("name", pointer.name));
    entries.append(bytesPair("bundle", pointer.bundleId));
    entries.append(bytesPair("key", pointer.bundleSecret));
    entries.append(uintPair("seq", pointer.sequence));
    entries.append(intPair("ts", pointer.timestamp));
    entries.append(bytesPair("pub", pointer.publicKey));
    return Cbor::encodeMap(entries);
}

QByteArray encode(const Pointer &pointer)
{
    if (pointer.signature.size() != SignatureSize)
        return QByteArray();
    const QByteArray body = encodeBody(pointer);
    if (body.isEmpty())
        return QByteArray();

    // build the same map plus the signature, then encode it canonically; the
    // body bytes are not embedded, because canonical maps are not concatenative.
    QVector<CborPair> entries;
    entries.append(uintPair("v", pointer.version));
    entries.append(textPair("name", pointer.name));
    entries.append(bytesPair("bundle", pointer.bundleId));
    entries.append(bytesPair("key", pointer.bundleSecret));
    entries.append(uintPair("seq", pointer.sequence));
    entries.append(intPair("ts", pointer.timestamp));
    entries.append(bytesPair("pub", pointer.publicKey));
    entries.append(bytesPair("sig", pointer.signature));
    return Cbor::encodeMap(entries);
}

bool decode(const QByteArray &bytes, Pointer *pointer, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!pointer) {
        *error = QStringLiteral("no pointer to fill");
        return false;
    }

    Cbor::Value map;
    if (!Cbor::decode(bytes, &map, error))
        return false;
    if (!map.isMap()) {
        *error = QStringLiteral("channel pointer is not a CBOR map");
        return false;
    }
    static const char *const bodyKeys[] = { "v", "name", "bundle", "key", "seq", "ts", "pub" };
    if (!checkKeys(map, bodyKeys, 7, true, "channel pointer", error))
        return false;

    Pointer parsed;
    if (!integerMember(map, "v", &parsed.version, error))
        return false;
    if (parsed.version != Version) {
        *error = QStringLiteral("unsupported channel pointer version %1")
                     .arg(parsed.version);
        return false;
    }

    const Cbor::Value *name = map.member("name");
    if (!name || name->type != Cbor::Value::Text) {
        *error = QStringLiteral("channel pointer has no \"name\"");
        return false;
    }
    parsed.name = name->text;

    if (!bytesMember(map, "bundle", Crypto::Sha256Size, &parsed.bundleId, error))
        return false;
    if (!bytesMember(map, "key", Crypto::KeySize, &parsed.bundleSecret, error))
        return false;
    if (!integerMember(map, "seq", &parsed.sequence, error))
        return false;
    if (parsed.sequence < 1) {
        *error = QStringLiteral("channel pointer has a zero sequence");
        return false;
    }

    const Cbor::Value *timestamp = map.member("ts");
    if (!timestamp || timestamp->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("channel pointer has no unsigned \"ts\"");
        return false;
    }
    if (timestamp->unsignedValue > quint64(std::numeric_limits<qint64>::max())) {
        *error = QStringLiteral("channel pointer timestamp is out of range");
        return false;
    }
    parsed.timestamp = qint64(timestamp->unsignedValue);

    if (!bytesMember(map, "pub", PublicKeySize, &parsed.publicKey, error))
        return false;
    if (!bytesMember(map, "sig", SignatureSize, &parsed.signature, error))
        return false;

    *pointer = parsed;
    return true;
}

bool sign(Pointer *pointer, const QByteArray &privateKey, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!pointer) {
        *error = QStringLiteral("no pointer to sign");
        return false;
    }
    if (privateKey.size() != PrivateKeySize) {
        *error = QStringLiteral("channel private key is not %1 bytes").arg(PrivateKeySize);
        return false;
    }

    QByteArray publicKey;
    if (!Crypto::ed25519PublicKey(privateKey, &publicKey)) {
        *error = QStringLiteral("could not derive the channel public key");
        return false;
    }
    pointer->publicKey = publicKey;

    const QByteArray body = encodeBody(*pointer);
    if (body.isEmpty()) {
        *error = QStringLiteral("channel pointer is not valid before signing");
        return false;
    }
    if (!Crypto::ed25519Sign(privateKey, body, &pointer->signature)) {
        *error = QStringLiteral("could not sign the channel pointer");
        return false;
    }
    return true;
}

bool verify(const Pointer &pointer, const QByteArray &expectedId, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (pointer.publicKey.size() != PublicKeySize
        || pointer.signature.size() != SignatureSize) {
        *error = QStringLiteral("channel pointer is missing its key or signature");
        return false;
    }
    if (!expectedId.isEmpty()) {
        if (expectedId.size() != IdSize) {
            *error = QStringLiteral("channel id is not %1 bytes").arg(IdSize);
            return false;
        }
        if (id(pointer.publicKey) != expectedId) {
            *error = QStringLiteral("channel pointer is signed by a different channel");
            return false;
        }
    }

    const QByteArray body = encodeBody(pointer);
    if (body.isEmpty()) {
        *error = QStringLiteral("channel pointer is not structurally valid");
        return false;
    }
    if (!Crypto::ed25519Verify(pointer.publicKey, body, pointer.signature)) {
        *error = QStringLiteral("channel pointer signature does not verify");
        return false;
    }
    return true;
}

bool verifyBytes(const QByteArray &bytes, const QByteArray &expectedId,
                 Pointer *pointer, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    Pointer parsed;
    if (!decode(bytes, &parsed, error))
        return false;
    if (!verify(parsed, expectedId, error))
        return false;
    if (pointer)
        *pointer = parsed;
    return true;
}

bool isNewer(const Pointer &candidate, const Pointer &current)
{
    if (candidate.sequence != current.sequence)
        return candidate.sequence > current.sequence;
    return candidate.timestamp > current.timestamp;
}

QByteArray key(const QByteArray &readSecret)
{
    if (readSecret.size() != ReadSecretSize)
        return QByteArray();
    QByteArray out;
    if (!Crypto::hkdfSha256(readSecret, QByteArray(Crypto::Sha256Size, '\0'),
                            QByteArrayLiteral("kelp/v1/channel"), Crypto::KeySize, &out))
        return QByteArray();
    return out;
}

QByteArray encodeUpdateBody(const Update &update)
{
    if (update.channelId.size() != IdSize
        || update.pointer.isEmpty()
        || update.publicKey.size() != PublicKeySize
        || update.sequence < 1
        || update.timestamp < 0)
        return QByteArray();

    QVector<CborPair> entries;
    entries.append(uintPair("v", Version));
    entries.append(bytesPair("channel", update.channelId));
    entries.append(bytesPair("pointer", update.pointer));
    entries.append(uintPair("seq", update.sequence));
    entries.append(intPair("ts", update.timestamp));
    entries.append(bytesPair("pub", update.publicKey));
    return Cbor::encodeMap(entries);
}

QByteArray encodeUpdate(const Update &update)
{
    if (update.signature.size() != SignatureSize)
        return QByteArray();
    const QByteArray body = encodeUpdateBody(update);
    if (body.isEmpty())
        return QByteArray();

    QVector<CborPair> entries;
    entries.append(uintPair("v", Version));
    entries.append(bytesPair("channel", update.channelId));
    entries.append(bytesPair("pointer", update.pointer));
    entries.append(uintPair("seq", update.sequence));
    entries.append(intPair("ts", update.timestamp));
    entries.append(bytesPair("pub", update.publicKey));
    entries.append(bytesPair("sig", update.signature));
    return Cbor::encodeMap(entries);
}

bool decodeUpdate(const QByteArray &bytes, Update *update, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!update) {
        *error = QStringLiteral("no update to fill");
        return false;
    }

    Cbor::Value map;
    if (!Cbor::decode(bytes, &map, error))
        return false;
    if (!map.isMap()) {
        *error = QStringLiteral("channel update is not a CBOR map");
        return false;
    }
    static const char *const bodyKeys[] = { "v", "channel", "pointer",
                                            "seq", "ts", "pub" };
    if (!checkKeys(map, bodyKeys, 6, true, "channel update", error))
        return false;

    Update parsed;
    quint64 version = 0;
    if (!integerMember(map, "v", &version, error))
        return false;
    if (version != Version) {
        *error = QStringLiteral("unsupported channel update version %1").arg(version);
        return false;
    }

    if (!bytesMember(map, "channel", IdSize, &parsed.channelId, error))
        return false;

    const Cbor::Value *pointer = map.member("pointer");
    if (!pointer || pointer->type != Cbor::Value::Bytes || pointer->bytes.isEmpty()) {
        *error = QStringLiteral("channel update has no pointer");
        return false;
    }
    parsed.pointer = pointer->bytes;

    if (!integerMember(map, "seq", &parsed.sequence, error))
        return false;
    if (parsed.sequence < 1) {
        *error = QStringLiteral("channel update has a zero sequence");
        return false;
    }

    const Cbor::Value *timestamp = map.member("ts");
    if (!timestamp || timestamp->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("channel update has no unsigned \"ts\"");
        return false;
    }
    if (timestamp->unsignedValue > quint64(std::numeric_limits<qint64>::max())) {
        *error = QStringLiteral("channel update timestamp is out of range");
        return false;
    }
    parsed.timestamp = qint64(timestamp->unsignedValue);

    if (!bytesMember(map, "pub", PublicKeySize, &parsed.publicKey, error))
        return false;
    if (!bytesMember(map, "sig", SignatureSize, &parsed.signature, error))
        return false;

    *update = parsed;
    return true;
}

bool signUpdate(Update *update, const QByteArray &privateKey, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!update) {
        *error = QStringLiteral("no update to sign");
        return false;
    }
    if (privateKey.size() != PrivateKeySize) {
        *error = QStringLiteral("channel private key is not %1 bytes").arg(PrivateKeySize);
        return false;
    }

    QByteArray publicKey;
    if (!Crypto::ed25519PublicKey(privateKey, &publicKey)) {
        *error = QStringLiteral("could not derive the channel public key");
        return false;
    }
    const QByteArray channelId = id(publicKey);
    if (channelId.isEmpty()) {
        *error = QStringLiteral("could not derive the channel id");
        return false;
    }
    update->publicKey = publicKey;
    update->channelId = channelId;

    const QByteArray body = encodeUpdateBody(*update);
    if (body.isEmpty()) {
        *error = QStringLiteral("channel update is not valid before signing");
        return false;
    }
    if (!Crypto::ed25519Sign(privateKey, body, &update->signature)) {
        *error = QStringLiteral("could not sign the channel update");
        return false;
    }
    return true;
}

bool verifyUpdate(const Update &update, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (update.publicKey.size() != PublicKeySize
        || update.channelId.size() != IdSize
        || update.signature.size() != SignatureSize
        || update.pointer.isEmpty()) {
        *error = QStringLiteral("channel update is missing a field");
        return false;
    }
    if (id(update.publicKey) != update.channelId) {
        *error = QStringLiteral("channel update is not for the key it names");
        return false;
    }

    const QByteArray body = encodeUpdateBody(update);
    if (body.isEmpty()) {
        *error = QStringLiteral("channel update is not structurally valid");
        return false;
    }
    if (!Crypto::ed25519Verify(update.publicKey, body, update.signature)) {
        *error = QStringLiteral("channel update signature does not verify");
        return false;
    }
    return true;
}

QByteArray seal(const Pointer &pointer, const QByteArray &readSecret, bool *ok)
{
    if (ok)
        *ok = false;
    if (pointer.signature.size() != SignatureSize)
        return QByteArray();

    const QByteArray channelKey = key(readSecret);
    const QByteArray body = encode(pointer);
    if (channelKey.isEmpty() || body.isEmpty())
        return QByteArray();

    bool sealed = false;
    const QByteArray objectBytes = Object::sealRandom(channelKey, body, &sealed);
    if (!sealed)
        return QByteArray();
    if (ok)
        *ok = true;
    return objectBytes;
}

bool open(const QByteArray &objectBytes, const QByteArray &readSecret,
          const QByteArray &expectedId, Pointer *pointer, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    const QByteArray channelKey = key(readSecret);
    if (channelKey.isEmpty()) {
        *error = QStringLiteral("channel read secret is not %1 bytes").arg(ReadSecretSize);
        return false;
    }

    QByteArray plaintext;
    if (!Object::open(channelKey, objectBytes, &plaintext, error))
        return false;
    if (!verifyBytes(plaintext, expectedId, pointer, error))
        return false;
    return true;
}

QString encodeLink(const Link &link)
{
    if (link.id.size() != IdSize || link.readSecret.size() != ReadSecretSize)
        return QString();

    QString text = QStringLiteral("kelpchannel://");
    text += QString::fromLatin1(Kelp::Link::base32Encode(link.id));

    QStringList query;
    for (int i = 0; i < link.relays.size(); ++i) {
        query.append(QStringLiteral("r=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(link.relays.at(i))));
    }
    if (!query.isEmpty())
        text += QLatin1Char('?') + query.join(QLatin1Char('&'));

    text += QStringLiteral("#k=") + Kelp::Link::base64UrlEncode(link.readSecret);
    return text;
}

bool decodeLink(const QString &text, Link *link, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!link) {
        *error = QStringLiteral("no destination for the channel link");
        return false;
    }

    const QString scheme = QStringLiteral("kelpchannel://");
    QString rest = text.trimmed();
    if (!rest.startsWith(scheme, Qt::CaseInsensitive)) {
        *error = QStringLiteral("link does not start with kelpchannel://");
        return false;
    }
    rest = rest.mid(scheme.size());

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

    Link parsed;
    if (!Kelp::Link::base32Decode(rest, &parsed.id, error))
        return false;
    if (parsed.id.size() != IdSize) {
        *error = QStringLiteral("channel id is not %1 bytes").arg(IdSize);
        return false;
    }

    bool haveSecret = false;
    const QStringList fragmentParts = fragment.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < fragmentParts.size(); ++i) {
        const QString &part = fragmentParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        if (part.left(equals) != QLatin1String("k"))
            continue;
        if (haveSecret) {
            *error = QStringLiteral("channel link has more than one key fragment");
            return false;
        }
        if (!Kelp::Link::base64UrlDecode(part.mid(equals + 1), &parsed.readSecret, error))
            return false;
        haveSecret = true;
    }
    if (!haveSecret) {
        *error = QStringLiteral("channel link has no key fragment");
        return false;
    }
    if (parsed.readSecret.size() != ReadSecretSize) {
        *error = QStringLiteral("channel read secret is not %1 bytes").arg(ReadSecretSize);
        return false;
    }

    const QStringList queryParts = query.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < queryParts.size(); ++i) {
        const QString &part = queryParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        if (part.left(equals) == QLatin1String("r")) {
            parsed.relays.append(QUrl::fromPercentEncoding(part.mid(equals + 1).toUtf8()));
        }
    }

    *link = parsed;
    return true;
}

} // namespace Channel
} // namespace Kelp
