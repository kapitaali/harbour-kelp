#ifndef KELPTUNNEL_H
#define KELPTUNNEL_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include "kelp_wire.h"

class KelpObjectStore;
class KelpTls;
class QTcpSocket;
class QTimer;

/*
 * The publisher's side of the relay (wire spec §12.1).
 *
 * Carrier NAT means the phone cannot accept a connection, so it dials *out* —
 * this keeps that tunnel open. It registers the bundle (its id, its relay token
 * and its shape) and then answers the relay's WANT frames with DATA taken from
 * the local object store, or REJECT when it does not have the object.
 *
 * It holds no secret beyond the relay token: the relay is given a credential of
 * its own (Kelp::relayToken) and nothing that unlocks content or the swarm. What
 * it serves is the same ciphertext the LAN path serves.
 *
 * The tunnel reconnects on its own if the relay drops it, so a phone that moves
 * between networks recovers without the user re-sharing. It also records what
 * recipients asked for and how much of it went out, so the publisher can tell a
 * fetch that was served from one that never arrived (see Traffic).
 */
class KelpTunnel : public QObject
{
    Q_OBJECT

public:
    explicit KelpTunnel(KelpObjectStore *store, QObject *parent = nullptr);
    ~KelpTunnel();

    // Opens a tunnel to the first relay that accepts it and registers the
    // bundle. Returns false only when the arguments are unusable; a relay that
    // cannot be reached is retried in the background.
    bool start(const QStringList &relays, const QByteArray &bundleId,
               const QByteArray &relayToken, const QString &name, int objects,
               int files, quint64 bytes, QString *error);

    void stop();

    bool isLive() const { return m_live; }
    // The relay it is currently connected to, or empty when it is not.
    QString relay() const { return m_live ? m_current : QString(); }

    /*
     * What recipients have asked for through the current tunnel, and how much of
     * it went out. The relay multiplexes recipients, so these counts belong to
     * the tunnel rather than to one peer — which is all the publisher can know on
     * this path. One line is written to the log when the asking stops for a few
     * seconds, or when the tunnel goes down; the tests read this directly (§3.15).
     */
    struct Traffic {
        qint64 wanted = 0;      // WANT frames: objects a recipient asked for
        qint64 objects = 0;     // DATA frames sent
        qint64 bytes = 0;       // their size as stored (ciphertext)
        qint64 rejected = 0;    // REJECTs: wanted, but not held here
    };
    Traffic traffic() const { return m_traffic; }

signals:
    void changed();

private slots:
    void onConnected();
    void onEncrypted();
    void onReadyRead();
    void onSocketError();
    void onClosed();
    void onKeepalive();
    void onRetry();

private:
    void sendRegister();
    void handleFrame(quint8 type, const QByteArray &payload);
    void serve(const QByteArray &objectId);
    void flushTraffic();
    void connectToRelay(int index);
    void teardown();
    void scheduleRetry();

    KelpObjectStore *m_store;
    QStringList m_relays;
    int m_index;
    QByteArray m_bundleId;
    QByteArray m_token;
    QString m_name;
    int m_objects;
    int m_files;
    quint64 m_bytes;
    Traffic m_traffic;

    bool m_wanted;
    bool m_live;
    int m_attempt;
    QString m_current;

    QTcpSocket *m_socket;
    KelpTls *m_tls;
    QTimer *m_keepalive;
    QTimer *m_retry;
    QTimer *m_idle;
    Kelp::Wire::FrameReader m_reader;
};

#endif // KELPTUNNEL_H
