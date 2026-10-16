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
 * between networks recovers without the user re-sharing.
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
               const QByteArray &relayToken, int objects, int files, quint64 bytes,
               QString *error);

    void stop();

    bool isLive() const { return m_live; }
    // The relay it is currently connected to, or empty when it is not.
    QString relay() const { return m_live ? m_current : QString(); }

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
    void connectToRelay(int index);
    void teardown();
    void scheduleRetry();

    KelpObjectStore *m_store;
    QStringList m_relays;
    int m_index;
    QByteArray m_bundleId;
    QByteArray m_token;
    int m_objects;
    int m_files;
    quint64 m_bytes;

    bool m_wanted;
    bool m_live;
    int m_attempt;
    QString m_current;

    QTcpSocket *m_socket;
    KelpTls *m_tls;
    QTimer *m_keepalive;
    QTimer *m_retry;
    Kelp::Wire::FrameReader m_reader;
};

#endif // KELPTUNNEL_H
