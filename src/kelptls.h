#ifndef KELPTLS_H
#define KELPTLS_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QTcpSocket;
QT_END_NAMESPACE

struct ssl_ctx_st;
struct ssl_st;
struct bio_st;

/*
 * The relay tunnel's transport: TLS 1.3 with ALPN "kelp/1" (wire spec §7.1).
 *
 * Qt 5.6 cannot speak this. Its QSslSocket has no TLS 1.3 and no client-side
 * ALPN, so rather than settle for TLS 1.2 with no protocol negotiation, the
 * tunnel drives OpenSSL 3's libssl directly — the same library family that
 * already backs libkelp's content crypto, and already on the device as
 * libssl.so.3. OpenSSL does the TLS; only the event-loop integration is ours.
 *
 * Integration is deliberately through Qt's socket, not the raw fd: Qt owns the
 * connection (connect, DNS, error handling) and this class takes the bytes it
 * produces and feeds them to OpenSSL, and hands OpenSSL's output back the same
 * way. OpenSSL is attached with a pair of memory BIOs, so Qt's internal socket
 * buffering and OpenSSL's records never compete for the same fd — the mistake
 * that makes a naive "hand the fd to libssl" integration lose bytes.
 *
 * The certificate is not verified: peers are self-signed and content is verified
 * by hash regardless (§7.1). TLS here protects metadata in transit; it is not a
 * trust anchor. The one thing that *is* enforced is the ALPN protocol — a peer
 * that does not negotiate "kelp/1" is refused, because that is what pins the
 * protocol family (wire spec §1.2, §16).
 *
 * The same class serves both ends: a Client tunnel (the publisher dialling out,
 * §3.2) and a Server tunnel (the relay accepting it, and reused later for
 * steward/peer connections). It is app-layer code on purpose — libkelp stays
 * pure and I/O-free.
 */
class KelpTls : public QObject
{
    Q_OBJECT

public:
    enum Mode { Client, Server };

    explicit KelpTls(QObject *parent = nullptr);
    ~KelpTls();

    // Server side: the certificate and key to present (PEM). Must be set before
    // start() in Server mode. The certificate is normally self-signed.
    bool setCertificate(const QByteArray &certificatePem, const QByteArray &keyPem,
                        QString *error);

    // Server side, before start(): which ALPN protocols to accept, most
    // preferred first. Defaults to "kelp/1" alone. A relay that also serves
    // browsers lists "http/1.1" here (design §5.2, C4b) — a browser does not
    // speak "kelp/1", and a page that never loads is not a gateway. The client
    // end offers, and still requires, "kelp/1": nothing about the tunnel's
    // guarantee changes.
    void setAlpnProtocols(const QStringList &protocols);

    // Takes over an already-connected (Client) or already-accepted (Server)
    // socket. The socket must outlive this object. Returns false only when the
    // session cannot even be set up; a handshake that fails later arrives as the
    // error() signal.
    bool start(Mode mode, QTcpSocket *socket, QString *error);

    bool isEncrypted() const { return m_handshakeComplete; }
    // "TLSv1.3" once encrypted. The negotiated ALPN, "kelp/1".
    QString protocolVersion() const;
    QString alpnProtocol() const { return m_alpn; }

    // Decrypted application bytes received so far.
    QByteArray readAll();

    // Queues data for the peer. Every byte is accepted here and written out as
    // the socket allows, so a short write is not lost. Returns data.size().
    qint64 write(const QByteArray &data);

    // Best-effort close: a TLS close_notify, then the socket is shut down.
    // Safe to call twice.
    void close();

signals:
    // The handshake finished and ALPN is "kelp/1": the tunnel is usable.
    void encrypted();
    void readyRead();
    void error(const QString &message);
    void closed();

private slots:
    void onSocketReadyRead();
    void onSocketBytesWritten();
    void onSocketDisconnected();
    void onSocketError();

private:
    void pump();
    void flushBio();
    void writePending();
    void fail(const QString &message);
    QString opensslError(const QString &prefix) const;

    Mode m_mode;
    QTcpSocket *m_socket;
    ssl_ctx_st *m_ctx;
    ssl_st *m_ssl;
    QStringList m_alpnProtocols;   // server: what to accept; client: "kelp/1"
    QByteArray m_alpnWire;         // the same list, in ALPN wire form
    bio_st *m_rbio;              // owned by m_ssl once SSL_set_bio has been called
    bio_st *m_wbio;

    QByteArray m_certificatePem;
    QByteArray m_keyPem;

    QByteArray m_in;             // decrypted, waiting for readAll()
    QByteArray m_pending;        // queued, not yet accepted by SSL_write()

    QString m_alpn;
    bool m_handshakeComplete;
    bool m_closed;
    bool m_closedNotified;      // closed() is a one-shot: emit it at most once
    bool m_failed;

    void notifyClosed();
};

#endif // KELPTLS_H
