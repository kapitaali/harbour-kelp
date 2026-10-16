#ifndef KELPBUNDLES_H
#define KELPBUNDLES_H

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "kelp_bundle.h"
#include "kelp_link.h"

class KelpObjectStore;
class KelpServer;
class KelpSettings;
class KelpTunnel;

/*
 * The QML-facing side of the content layer.
 *
 * `seal` takes the paths the user picked, snapshots them into the object store
 * and returns everything the UI needs to describe the result — including the
 * kelp:// link, which is the only place the key is ever written down.
 *
 * Sealing also publishes: the bundle starts being served on the LAN and its link
 * gains the address, so the result is a link that actually reaches the content.
 * The Settings page can withhold the address, in which case the bundle is not
 * served locally and only a relay can carry it.
 */
class KelpBundles : public QObject
{
    Q_OBJECT

public:
    explicit KelpBundles(KelpObjectStore *store, KelpServer *server = nullptr,
                         QObject *parent = nullptr);

    // Relays to publish on and to put in links as `r=`. The Settings page owns
    // this; an empty list means LAN-only, which is the default.
    Q_INVOKABLE void setRelays(const QStringList &relays);
    QStringList relays() const { return m_relays; }

    // User preferences the publisher acts on: whether a link names this device
    // on the LAN, and whether the link is written to the log. Optional: without
    // one, links carry the address and nothing is written to the log unless the
    // KELP_DEBUG_LINK override is set.
    void setSettings(KelpSettings *settings) { m_settings = settings; }

    // Returns { ok, error, link, bundleId, secret, objectCount, fileCount,
    //           totalBytes, skippedLinks, lan, relay }.
    Q_INVOKABLE QVariantMap seal(const QStringList &paths, const QString &name);

    // Serves a bundle that was shared in an earlier run — the objects and the
    // key are unchanged, only the address is minted afresh, because the LAN port
    // is ephemeral and will have moved. Restores the saved share list at startup
    // (see KelpShares). Returns { ok, error, link, lan, relay }.
    Q_INVOKABLE QVariantMap republish(const QString &bundleId, const QString &secret,
                                      const QString &name, int objects, int files,
                                      quint64 bytes);

    // Stops serving a bundle. The objects stay on disk; nothing else can fetch
    // them until they are shared again.
    Q_INVOKABLE void unshare(const QString &bundleId);

    // Returns { ok, path, objects, bytes, serving } for the About page.
    Q_INVOKABLE QVariantMap storeInfo();

    // Settings → Storage (design §12.7). `shares` is the list the Bundles page is
    // showing — the rows this device actually serves — each with `bundleId` and
    // `secret`. The objects those bundles reach are *in use*; everything else in
    // the store is only a cache, left behind by a fetch or by a share that is gone.
    // Returns { ok, error, objects, bytes, inUseObjects, inUseBytes, unusedObjects,
    // unusedBytes }.
    Q_INVOKABLE QVariantMap storeUsage(const QVariantList &shares);

    // Deletes exactly what storeUsage() called unused, and nothing else. Refuses —
    // deleting nothing — if any share cannot be walked, because an object that
    // cannot be accounted for must never be mistaken for junk. Returns
    // { ok, error, removed, freedBytes }.
    Q_INVOKABLE QVariantMap pruneStore(const QVariantList &shares);

    // Breaks a kelp:// link into the parts the UI explains: the content address
    // and the hints saying where to fetch it. Returns
    // { ok, error, bundleId, relays, lan, name }. The key is deliberately not
    // returned — it never needs to leave the link.
    Q_INVOKABLE QVariantMap linkInfo(const QString &link);

private:
    bool collect(const QString &path, const QString &name, int depth,
                 Kelp::SourceNode *node, int *fileCount, int *skippedLinks,
                 QString *error);

    // Serves an already-sealed bundle and mints a link for it: the LAN address is
    // added as `l=` and any relays as `r=`. Shared by seal() and republish(), so
    // a freshly sealed bundle and a restored one are published the same way.
    Kelp::Link::Bundle publish(const QByteArray &rootId, const QByteArray &secret,
                               const QString &name, int objects, int files, quint64 bytes);

    KelpObjectStore *m_store;
    KelpServer *m_server;
    KelpSettings *m_settings = nullptr;

    QStringList m_relays;
    // One outgoing tunnel per shared bundle, keyed by the base32 bundle id.
    QHash<QString, KelpTunnel *> m_tunnels;
};

#endif // KELPBUNDLES_H
