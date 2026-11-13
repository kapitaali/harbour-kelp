#ifndef KELP_CHANNEL_H
#define KELP_CHANNEL_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace Kelp {
namespace Channel {

/*
 * A channel is a mutable signed pointer (WS-C4 §4).
 *
 * A bundle link names one immutable bundle. A channel link names a keypair, and
 * the pointer behind it says which bundle is current:
 *
 *   channel_id = SHA-256(public_key)          (32 bytes, base32 in a link)
 *   pointer    = { v, name, bundle, key, seq, ts, pub, sig }
 *
 * `key` is the current bundle's content key (the `#k=` of its own kelp:// link).
 * A recipient needs it to open the bundle, and it can only reach the recipient
 * inside the sealed pointer, so a channel link is a read capability for the
 * current bundle and nothing more. The relay never sees it.
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
    QByteArray bundleSecret; // 32 bytes: the content key of that bundle
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

// Structural decode: strict canonical CBOR, exactly the seven/eight known keys,
// known version, 32-byte bundle, key and public key, 64-byte signature. It does not
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

/*
 * The channel link (WS-C4 §4.1):
 *
 *   kelpchannel://<channel_id_b32>?r=<relay>[&r=…]#k=<read_secret>
 *
 * Like a bundle link, the fragment is a capability and never leaves the
 * recipient's device with the request; `r=` names relays that may hold the
 * pointer. The read secret decrypts pointers but cannot sign them, so passing
 * the link on gives read access, not publishing rights.
 */
struct Link {
    QByteArray id;            // 32 bytes, SHA-256(public key)
    QByteArray readSecret;    // 32 bytes
    QStringList relays;       // tried in order

    Link() {}
};

QString encodeLink(const Link &link);

// Strict about the scheme, the id and a single 32-byte `#k=`; unknown query
// parameters are ignored so a later addition does not make a link unreadable.
bool decodeLink(const QString &text, Link *link, QString *error);

/*
 * Relay registration (WS-C4 §4.4).
 *
 * A relay stores the sealed pointer but cannot read it, so it cannot check the
 * pointer's inner signature. This outer statement is what it can check: the
 * channel key signs the exact pointer bytes and a monotonic sequence.
 *
 *   update = { v, channel, seq, ts, pub, pointer, sig }
 *
 * `channel` must be SHA-256(pub), `pointer` is the sealed Kelp object to serve,
 * and `sig` is Ed25519 over the same map without `sig`. A relay keeps the
 * highest sequence it has seen per channel; a replay of an older or equal
 * update is ignored, so only the holder of the private key can move a channel
 * forward. The relay still learns no name, no bundle id and no key material
 * beyond the public key that is already implicit in the channel id.
 */
struct Update {
    QByteArray channelId;    // 32 bytes
    QByteArray pointer;      // the sealed pointer object, exactly as served
    quint64 sequence;
    qint64 timestamp;
    QByteArray publicKey;    // 32 bytes
    QByteArray signature;    // 64 bytes

    Update() : sequence(0), timestamp(0) {}
};

QByteArray encodeUpdateBody(const Update &update);
QByteArray encodeUpdate(const Update &update);

// Structural decode: strict canonical CBOR, exactly the seven known keys,
// version 1, 32-byte channel and public key, non-empty pointer, 64-byte
// signature. Does not verify the signature.
bool decodeUpdate(const QByteArray &bytes, Update *update, QString *error);

// Derives the public key and channel id from `privateKey`, then signs the body.
bool signUpdate(Update *update, const QByteArray &privateKey, QString *error);

// Verifies the channel-id binding and the signature.
bool verifyUpdate(const Update &update, QString *error);

} // namespace Channel
} // namespace Kelp

#endif // KELP_CHANNEL_H
