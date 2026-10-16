#include "kelptls.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QTcpSocket>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace {

// The ALPN protocol family, in the wire form ALPN uses: one length byte then
// the bytes. "kelp/1" is 6 bytes long. This is the value that pins the protocol
// version — see wire spec §1.2 (TLS_ALPN) and §16.
const char kAlpnName[] = "kelp/1";
const int kAlpnNameLength = 6;
const unsigned char kAlpnWire[] = { 6, 'k', 'e', 'l', 'p', '/', '1' };

// How much of a record we are willing to move in one call.
const int kTransferChunk = 16 * 1024;

// The relay is chosen by ALPN, so a peer that does not offer "kelp/1" is not a
// Kelp peer — refuse the connection rather than negotiate something else.
int alpnSelect(SSL *, const unsigned char **out, unsigned char *outlen,
               const unsigned char *in, unsigned int inlen, void *)
{
    const int result = SSL_select_next_proto(
        const_cast<unsigned char **>(out), outlen,
        kAlpnWire, int(sizeof(kAlpnWire)), in, inlen);
    if (result == OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_OK;
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

} // namespace

KelpTls::KelpTls(QObject *parent)
    : QObject(parent),
      m_mode(Client),
      m_socket(nullptr),
      m_ctx(nullptr),
      m_ssl(nullptr),
      m_rbio(nullptr),
      m_wbio(nullptr),
      m_handshakeComplete(false),
      m_closed(false),
      m_closedNotified(false),
      m_failed(false)
{
}

KelpTls::~KelpTls()
{
    // SSL_free releases the BIOs it was given.
    if (m_ssl)
        SSL_free(m_ssl);
    if (m_ctx)
        SSL_CTX_free(m_ctx);
}

bool KelpTls::setCertificate(const QByteArray &certificatePem, const QByteArray &keyPem,
                             QString *error)
{
    if (certificatePem.isEmpty() || keyPem.isEmpty()) {
        if (error)
            *error = QStringLiteral("a server certificate and key are both required");
        return false;
    }
    m_certificatePem = certificatePem;
    m_keyPem = keyPem;
    return true;
}

QString KelpTls::opensslError(const QString &prefix) const
{
    QString detail;
    unsigned long code = ERR_get_error();
    while (code != 0) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!detail.isEmpty())
            detail += QStringLiteral("; ");
        detail += QString::fromLatin1(buffer);
        code = ERR_get_error();
    }
    return detail.isEmpty() ? prefix : prefix + QStringLiteral(": ") + detail;
}

bool KelpTls::start(Mode mode, QTcpSocket *socket, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    if (m_ssl) {
        *error = QStringLiteral("this session has already been started");
        return false;
    }
    if (!socket) {
        *error = QStringLiteral("no socket to speak TLS over");
        return false;
    }

    m_mode = mode;
    m_socket = socket;
    m_failed = false;

    m_ctx = SSL_CTX_new(mode == Client ? TLS_client_method() : TLS_server_method());
    if (!m_ctx) {
        *error = opensslError(QStringLiteral("could not create a TLS context"));
        return false;
    }

    // Pinned to 1.3 on both ends: this is the transport the spec asks for, and
    // it is why the tunnel does not go through Qt's TLS.
    SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(m_ctx, TLS1_3_VERSION);

    // Self-signed and unverified on purpose (§7.1): the certificate carries no
    // trust, content is hash-verified, and a CA would be a central party the
    // whole design avoids.
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_NONE, nullptr);

    if (mode == Server) {
        SSL_CTX_set_alpn_select_cb(m_ctx, alpnSelect, nullptr);

        if (m_certificatePem.isEmpty() || m_keyPem.isEmpty()) {
            *error = QStringLiteral("the server needs a certificate and key before it starts");
            return false;
        }
        BIO *certBio = BIO_new_mem_buf(m_certificatePem.constData(), m_certificatePem.size());
        X509 *certificate = certBio ? PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr) : nullptr;
        BIO_free(certBio);
        BIO *keyBio = BIO_new_mem_buf(m_keyPem.constData(), m_keyPem.size());
        EVP_PKEY *key = keyBio ? PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr) : nullptr;
        BIO_free(keyBio);
        const bool loaded = certificate && key
            && SSL_CTX_use_certificate(m_ctx, certificate) == 1
            && SSL_CTX_use_PrivateKey(m_ctx, key) == 1;
        if (certificate)
            X509_free(certificate);
        if (key)
            EVP_PKEY_free(key);
        if (!loaded) {
            *error = opensslError(QStringLiteral("could not load the server certificate"));
            return false;
        }
    }

    m_ssl = SSL_new(m_ctx);
    if (!m_ssl) {
        *error = opensslError(QStringLiteral("could not create a TLS session"));
        return false;
    }

    // Memory BIOs, not the socket fd: Qt keeps owning the connection, and the
    // two never read the same descriptor.
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    if (!m_rbio || !m_wbio) {
        *error = opensslError(QStringLiteral("could not create the TLS buffers"));
        return false;
    }
    SSL_set_bio(m_ssl, m_rbio, m_wbio);

    if (mode == Client) {
        if (SSL_set_alpn_protos(m_ssl, kAlpnWire, int(sizeof(kAlpnWire))) != 0) {
            *error = opensslError(QStringLiteral("could not offer ALPN \"kelp/1\""));
            return false;
        }
        SSL_set_connect_state(m_ssl);
    } else {
        SSL_set_accept_state(m_ssl);
    }

    connect(m_socket, &QTcpSocket::readyRead, this, &KelpTls::onSocketReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten, this, &KelpTls::onSocketBytesWritten);
    connect(m_socket, &QTcpSocket::disconnected, this, &KelpTls::onSocketDisconnected);
    connect(m_socket,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, &KelpTls::onSocketError);

    // Whatever the socket already buffered before we took it over.
    const QByteArray early = m_socket->readAll();
    if (!early.isEmpty())
        BIO_write(m_rbio, early.constData(), early.size());

    // Kick off the handshake (the client emits its ClientHello here).
    pump();
    return !m_failed;
}

