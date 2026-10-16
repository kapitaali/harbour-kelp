#include "kelp_wire.h"

#include <QPair>

#include "kelp_cbor.h"
#include "kelp_crypto.h"

namespace Kelp {
namespace Wire {

namespace {

quint32 readBigEndian32(const char *bytes)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(bytes);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

void appendBigEndian32(QByteArray *out, quint32 value)
{
    out->append(char((value >> 24) & 0xff));
    out->append(char((value >> 16) & 0xff));
    out->append(char((value >> 8) & 0xff));
    out->append(char(value & 0xff));
}

// --- CBOR field helpers -------------------------------------------------

bool needUnsigned(const Cbor::Value &map, const char *key, quint64 *out, QString *error,
                  const char *what)
{
    const Cbor::Value *value = map.member(key);
    if (!value || value->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("%1 is missing \"%2\"").arg(QString::fromLatin1(what),
                                                           QString::fromLatin1(key));
        return false;
    }
    *out = value->unsignedValue;
    return true;
}

bool needBytes(const Cbor::Value &map, const char *key, QByteArray *out, QString *error,
               const char *what)
{
    const Cbor::Value *value = map.member(key);
    if (!value || value->type != Cbor::Value::Bytes) {
        *error = QStringLiteral("%1 is missing \"%2\"").arg(QString::fromLatin1(what),
                                                           QString::fromLatin1(key));
        return false;
    }
    *out = value->bytes;
    return true;
}

// Parses a payload as a canonical map, or explains why it is not one.
bool asMap(const QByteArray &payload, Cbor::Value *out, QString *error, const char *what)
{
    QString reason;
    if (!Cbor::decode(payload, out, &reason)) {
        *error = QStringLiteral("%1 is not canonical CBOR: %2")
            .arg(QString::fromLatin1(what), reason);
        return false;
    }
    if (!out->isMap()) {
        *error = QStringLiteral("%1 is not a map").arg(QString::fromLatin1(what));
        return false;
    }
    return true;
}

QPair<QByteArray, QByteArray> hashPair(const char *key, const QByteArray &value)
{
    return QPair<QByteArray, QByteArray>(Cbor::encodeText(QString::fromLatin1(key)),
                                         Cbor::encodeBytes(value));
}

QPair<QByteArray, QByteArray> uintPair(const char *key, quint64 value)
{
    return QPair<QByteArray, QByteArray>(Cbor::encodeText(QString::fromLatin1(key)),
                                         Cbor::encodeUnsigned(value));
}

} // namespace

// --- Framing ------------------------------------------------------------

QByteArray encodeFrame(quint8 type, const QByteArray &payload)
{
    const quint32 length = quint32(1 + payload.size());
    QByteArray out;
    out.reserve(int(length) + 4);
    appendBigEndian32(&out, length);
    out.append(char(type));
    out.append(payload);
    return out;
}

void FrameReader::push(const QByteArray &data)
{
    if (m_failed || data.isEmpty())
        return;
    m_buffer.append(data);

    // Reject an oversized frame as soon as its header is readable, rather than
    // buffering up to 4 GiB because a peer said so.
    if (m_buffer.size() >= 4) {
        const quint32 length = readBigEndian32(m_buffer.constData());
        if (length == 0 || length > quint32(MaxFrameBytes)) {
            m_failed = true;
            m_error = QStringLiteral("a frame declared %1 bytes, which is not allowed")
                .arg(length);
            m_buffer.clear();
        }
    }
}

bool FrameReader::next(quint8 *type, QByteArray *payload)
{
    if (m_failed || m_buffer.size() < 4)
        return false;

    const quint32 length = readBigEndian32(m_buffer.constData());
    if (length == 0 || length > quint32(MaxFrameBytes)) {
        m_failed = true;
        m_error = QStringLiteral("a frame declared %1 bytes, which is not allowed").arg(length);
        m_buffer.clear();
        return false;
    }
    if (quint32(m_buffer.size()) < 4 + length)
        return false;                        // the rest is still in flight

    *type = quint8(m_buffer.at(4));
    *payload = m_buffer.mid(5, int(length) - 1);
    m_buffer.remove(0, int(4 + length));
    return true;
}

// --- Handshake ----------------------------------------------------------

QByteArray handshakeMac(const QByteArray &swarmToken, char prefix,
                        const QByteArray &firstNonce, const QByteArray &secondNonce,
                        const QByteArray &bundleId)
{
    QByteArray data;
    data.append(prefix);
    data.append(firstNonce);
    data.append(secondNonce);
    data.append(bundleId);

    QByteArray mac;
    if (!Crypto::hmacSha256(swarmToken, data, &mac))
        return QByteArray();
    return mac;
}

bool macEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return difference == 0;
}

// --- Messages -----------------------------------------------------------

QByteArray encodeHello(const HelloMessage &hello)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(uintPair("v", hello.version));
    map.append(hashPair("bid", hello.bundleId));
    map.append(hashPair("pid", hello.peerId));
    map.append(hashPair("nonce", hello.nonce));
    return Cbor::encodeMap(map);
}

