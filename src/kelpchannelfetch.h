#ifndef KELPCHANNELFETCH_H
#define KELPCHANNELFETCH_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include "kelp_channel.h"

class KelpTls;
class QTcpSocket;
class QTimer;

/*
 * Resolves a kelpchannel:// link to the bundle it currently points at (WS-C4
 * §4.3, §4.4).
 *
 * The link names a channel and its read secret. For each relay the link names,
 * this asks for the channel's sealed pointer over the same TLS "kelp/1" path a
 * bundle fetch uses, opens it with the read secret, and checks it against the
 * channel id. Relays are asked in turn and the newest verified pointer wins, so
 * a relay that is behind does not roll a reader back.
 *
 * The result is an ordinary kelp:// bundle link built from the pointer. The
 * caller hands it to KelpClient, which then downloads and verifies the bundle
 * exactly as for any other link. This class never fetches bundle content.
 *
 * The result carries a capability (the bundle link's fragment), so it is never
 * logged, and a failure names a relay address or a reason, not a link.
 */
class KelpChannelFetch : public QObject
{
    Q_OBJECT

public:
    explicit KelpChannelFetch(QObject *parent = nullptr);

    // Starts resolving `link`. Returns { ok, error }: ok means the lookup has
    // started and resolved() will say what it found; otherwise the link was
    // refused and nothing is in flight.
    Q_INVOKABLE QVariantMap resolveAsync(const QString &link);

    // Abandons a lookup in flight. It reports itself as cancelled.
    Q_INVOKABLE void cancel();

signals:
    // { ok, error, name, sequence, timestamp, bundleId, link }. `link` is the
    // bundle's kelp:// link, present only when ok.
    void resolved(const QVariantMap &result);

private:
    void nextRelay();
    void onConnected();
    void onEncrypted();
    void onReadyRead();
    void onTlsDone();
    void parseResponse();
    void handleResponse(int status, const QByteArray &body);
    void relayFailed(const QString &why);
    void dropConnection();
    void finish();

    Kelp::Channel::Link m_link;
    int m_index;
    bool m_active;
    bool m_have;
    bool m_responseDone;
    QString m_lastError;
    Kelp::Channel::Pointer m_best;

    QTcpSocket *m_socket;
    KelpTls *m_tls;
    QByteArray m_buffer;
    QTimer *m_timeout;
};

#endif // KELPCHANNELFETCH_H
