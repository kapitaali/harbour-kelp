#ifndef KELPSERVER_H
#define KELPSERVER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class KelpObjectStore;
class QHostAddress;
class QTcpServer;
class QTcpSocket;
class QTimer;

/*
 * The publisher's side of the LAN fast path (wire spec §7.2).
 *
 * Listens on this device's LAN address and serves content objects, one HTTP GET
 * at a time, to anyone who proves they hold the link by presenting a bearer
 * token derived from its secret. It deliberately binds the LAN interface rather
 * than every interface: LAN-direct should be reachable on the local network and
 * nowhere else, and mobile data should not open a port by accident.
 *
 * Objects are served only while their bundle is being shared, so "Stop sharing"
 * really stops. Nothing here decrypts: the objects are ciphertext and the
 * receiver checks each id.
 *
 * It keeps its own house in order, too: a cap on connections that have not asked
 * for anything yet, and a deadline for sending a request, so one stalled or greedy
 * client on the Wi-Fi cannot wedge the listener for everyone else.
 */
class KelpServer : public QObject
{
    Q_OBJECT

public:
    explicit KelpServer(KelpObjectStore *store, QObject *parent = nullptr);

    // Binds the first usable LAN address. Failing is not fatal: the bundle is
    // still sealed, its link just carries no `l=`.
    bool start(QString *error);
    bool isListening() const;

    // "host:port" for the link, empty when not listening.
    QString lanAddress() const;

    void share(const QByteArray &bundleId, const QByteArray &swarmToken,
               int objects, int files, quint64 bytes);
    void unshare(const QByteArray &bundleId);

private slots:
    void onConnection();
    void sweepPending();

private:
    struct Shared {
        QByteArray token;
        int objects;
        int files;
        quint64 bytes;
    };

    // A connection that has not delivered a complete request yet.
    struct Pending {
        QByteArray buffer;
        qint64 accepted;
    };

    static QHostAddress lanHostAddress();

    void readRequest(QTcpSocket *socket);
    void handle(QTcpSocket *socket, const QByteArray &request);
    void respond(QTcpSocket *socket, int status, const char *reason,
                 const char *contentType, const QByteArray &body);

    KelpObjectStore *m_store;
    QTcpServer *m_server;
    QTimer *m_sweeper;
    QHash<QTcpSocket *, Pending> m_pending;
    QHash<QByteArray, Shared> m_shared;
    QString m_host;
    quint16 m_port;
};

#endif // KELPSERVER_H
