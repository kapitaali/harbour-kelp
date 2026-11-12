#ifndef KELP_CHANNEL_H
#define KELP_CHANNEL_H

#include <QByteArray>
#include <QString>

namespace Kelp {
namespace Channel {

/*
 * A channel is a mutable signed pointer (WS-C4 §4).
 *
 * A bundle link names one immutable bundle. A channel link names a keypair, and
 * the pointer behind it says which bundle is current:
 *
 *   channel_id = SHA-256(public_key)          (32 bytes, base32 in a link)
 *   pointer    = { v, name, bundle, seq, ts, pub, sig }
 *
 * The pointer is canonical CBOR. Its signature is Ed25519 over the same map
 * *without* `sig`, with the channel's private key. The public key travels in
 * the pointer because channel_id is a hash and cannot give it back; a recipient
 * checks SHA-256(pub) == channel_id before it trusts anything the pointer says.
 *
 * On the wire the pointer is a Kelp object: the plaintext map is sealed under
 *   channel_key = HKDF-SHA256(read_secret, salt=∅, info="kelp/v1/channel")
 * so a relay stores ciphertext and learns neither the name nor the bundle it
 * points at. The read secret is the channel link's `#k=`, never leaves the
 * publisher except through the link, and is not the signing key: a recipient
 * can read a channel but cannot update it.
 *
 * The signature covers `seq` and `ts`, so a relay (or a steward) cannot roll a
 * pointer back or rename it without the private key. Replay of an *older* valid
 * pointer is the caller's policy to refuse: see isNewer().
 */

enum {
    PublicKeySize = 32,
    PrivateKeySize = 32,
    SignatureSize = 64,
    IdSize = 32,
    ReadSecretSize = 32,
    Version = 1
};

struct KeyPair {
    QByteArray publicKey;
    QByteArray privateKey;

    KeyPair() {}
};

// A fresh channel identity. Ed25519 keys are 32-byte seeds; `privateKey` must
// never be written to a log or a link.
bool generateKeyPair(KeyPair *keys, QString *error);

// channel_id = SHA-256(publicKey). Empty for a key of the wrong size.
QByteArray id(const QByteArray &publicKey);

struct Pointer {
    quint64 version;
    QString name;
    QByteArray bundleId;     // 32 bytes: the root object id of the current bundle
    quint64 sequence;        // 1 for the first pointer, then strictly increasing
    qint64 timestamp;        // Unix seconds, set by the publisher
    QByteArray publicKey;    // 32 bytes, the signer's
    QByteArray signature;    // 64 bytes, over the map without this field

    Pointer() : version(Version), sequence(0), timestamp(0) {}
};

// Canonical CBOR of the pointer without `sig` — the bytes the signature covers.
QByteArray encodeBody(const Pointer &pointer);

// Canonical CBOR of the whole pointer, `sig` included.
QByteArray encode(const Pointer &pointer);

// Structural decode: strict canonical CBOR, exactly the six/seven known keys,
// known version, 32-byte bundle and public key, 64-byte signature. It does not
// verify the signature; call verify() for that.
bool decode(const QByteArray &bytes, Pointer *pointer, QString *error);

// Signs the pointer body with `privateKey` and stores the public key and the
// signature in the pointer. The public key is derived from the private key, so
// the two cannot drift apart.
bool sign(Pointer *pointer, const QByteArray &privateKey, QString *error);

// Verifies the signature and, when `expectedId` is not empty, that the pointer's
// public key hashes to it. A pointer with no signature, a wrong key or a wrong
// channel id fails with a reason.
bool verify(const Pointer &pointer, const QByteArray &expectedId, QString *error);

// decode() + verify() in one step.
bool verifyBytes(const QByteArray &bytes, const QByteArray &expectedId,
                 Pointer *pointer, QString *error);

// Whether `candidate` should replace `current`: a strictly higher sequence, or
// the same sequence with a later timestamp (a re-publish of the same update).
// Equal or older pointers are refused, which is the rollback rule.
bool isNewer(const Pointer &candidate, const Pointer &current);

// The pointer as a Kelp object, sealed under the channel read secret. The
// object id (Object::id) is what a relay maps channel_id to. Returns an empty
// QByteArray on failure and sets `ok`.
QByteArray seal(const Pointer &pointer, const QByteArray &readSecret, bool *ok);

// Opens and verifies a sealed pointer. `expectedId` is the channel id from the
// link; pass an empty one only when the caller already has it from elsewhere.
bool open(const QByteArray &objectBytes, const QByteArray &readSecret,
          const QByteArray &expectedId, Pointer *pointer, QString *error);

// The channel key derivation, exposed for tests and for a relay that must not
// have it. Empty when the secret is the wrong size.
QByteArray key(const QByteArray &readSecret);

} // namespace Channel
} // namespace Kelp

#endif // KELP_CHANNEL_H