bool decodeHello(const QByteArray &payload, HelloMessage *hello, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "HELLO"))
        return false;
    if (!needUnsigned(map, "v", &hello->version, error, "HELLO"))
        return false;
    if (!needBytes(map, "bid", &hello->bundleId, error, "HELLO"))
        return false;
    if (!needBytes(map, "pid", &hello->peerId, error, "HELLO"))
        return false;
    if (!needBytes(map, "nonce", &hello->nonce, error, "HELLO"))
        return false;
    if (hello->bundleId.size() != Crypto::Sha256Size || hello->peerId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("HELLO ids must be 32-byte hashes");
        return false;
    }
    if (hello->nonce.size() < 4 || hello->nonce.size() > 16) {
        *error = QStringLiteral("HELLO nonce must be 4..16 bytes");
        return false;
    }
    return true;
}

QByteArray encodeAuth(const AuthMessage &auth)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(hashPair("mac", auth.mac));
    return Cbor::encodeMap(map);
}

bool decodeAuth(const QByteArray &payload, AuthMessage *auth, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "AUTH"))
        return false;
    if (!needBytes(map, "mac", &auth->mac, error, "AUTH"))
        return false;
    if (auth->mac.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("AUTH mac must be a 32-byte HMAC");
        return false;
    }
    return true;
}

QByteArray encodeWant(const WantMessage &want)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(hashPair("h", want.hash));
    return Cbor::encodeMap(map);
}

bool decodeWant(const QByteArray &payload, WantMessage *want, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "WANT"))
        return false;
    if (!needBytes(map, "h", &want->hash, error, "WANT"))
        return false;
    if (want->hash.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("WANT hash must be a 32-byte object id");
        return false;
    }
    return true;
}

QByteArray encodeData(const DataMessage &data)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(uintPair("i", data.index));
    map.append(hashPair("h", data.hash));
    map.append(hashPair("b", data.bytes));
    return Cbor::encodeMap(map);
}

bool decodeData(const QByteArray &payload, DataMessage *data, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "DATA"))
        return false;
    if (!needUnsigned(map, "i", &data->index, error, "DATA"))
        return false;
    if (!needBytes(map, "h", &data->hash, error, "DATA"))
        return false;
    if (!needBytes(map, "b", &data->bytes, error, "DATA"))
        return false;
    if (data->hash.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("DATA hash must be a 32-byte object id");
        return false;
    }
    return true;
}

QByteArray encodeReject(const RejectMessage &reject)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(hashPair("h", reject.hash));
    map.append(uintPair("why", reject.why));
    return Cbor::encodeMap(map);
}

bool decodeReject(const QByteArray &payload, RejectMessage *reject, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "REJECT"))
        return false;
    if (!needBytes(map, "h", &reject->hash, error, "REJECT"))
        return false;
    if (!needUnsigned(map, "why", &reject->why, error, "REJECT"))
        return false;
    return true;
}

QByteArray encodePing(quint64 t)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(uintPair("t", t));
    return Cbor::encodeMap(map);
}

bool decodePing(const QByteArray &payload, quint64 *t, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "PING/PONG"))
        return false;
    return needUnsigned(map, "t", t, error, "PING/PONG");
}

QByteArray encodeError(const ErrorMessage &error)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(uintPair("e", error.code));
    map.append(QPair<QByteArray, QByteArray>(Cbor::encodeText(QStringLiteral("msg")),
                                             Cbor::encodeText(error.message)));
    return Cbor::encodeMap(map);
}

bool decodeError(const QByteArray &payload, ErrorMessage *error, QString *errorOut)
{
    Cbor::Value map;
    if (!asMap(payload, &map, errorOut, "ERROR"))
        return false;
    if (!needUnsigned(map, "e", &error->code, errorOut, "ERROR"))
        return false;
    const Cbor::Value *message = map.member("msg");
    if (message && message->type == Cbor::Value::Text)
        error->message = message->text;
    return true;
}

QByteArray encodeRegister(const RegisterMessage &registration)
{
    QVector<QPair<QByteArray, QByteArray> > map;
    map.append(uintPair("b", registration.bytes));
    map.append(hashPair("bid", registration.bundleId));
    map.append(uintPair("f", registration.files));
    map.append(uintPair("o", registration.objects));
    map.append(hashPair("tok", registration.token));
    return Cbor::encodeMap(map);
}

bool decodeRegister(const QByteArray &payload, RegisterMessage *registration, QString *error)
{
    Cbor::Value map;
    if (!asMap(payload, &map, error, "REGISTER"))
        return false;
    if (!needBytes(map, "bid", &registration->bundleId, error, "REGISTER"))
        return false;
    if (!needBytes(map, "tok", &registration->token, error, "REGISTER"))
        return false;
    if (!needUnsigned(map, "o", &registration->objects, error, "REGISTER"))
        return false;
    if (!needUnsigned(map, "f", &registration->files, error, "REGISTER"))
        return false;
    if (!needUnsigned(map, "b", &registration->bytes, error, "REGISTER"))
        return false;
    if (registration->bundleId.size() != Crypto::Sha256Size
        || registration->token.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("REGISTER ids and token must be 32 bytes");
        return false;
    }
    return true;
}

} // namespace Wire
} // namespace Kelp