QString KelpTls::protocolVersion() const
{
    if (!m_ssl || !m_handshakeComplete)
        return QString();
    return QString::fromLatin1(SSL_get_version(m_ssl));
}

QByteArray KelpTls::readAll()
{
    const QByteArray out = m_in;
    m_in.clear();
    return out;
}

qint64 KelpTls::write(const QByteArray &data)
{
    if (!m_ssl || m_failed || !m_handshakeComplete)
        return -1;
    m_pending.append(data);
    writePending();
    return data.size();
}

void KelpTls::close()
{
    if (!m_socket)
        return;
    if (m_ssl && m_handshakeComplete && !m_closed) {
        // Best-effort close_notify; the peer may already be gone.
        SSL_shutdown(m_ssl);
        flushBio();
    }
    m_socket->disconnectFromHost();
}

void KelpTls::onSocketReadyRead()
{
    if (m_failed)
        return;
    const QByteArray incoming = m_socket->readAll();
    if (!incoming.isEmpty() && m_rbio)
        BIO_write(m_rbio, incoming.constData(), incoming.size());
    pump();
}

void KelpTls::onSocketBytesWritten()
{
    if (m_failed)
        return;
    writePending();
}

// closed() is emitted from two places (a TLS close_notify seen mid-pump, and the
// socket going away) and must be seen once. Handlers routinely delete their
// session on it, so a second emission would be a use-after-free.
void KelpTls::notifyClosed()
{
    if (m_closedNotified)
        return;
    m_closed = true;
    m_closedNotified = true;
    emit closed();
}

void KelpTls::onSocketDisconnected()
{
    if (m_failed)
        return;
    // Drain anything already decrypted before reporting the close.
    pump();
    notifyClosed();
}

void KelpTls::onSocketError()
{
    if (m_failed || !m_socket)
        return;
    fail(QStringLiteral("the connection failed: %1").arg(m_socket->errorString()));
}

void KelpTls::fail(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    qWarning() << "KelpTls:" << message;
    emit error(message);
}

void KelpTls::flushBio()
{
    if (!m_wbio || !m_socket)
        return;
    while (BIO_pending(m_wbio) > 0) {
        char buffer[kTransferChunk];
        const int n = BIO_read(m_wbio, buffer, sizeof(buffer));
        if (n <= 0)
            break;
        m_socket->write(buffer, n);
    }
    m_socket->flush();
}

void KelpTls::writePending()
{
    if (!m_ssl || m_failed || !m_handshakeComplete || m_pending.isEmpty())
        return;

    while (!m_pending.isEmpty()) {
        const int n = SSL_write(m_ssl, m_pending.constData(), m_pending.size());
        if (n > 0) {
            m_pending.remove(0, n);
            continue;
        }
        const int err = SSL_get_error(m_ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            break;                         // retried when the socket moves
        fail(opensslError(QStringLiteral("could not write to the tunnel")));
        return;
    }
    flushBio();
}

void KelpTls::pump()
{
    if (!m_ssl || m_failed)
        return;

    if (!m_handshakeComplete) {
        const int result = SSL_do_handshake(m_ssl);
        if (result != 1) {
            const int err = SSL_get_error(m_ssl, result);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                flushBio();
                return;
            }
            fail(opensslError(QStringLiteral("the TLS handshake failed")));
            return;
        }

        m_handshakeComplete = true;

        const unsigned char *protocol = nullptr;
        unsigned int length = 0;
        SSL_get0_alpn_selected(m_ssl, &protocol, &length);
        m_alpn = length > 0
            ? QString::fromLatin1(reinterpret_cast<const char *>(protocol), int(length))
            : QString();

        if (m_alpn != QLatin1String(kAlpnName)) {
            fail(m_alpn.isEmpty()
                 ? QStringLiteral("the peer did not negotiate ALPN \"kelp/1\"")
                 : QStringLiteral("the peer negotiated ALPN \"%1\", not \"kelp/1\"").arg(m_alpn));
            return;
        }

        flushBio();
        qInfo() << "KelpTls: encrypted with" << protocolVersion() << "ALPN" << m_alpn;
        emit encrypted();
    }

    bool received = false;
    for (;;) {
        char buffer[kTransferChunk];
        const int n = SSL_read(m_ssl, buffer, sizeof(buffer));
        if (n > 0) {
            m_in.append(buffer, n);
            received = true;
            continue;
        }
        const int err = SSL_get_error(m_ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            break;
        if (err == SSL_ERROR_ZERO_RETURN) {
            m_closed = true;
            break;
        }
        fail(opensslError(QStringLiteral("could not read from the tunnel")));
        return;
    }

    writePending();
    flushBio();

    if (received)
        emit readyRead();
    if (m_closed)
        notifyClosed();
}
