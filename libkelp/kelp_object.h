#ifndef KELP_OBJECT_H
#define KELP_OBJECT_H

#include <QByteArray>
#include <QString>

namespace Kelp {
namespace Object {

/*
 * A content object — the only noun in the protocol.
 *
 *   object = 0x01 || nonce[12] || AES-256-GCM(content_key, nonce, plaintext,
 *                                              aad = "kelp/v1/object")
 *
 * The plaintext is either a data chunk or a manifest; the object itself does
 * not say which, so the id is the whole address. object_id is the SHA-256 of
 * the full object bytes, i.e. of the ciphertext: a relay sees only random
 * bytes, and there is no known-plaintext confirmation oracle.
 */

enum {
    EnvelopeVersion = 1,
    NonceSize = 12,
    TagSize = 16,
    MaxObjectBytes = 1048576      // 1 MiB, envelope included
};

QByteArray aad();

// object_id = SHA-256 of the full object bytes.
QByteArray id(const QByteArray &objectBytes);

QByteArray seal(const QByteArray &contentKey, const QByteArray &plaintext,
                const QByteArray &nonce, bool *ok);

// Fresh random nonce per object — what production code uses.
QByteArray sealRandom(const QByteArray &contentKey, const QByteArray &plaintext,
                      bool *ok);

bool open(const QByteArray &contentKey, const QByteArray &objectBytes,
          QByteArray *plaintext, QString *error);

} // namespace Object
} // namespace Kelp

#endif // KELP_OBJECT_H
