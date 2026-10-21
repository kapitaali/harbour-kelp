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
 *
 * It also records what each peer asked for and writes one line per peer to the log
 * once the peer goes quiet, so a publisher can tell a fetch that worked from a
 * recipient that never arrived (see Traffic).
 *
 * The bound address is not permanent: a phone that changes Wi-Fi network gets a
 * new one, and a listener left on the old address would accept nothing. A short
 * poll notices the move, rebinds (the port is ephemeral, so it can move too), and
 * emits addressChanged; the app re-mints the links it is showing. Existing links
 * can never be repaired — a link is a copy — so the point is that the rows have a
 * working link again without a restart.
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
               const QString &name, int objects, int files, quint64 bytes);
    void unshare(const QByteArray &bundleId);

    /*
     * What one peer has asked for, keyed by address — deliberately not
     * address:port, because every request is its own connection and a port would
     * make each one look like a new peer. `requests` counts authenticated `/info`
     * calls, which the client makes once per fetch, so it is the number of times
     * this peer started fetching. The log writes one line per peer when it goes
     * quiet; the tests read it directly (§3.14).
     */
    struct Traffic {
        qint64 requests = 0;
        qint64 objects = 0;
        qint64 bytes = 0;
        qint64 refused = 0;         // 401s: a link that was not recognised
        QString bundle;             // the last bundle served, by name
        qint64 lastMs = 0;
    };
    QHash<QString, Traffic> traffic() const { return m_traffic; }

signals:
    // The address written into links changed, including becoming empty when the
    // device lost its network. Everything already handed out is now stale; the
    // app republishes its rows from this signal.
    void addressChanged();

private slots:
    void onConnection();
    void sweepPending();
    void checkAddress();

private:
    struct Shared {
        QByteArray token;
        QString name;
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

    void closeListener();
    void readRequest(QTcpSocket *socket);
    void handle(QTcpSocket *socket, const QByteArray &request);
    void flushTraffic();
    void respond(QTcpSocket *socket, int status, const char *reason,
                 const char *contentType, const QByteArray &body);

    KelpObjectStore *m_store;
    QTcpServer *m_server;
    QTimer *m_sweeper;
    QTimer *m_addresses;
    QHash<QTcpSocket *, Pending> m_pending;
    QHash<QByteArray, Shared> m_shared;
    QHash<QString, Traffic> m_traffic;
    QString m_host;          // what links advertise (KELP_ADVERTISE_HOST may change it)
    QString m_boundHost;     // the interface address the socket is actually bound to
    quint16 m_port;
};

#endif // KELPSERVER_H
