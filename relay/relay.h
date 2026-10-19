#ifndef KELP_RELAY_H
#define KELP_RELAY_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "kelp_wire.h"

class KelpTls;
class QTcpServer;
class QTcpSocket;
class QTimer;

/*
 * The Kelp relay (kelp-swarm-wire-spec.md §12.1).
 *
 * A relay exists for one reason: the publisher is behind carrier NAT and cannot
 * accept a connection, so the publisher dials *out* to the relay and keeps that
 * tunnel open. A recipient then fetches from the relay, and the relay asks the
 * origin for the object down the tunnel it already has.
 *
 *     recipient ──fetch──▶ relay ──WANT/DATA──▶ publisher (tunnel)
 *
 * One address, two roles: every connection is TLS 1.3 with ALPN "kelp/1", and
 * the role is decided by what arrives first — an HTTP request line begins with
 * an ASCII method letter, while a wire frame begins with a 4-byte length (a NUL
 * or another low byte). One advertised address is what a link's `r=` can name;
 * two ports would force the link to carry both.
 *
 * It is deliberately untrusted. It holds only the relay_token (§12.1) — never
 * the content key or the swarm token — and it carries the same ciphertext
 * envelopes a LAN peer would. It can withhold an object but it cannot
 * substitute one: the recipient verifies every object against its id, so a
 * lying relay produces a failed download, not a wrong file.
 *
 * It is also a gateway for a recipient with no app at all (design §5.2): the
 * same port and the same token-gated object paths, plus a loader page served
 * under /b/<bundle-id>/ that a browser opens. The loader derives the relay token
 * from the link's fragment itself (WebCrypto HKDF), so the gateway needs no data
 * path of its own and the relay still sees ciphertext only. A browser speaks
 * ALPN "http/1.1", so the ALPN list here is "kelp/1" and "http/1.1"; a tunnel
 * still has to negotiate "kelp/1".
 */
class Relay : public QObject
{
    Q_OBJECT

public:
    explicit Relay(QObject *parent = nullptr);
    ~Relay();

    // Binds one port on all interfaces. certPem/keyPem are the relay's own
    // self-signed material (peers accept any cert; content is hash-verified).
    bool start(quint16 port, const QByteArray &certPem, const QByteArray &keyPem,
               QString *error);
    quint16 port() const;

private slots:
    void onConnection();
    void sweep();

private:
    enum Role { Unknown, Tunnel, Recipient };

    struct Connection {
        QTcpSocket *socket;
        KelpTls *tls;
        Role role;
        QByteArray buffer;              // only until the role is decided / request is whole
        Kelp::Wire::FrameReader reader; // tunnels only
        bool busy;                      // recipient: a request is being served
        bool closeAfter;                // recipient: this answer ends the connection

        Connection() : socket(nullptr), tls(nullptr), role(Unknown), busy(false),
                       closeAfter(false) {}
    };

    struct Bundle {
        QByteArray token;
        quint64 objects;
        quint64 files;
        quint64 bytes;
        KelpTls *tunnel;

        Bundle() : objects(0), files(0), bytes(0), tunnel(nullptr) {}
    };

    struct Pending {
        Connection *recipient;
        QByteArray bundleId;
        qint64 deadline;

        Pending() : recipient(nullptr), deadline(0) {}
    };

    void setup(QTcpSocket *socket);
    void onData(Connection *connection);
    void decide(Connection *connection);
    void feedReader(Connection *connection, const QByteArray &data);

    void onTunnelFrame(Connection *connection, quint8 type, const QByteArray &payload);
    void dropTunnel(Connection *connection);

    void handleRequest(Connection *connection, const QByteArray &request);
    void handleGatewayRequest(Connection *connection, const QString &path);
    bool tryRequest(Connection *connection);
    void respond(Connection *connection, int status, const char *reason,
                 const char *contentType, const QByteArray &body,
                 const QByteArray &extraHeaders = QByteArray());
    void answerPending(const QByteArray &objectId, const QByteArray &bytes, bool found);
    void retire(Connection *connection);

    QTcpServer *m_server;
    QTimer *m_sweeper;
    QByteArray m_certificatePem;
    QByteArray m_keyPem;

    // The gateway's own files, from the qrc (relay/gateway.qrc).
    QByteArray m_loaderHtml;
    QByteArray m_coreJs;
    QByteArray m_pageJs;
    QByteArray m_swJs;

    QList<Connection *> m_connections;
    QHash<QByteArray, Bundle> m_bundles;              // bundle_id -> bundle
    QHash<QByteArray, Pending> m_pending;             // object_id -> waiting recipient
    QHash<KelpTls *, QList<QByteArray> > m_tunnels;   // tunnel -> bundle ids it serves
};

#endif // KELP_RELAY_H
