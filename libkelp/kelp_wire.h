#ifndef KELP_WIRE_H
#define KELP_WIRE_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Kelp {
namespace Wire {

/*
 * The Kelp tunnel wire format (kelp-swarm-wire-spec.md §7.1, §8, §9).
 *
 * Pure encoding and decoding — no sockets, no state beyond a byte buffer — so
 * it lives with the content layer and is unit-tested the same way. The app and
 * the relay both speak it; the TLS that carries it (TLS 1.3 + ALPN "kelp/1") is
 * the app layer's business, not this module's.
 *
 * Two things live here:
 *   * the frame:  length(u32, big-endian) || type(u8) || payload(CBOR), where
 *     `length` counts the type byte and the payload;
 *   * the messages: HELLO/AUTH (the §8 handshake) and the data-plane messages
 *     the relay-only path needs (WANT/DATA/REJECT and a PING/PONG keepalive).
 *
 * The swarm-only messages — HAVE_RANGES, HAVE, CANCEL, PEX — are §9 types too,
 * but nothing uses them until the swarm itself (C7), so they are deliberately
 * not here yet; adding them later is additive.
 *
 * Message structs carry a "Message" suffix so they do not collide with the
 * frame-type enumerators of the same name in this namespace.
 */

enum {
    // A frame longer than this is refused before it is buffered (spec §1.2).
    MaxFrameBytes = 4194304
};

// Frame type codes (spec §9). Values are the wire codes.
enum Type {
    Hello = 0x10,
    Auth = 0x11,
    Want = 0x14,
    Data = 0x15,
    Reject = 0x16,
    Ping = 0x20,
    Pong = 0x21,
    Error = 0x7f,
    // Tunnel setup, not a §9 swarm message: the publisher tells the relay which
    // bundle it will serve and the relay-scoped credential it may gate on. See
    // the relay handshake in kelp-swarm-wire-spec.md §17.
    Register = 0x30
};

// Error codes (spec §14), so the app and the relay name the same things.
enum ErrorCode {
    BadToken = 1,
    BadBundle = 2,
    VersionError = 3,
    BadData = 4,
    BadFrame = 5,
    NotFound = 6,
    RateLimited = 7,
    Internal = 8
};

// --- Framing ------------------------------------------------------------

// One whole frame. `payload` is the CBOR body.
QByteArray encodeFrame(quint8 type, const QByteArray &payload);

// Reassembles frames from a byte stream: TLS hands over arbitrary chunks, the
// wire hands over whole frames, and this is the seam. `push()` never fails on
// its own; `next()` yields frames as they complete.
class FrameReader
{
public:
    FrameReader() : m_failed(false) {}

    // Appends what arrived. A frame header claiming more than MaxFrameBytes
    // flags the reader as failed and drops the buffer.
    void push(const QByteArray &data);

    // True when a whole frame is available; fills `type` and `payload`.
    bool next(quint8 *type, QByteArray *payload);

    bool failed() const { return m_failed; }
    QString error() const { return m_error; }

private:
    QByteArray m_buffer;
    bool m_failed;
    QString m_error;
};

// --- Handshake (spec §8) ------------------------------------------------

//   mac = HMAC-SHA256(swarm_token, prefix || first_nonce || second_nonce || bundle_id)
//
// The initiator signs with prefix 'i' and its own nonce first; the responder
// answers with 'r' and the nonces in the opposite order. So holding the token
// is proved without ever sending it, and neither side can be replayed: the
// nonces are fresh per connection.
QByteArray handshakeMac(const QByteArray &swarmToken, char prefix,
                        const QByteArray &firstNonce, const QByteArray &secondNonce,
                        const QByteArray &bundleId);

// Compares two MACs in constant time.
bool macEquals(const QByteArray &a, const QByteArray &b);

// --- Messages (spec §9) -------------------------------------------------

struct HelloMessage {
    quint64 version;
    QByteArray bundleId;      // object id of the root manifest
    QByteArray peerId;        // SHA-256 of the sender's static key; a name, not trust
    QByteArray nonce;         // 4..16 bytes, fresh per connection

    HelloMessage() : version(1) {}
};

struct AuthMessage {
    QByteArray mac;
};

struct WantMessage {
    QByteArray hash;          // object id being asked for
};

struct DataMessage {
    quint64 index;            // canonical index, for the receiver's map
    QByteArray hash;          // object id
    QByteArray bytes;         // the object envelope, exactly as stored

    DataMessage() : index(0) {}
};

struct RejectMessage {
    quint64 why;              // an ErrorCode from spec §14
    QByteArray hash;

    RejectMessage() : why(0) {}
};

struct PingMessage {
    quint64 t;                // sender's clock, echoed back

    PingMessage() : t(0) {}
};

struct ErrorMessage {
    quint64 code;             // spec §14
    QString message;

    ErrorMessage() : code(0) {}
};

// The publisher's tunnel registration: "I am the origin for `bundleId`, and the
// token recipients will present for it is `token`." `token` is the relay-scoped
// token (HKDF of the link secret with "kelp/v1/relay"), never the swarm token or
// the content key, so a relay that is compromised or curious learns only its own
// credential. Sent inside TLS.
struct RegisterMessage {
    QByteArray bundleId;
    QByteArray token;
    // The shape, so the relay can answer /info without holding the manifests
    // (it never has the content key). These are the sender's claims; the
    // recipient still trusts only the objects it verifies.
    quint64 objects;
    quint64 files;
    quint64 bytes;

    RegisterMessage() : objects(0), files(0), bytes(0) {}
};

QByteArray encodeHello(const HelloMessage &hello);
QByteArray encodeAuth(const AuthMessage &auth);
QByteArray encodeWant(const WantMessage &want);
QByteArray encodeData(const DataMessage &data);
QByteArray encodeReject(const RejectMessage &reject);
QByteArray encodePing(quint64 t);
QByteArray encodeError(const ErrorMessage &error);
QByteArray encodeRegister(const RegisterMessage &registration);

bool decodeHello(const QByteArray &payload, HelloMessage *hello, QString *error);
bool decodeAuth(const QByteArray &payload, AuthMessage *auth, QString *error);
bool decodeWant(const QByteArray &payload, WantMessage *want, QString *error);
bool decodeData(const QByteArray &payload, DataMessage *data, QString *error);
bool decodeReject(const QByteArray &payload, RejectMessage *reject, QString *error);
bool decodePing(const QByteArray &payload, quint64 *t, QString *error);
bool decodeError(const QByteArray &payload, ErrorMessage *error, QString *errorOut);
bool decodeRegister(const QByteArray &payload, RegisterMessage *registration, QString *error);

} // namespace Wire
} // namespace Kelp

#endif // KELP_WIRE_H
