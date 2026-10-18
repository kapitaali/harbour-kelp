#include "kelp_crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace Kelp {
namespace Crypto {

QByteArray sha256(const QByteArray &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(data.constData(), size_t(data.size()), digest, &length, EVP_sha256(), 0) != 1)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(digest), int(length));
}

QByteArray randomBytes(int count)
{
    if (count <= 0)
        return QByteArray();
    QByteArray out(count, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), count) != 1)
        return QByteArray();
    return out;
}

bool hkdfSha256(const QByteArray &ikm, const QByteArray &salt,
                const QByteArray &info, int length, QByteArray *out)
{
    if (!out)
        return false;
    // RFC 5869 caps the output at 255 * HashLen for SHA-256.
    if (length <= 0 || length > 255 * Sha256Size)
        return false;

    EVP_KDF *kdf = EVP_KDF_fetch(0, "HKDF", 0);
    if (!kdf)
        return false;
    EVP_KDF_CTX *context = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!context)
        return false;

    char digest[] = "SHA256";
    OSSL_PARAM params[5];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0);
    params[1] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_KEY, const_cast<char *>(ikm.constData()), size_t(ikm.size()));
    params[2] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_SALT, const_cast<char *>(salt.constData()), size_t(salt.size()));
    params[3] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_INFO, const_cast<char *>(info.constData()), size_t(info.size()));
    params[4] = OSSL_PARAM_construct_end();

    out->resize(length);
    const int result = EVP_KDF_derive(context, reinterpret_cast<unsigned char *>(out->data()),
                                      size_t(length), params);
    EVP_KDF_CTX_free(context);
    if (result != 1) {
        out->clear();
        return false;
    }
    return true;
}

bool pbkdf2Sha256(const QByteArray &passphrase, const QByteArray &salt,
                  int iterations, int length, QByteArray *out)
{
    if (!out || iterations < 1 || length <= 0)
        return false;

    QByteArray result(length, '\0');
    if (PKCS5_PBKDF2_HMAC(passphrase.constData(), passphrase.size(),
                          reinterpret_cast<const unsigned char *>(salt.constData()),
                          salt.size(), iterations, EVP_sha256(), length,
                          reinterpret_cast<unsigned char *>(result.data())) != 1)
        return false;

    *out = result;
    return true;
}

bool hmacSha256(const QByteArray &key, const QByteArray &data, QByteArray *out)
{
    if (!out)
        return false;

    EVP_MAC *mac = EVP_MAC_fetch(0, "HMAC", 0);
    if (!mac)
        return false;
    EVP_MAC_CTX *context = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!context)
        return false;

    char digest[] = "SHA256";
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0);
    params[1] = OSSL_PARAM_construct_end();

    unsigned char result[EVP_MAX_MD_SIZE];
    size_t length = 0;
    bool ok = EVP_MAC_init(context,
                           reinterpret_cast<const unsigned char *>(key.constData()),
                           size_t(key.size()), params) == 1
        && EVP_MAC_update(context, reinterpret_cast<const unsigned char *>(data.constData()),
                          size_t(data.size())) == 1
        && EVP_MAC_final(context, result, &length, sizeof(result)) == 1;

    EVP_MAC_CTX_free(context);
    if (!ok)
        return false;

    *out = QByteArray(reinterpret_cast<const char *>(result), int(length));
    return true;
}

bool aes256GcmEncrypt(const QByteArray &key, const QByteArray &nonce,
                      const QByteArray &plaintext, const QByteArray &aad,
                      QByteArray *out)
{
    if (!out || key.size() != KeySize || nonce.size() != NonceSize)
        return false;

    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context)
        return false;

    QByteArray result(plaintext.size() + TagSize, '\0');
    QByteArray tag(TagSize, '\0');
    int written = 0;
    int total = 0;
    bool ok = false;

    do {
        if (EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), 0, 0, 0) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), 0) != 1)
            break;
        if (EVP_EncryptInit_ex(context, 0, 0,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
            break;
        if (!aad.isEmpty()
            && EVP_EncryptUpdate(context, 0, &written,
                                 reinterpret_cast<const unsigned char *>(aad.constData()),
                                 aad.size()) != 1)
            break;
        if (!plaintext.isEmpty()) {
            if (EVP_EncryptUpdate(context, reinterpret_cast<unsigned char *>(result.data()),
                                  &written,
                                  reinterpret_cast<const unsigned char *>(plaintext.constData()),
                                  plaintext.size()) != 1)
                break;
            total = written;
        }
        if (EVP_EncryptFinal_ex(context, reinterpret_cast<unsigned char *>(result.data()) + total,
                                &written) != 1)
            break;
        total += written;
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, TagSize, tag.data()) != 1)
            break;
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(context);
    if (!ok)
        return false;

    result.resize(total);
    result.append(tag);
    *out = result;
    return true;
}

bool aes256GcmDecrypt(const QByteArray &key, const QByteArray &nonce,
                      const QByteArray &ciphertext, const QByteArray &aad,
                      QByteArray *out)
{
    if (!out || key.size() != KeySize || nonce.size() != NonceSize
        || ciphertext.size() < TagSize)
        return false;

    const QByteArray body = ciphertext.left(ciphertext.size() - TagSize);
    const QByteArray tag = ciphertext.right(TagSize);

    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context)
        return false;

    QByteArray result(body.size(), '\0');
    int written = 0;
    int total = 0;
    bool ok = false;

    do {
        if (EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), 0, 0, 0) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), 0) != 1)
            break;
        if (EVP_DecryptInit_ex(context, 0, 0,
                               reinterpret_cast<const unsigned char *>(key.constData()),
                               reinterpret_cast<const unsigned char *>(nonce.constData())) != 1)
            break;
        if (!aad.isEmpty()
            && EVP_DecryptUpdate(context, 0, &written,
                                 reinterpret_cast<const unsigned char *>(aad.constData()),
                                 aad.size()) != 1)
            break;
        if (!body.isEmpty()) {
            if (EVP_DecryptUpdate(context, reinterpret_cast<unsigned char *>(result.data()),
                                  &written,
                                  reinterpret_cast<const unsigned char *>(body.constData()),
                                  body.size()) != 1)
                break;
            total = written;
        }
        // The tag is only checked in Final, so a mismatch means no plaintext.
        if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, TagSize,
                                const_cast<char *>(tag.constData())) != 1)
            break;
        if (EVP_DecryptFinal_ex(context, reinterpret_cast<unsigned char *>(result.data()) + total,
                                &written) != 1)
            break;
        total += written;
        result.resize(total);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(context);
    if (!ok)
        return false;

    *out = result;
    return true;
}

} // namespace Crypto
} // namespace Kelp
