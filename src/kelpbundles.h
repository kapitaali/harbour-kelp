#ifndef KELPBUNDLES_H
#define KELPBUNDLES_H

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include "kelp_bundle.h"

class KelpObjectStore;
class KelpServer;
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

    // Returns { ok, error, link, bundleId, secret, objectCount, fileCount,
    //           totalBytes, skippedLinks, lan, relay }.
    Q_INVOKABLE QVariantMap seal(const QStringList &paths, const QString &name);

    // Stops serving a bundle. The objects stay on disk; nothing else can fetch
    // them until they are shared again.
    Q_INVOKABLE void unshare(const QString &bundleId);

    // Returns { ok, path, objects, bytes, serving } for the About page.
    Q_INVOKABLE QVariantMap storeInfo();

    // Breaks a kelp:// link into the parts the UI explains: the content address
    // and the hints saying where to fetch it. Returns
    // { ok, error, bundleId, relays, lan, name }. The key is deliberately not
    // returned — it never needs to leave the link.
    Q_INVOKABLE QVariantMap linkInfo(const QString &link);

private:
    bool collect(const QString &path, const QString &name, int depth,
                 Kelp::SourceNode *node, int *fileCount, int *skippedLinks,
                 QString *error);

    KelpObjectStore *m_store;
    KelpServer *m_server;

    QStringList m_relays;
    // One outgoing tunnel per shared bundle, keyed by the base32 bundle id.
    QHash<QString, KelpTunnel *> m_tunnels;
};

#endif // KELPBUNDLES_H
