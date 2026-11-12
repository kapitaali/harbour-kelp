#ifndef KELP_CRYPTO_H
#define KELP_CRYPTO_H

#include <QByteArray>

namespace Kelp {
namespace Crypto {

// The primitives Kelp is built on. Everything else is composition; nothing
// here is hand-rolled — it is thin glue over OpenSSL's libcrypto.

enum {
    Sha256Size = 32,
    KeySize = 32,
    NonceSize = 12,
    TagSize = 16
};

// Returns an empty QByteArray on failure.
QByteArray sha256(const QByteArray &data);

// Cryptographically secure random bytes; empty on failure.
QByteArray randomBytes(int count);

// HKDF-SHA256 (RFC 5869). `salt` may be empty, which is equivalent to 32 zero
// bytes (the RFC 5869 default) — see the unit tests.
bool hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                const QByteArray &info, int length, QByteArray *out);

// PBKDF2-HMAC-SHA256 (RFC 8018). Only used for passphrase links (design §12.3),
// where the point is that the derivation is *expensive*: the cost is the only
// thing standing between a passphrase and a stolen link, and it travels in the
// link so it can be raised later without breaking links already handed out.
// `iterations` must be at least 1; upper bounds are the caller's business (see
// kelp_link.h, which refuses an implausible count from a link).
bool pbkdf2Sha256(const QByteArray &passphrase, const QByteArray &salt,
                  int iterations, int length, QByteArray *out);

// HMAC-SHA256 (RFC 2104). Used to prove possession of the swarm token at
// tunnel setup (wire spec §8), and to prove that possession without putting the
// token itself on the wire.
bool hmacSha256(const QByteArray &key, const QByteArray &data, QByteArray *out);

// Ed25519 (RFC 8032). The private key is the 32-byte seed; the public key is
// derived from it. Only channels use it (WS-C4 §4): a bundle link is a
// capability, while a channel pointer is a signed statement that must verify
// with a public key the recipient got from the link.
enum {
    Ed25519PublicKeySize = 32,
    Ed25519PrivateKeySize = 32,
    Ed25519SignatureSize = 64
};

bool ed25519Generate(QByteArray *publicKey, QByteArray *privateKey);
bool ed25519PublicKey(const QByteArray &privateKey, QByteArray *publicKey);
bool ed25519Sign(const QByteArray &privateKey, const QByteArray &message,
                 QByteArray *signature);
bool ed25519Verify(const QByteArray &publicKey, const QByteArray &message,
                   const QByteArray &signature);

// AES-256-GCM. `out` is the ciphertext with the 16-byte tag appended.
// Decryption fails (returns false) if the tag does not verify, which is the
// only signal a caller needs.
bool aes256GcmEncrypt(const QByteArray &key, const QByteArray &nonce,
                      const QByteArray &plaintext, const QByteArray &aad,
                      QByteArray *out);

bool aes256GcmDecrypt(const QByteArray &key, const QByteArray &nonce,
                      const QByteArray &ciphertext, const QByteArray &aad,
                      QByteArray *out);

} // namespace Crypto
} // namespace Kelp

#endif // KELP_CRYPTO_H
