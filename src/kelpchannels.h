/*
 * Channels this device publishes.
 *
 * A bundle is immutable and its link names it forever. A channel is the mutable
 * version (WS-C4 §4): a stable `kelpchannel://` address whose pointer moves to a
 * new bundle when the user publishes one. This class owns the publisher side —
 * the key pair, the current pointer and the read secret the link carries.
 *
 * Two files, both owner-only, because they are not interchangeable secrets:
 *
 *   channels.json      name, ids, sequence, timestamp, the last few pointer
 *                      object ids, and the read secret (the link fragment, which
 *                      is what lets the link be minted again after a restart)
 *   channel-keys.json  the Ed25519 private keys
 *
 * The private key never enters channels.json and neither secret is ever
 * returned by load(): QML gets the rows it displays, and asks link() for the
 * one capability it is allowed to show. Signing is the difference — a leaked
 * link lets someone read a channel, not update it.
 *
 * No logging here may contain a key, a secret or a link. Errors name a channel
 * id or a file path, which are not capabilities.
 */

#ifndef KELPCHANNELS_H
#define KELPCHANNELS_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class KelpObjectStore;

/*
 * Where a signed channel update goes: the publisher's relay tunnels.
 *
 * Kept as an interface so the registry does not pull in the network stack (or
 * start a tunnel in its own test). KelpBundles implements it; the count it
 * returns is how many tunnels the update was handed to — zero means nothing is
 * shared over a relay right now, and the update is only waiting locally.
 */
class KelpChannelSink
{
public:
    virtual ~KelpChannelSink() {}
    virtual int sendChannelUpdate(const QByteArray &channelId,
                                  const QByteArray &update) = 0;
};


class KelpChannels : public QObject
{
    Q_OBJECT

public:
    explicit KelpChannels(KelpObjectStore *store, QObject *parent = nullptr);

    // Both live beside the log and the share list, in the directory Sailjail
    // provisions. Static: a path is a property of the app, not of an instance.
    static QString filePath();
    static QString keysFilePath();

    // Relays to name in a new link, the same list the publisher puts in bundle
    // links. Empty means the link carries no relay and only a direct fetch can
    // resolve it.
    void setRelays(const QStringList &relays) { m_relays = relays; }
    QStringList relays() const { return m_relays; }

    // Where signed relay updates go. Null (the default) means create() and
    // publish() still record the channel but cannot announce it; the result
    // says queued=false.
    void setSink(KelpChannelSink *sink) { m_sink = sink; }

    // Re-signs the current pointer of every channel and hands it to the sink.
    // Called at startup: relay state is RAM-only, so after a relay restart the
    // publisher is what puts its channels back. Returns how many updates the
    // sink accepted (0 when no relay tunnel is up).
    Q_INVOKABLE int reannounce();

    // The channel rows the UI shows: id, name, bundleId, sequence, timestamp,
    // hasKey, pointer. Never the private key or the read secret.
    Q_INVOKABLE QVariantList load() const;

    // Creates a channel whose first pointer names `bundleId` (the base32 bundle
    // id the Shares page already uses), signs it, stores the sealed pointer
    // object, and returns { ok, error, id, name, bundleId, sequence, timestamp,
    // pointer, link }. The link is the only thing in the result that carries the
    // read secret.
    //
    // `bundleSecret` is the bundle's own content key, as base64url text (the
    // secret a share row already holds). It goes into the signed pointer, sealed
    // for the link's readers, so a channel reader can open the bundle. Only a
    // bundle without a passphrase may be published this way: the pointer would
    // otherwise hand out the key past the passphrase.
    Q_INVOKABLE QVariantMap create(const QString &name, const QString &bundleId,
                                   const QString &bundleSecret);

    // Re-signs an existing channel to point at `bundleId` with sequence + 1 and
    // stores the new pointer. Returns the same shape as create(), the link
    // included (the read secret does not change).
    Q_INVOKABLE QVariantMap publish(const QString &channelId, const QString &bundleId,
                                    const QString &bundleSecret);

    // The kelpchannel:// link for a channel this device owns. Empty with a
    // logged reason when the channel is unknown or its stored bytes are not the
    // width the link needs.
    Q_INVOKABLE QString link(const QString &channelId) const;

private:
    struct Entry {
        QString id;             // base32
        QString name;
        QByteArray publicKey;
        QByteArray secret;      // read secret, 32 bytes
        QString bundleId;       // base32
        QByteArray bundleSecret; // 32 bytes, the bundle's content key
        quint64 sequence;
        qint64 timestamp;
        QStringList pointers;   // newest first, hex object ids

        Entry() : sequence(0), timestamp(0) {}
    };

    QVector<Entry> loadEntries(QString *error) const;
    bool saveEntries(const QVector<Entry> &entries, QString *error) const;
    QHash<QString, QByteArray> loadKeys(QString *error) const;
    bool saveKeys(const QHash<QString, QByteArray> &keys, QString *error) const;

    // Signs the relay statement for one sealed pointer and hands it to the sink.
    // True when at least one relay tunnel holds it. The statement is only needed
    // to register the channel with a relay, so a false here does not undo the
    // local publish; reannounce() repeats it later.
    bool announce(const QByteArray &pointerBytes, quint64 sequence, qint64 timestamp,
                  const QByteArray &privateKey);

    KelpObjectStore *m_store;
    KelpChannelSink *m_sink = nullptr;
    QStringList m_relays;
};

#endif // KELPCHANNELS_H
