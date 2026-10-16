#include "kelp_object.h"

#include "kelp_crypto.h"

namespace Kelp {
namespace Object {

namespace {
const char kAadBytes[] = "kelp/v1/object";
}

QByteArray aad()
{
    return QByteArray::fromRawData(kAadBytes, int(sizeof(kAadBytes) - 1));
}

QByteArray id(const QByteArray &objectBytes)
{
    return Crypto::sha256(objectBytes);
}

QByteArray seal(const QByteArray &contentKey, const QByteArray &plaintext,
                const QByteArray &nonce, bool *ok)
{
    if (ok)
        *ok = false;
    if (nonce.size() != NonceSize)
        return QByteArray();
    if (plaintext.size() > MaxObjectBytes - 1 - NonceSize - TagSize)
        return QByteArray();

    QByteArray ciphertext;
    if (!Crypto::aes256GcmEncrypt(contentKey, nonce, plaintext, aad(), &ciphertext))
        return QByteArray();

    QByteArray object;
    object.reserve(1 + NonceSize + ciphertext.size());
    object.append(char(EnvelopeVersion));
    object.append(nonce);
    object.append(ciphertext);

    if (ok)
        *ok = true;
    return object;
}

QByteArray sealRandom(const QByteArray &contentKey, const QByteArray &plaintext,
                      bool *ok)
{
    if (ok)
        *ok = false;
    const QByteArray nonce = Crypto::randomBytes(NonceSize);
    if (nonce.size() != NonceSize)
        return QByteArray();
    return seal(contentKey, plaintext, nonce, ok);
}

bool open(const QByteArray &contentKey, const QByteArray &objectBytes,
          QByteArray *plaintext, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!plaintext) {
        *error = QStringLiteral("no destination for the plaintext");
        return false;
    }
    if (objectBytes.size() < 1 + NonceSize + TagSize) {
        *error = QStringLiteral("object is too short");
        return false;
    }
    if (objectBytes.size() > MaxObjectBytes) {
        *error = QStringLiteral("object exceeds the 1 MiB cap");
        return false;
    }
    if (quint8(objectBytes.at(0)) != EnvelopeVersion) {
        *error = QStringLiteral("unsupported object envelope version");
        return false;
    }

    const QByteArray nonce = objectBytes.mid(1, NonceSize);
    const QByteArray ciphertext = objectBytes.mid(1 + NonceSize);
    if (!Crypto::aes256GcmDecrypt(contentKey, nonce, ciphertext, aad(), plaintext)) {
        *error = QStringLiteral("object failed to decrypt (wrong key or corrupt data)");
        return false;
    }
    return true;
}

} // namespace Object
} // namespace Kelp
