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
class KelpPreviewServer;
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
    //
    // `page` says the thing being shared is a *webpage* (design §5.5): the link
    // then tells the recipient to open it in a browser and leave nothing behind,
    // and validation that it really holds a page is the caller's job
    // (KelpFiles::pageEntry), because only it can look at the folder.
    Q_INVOKABLE QVariantMap seal(const QStringList &paths, const QString &name,
                                 bool page);

    // Serves a bundle that was shared in an earlier run — the objects and the
    // key are unchanged, only the address is minted afresh, because the LAN port
    // is ephemeral and will have moved. Restores the saved share list at startup
    // (see KelpShares). `page` is the row's own flag, so a restored webpage share
    // is still a webpage share. Returns { ok, error, link, lan, relay }.
    Q_INVOKABLE QVariantMap republish(const QString &bundleId, const QString &secret,
                                      const QString &name, int objects, int files,
                                      quint64 bytes, bool page);

    // Stops serving a bundle. The objects stay on disk; nothing else can fetch
    // them until they are shared again. This is the delete path — the caller
    // drops the row too — but the serving stop is also what pause() needs.
    Q_INVOKABLE void unshare(const QString &bundleId);

    // Stops serving a bundle without forgetting it: the objects stay on disk and
    // the share keeps its identity, so republish() brings it back. This is the
    // non-destructive half of unshare(), for a share the user wants to silence
    // for a while (already handed-out links stop working until it is resumed).
    Q_INVOKABLE void pause(const QString &bundleId);

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
    // { ok, error, bundleId, relays, lan, name, page }. `page` is the publisher's
    // "open this in a browser" hint (design §5.5) and is false for every link
    // minted before it existed. The key is deliberately not returned — it never
    // needs to leave the link.
    Q_INVOKABLE QVariantMap linkInfo(const QString &link);

    /*
     * The loopback preview (design §5.1).
     *
     * A browser cannot decrypt from the sender: WebCrypto needs a secure context,
     * and the LAN path is deliberately plain http:// to a private address. So this
     * app is what decrypts, and it serves the result to a browser on *this* device
     * over loopback. The server is optional — without one there is no preview, and
     * a share behaves exactly as before.
     */
    void setPreviewServer(KelpPreviewServer *preview);

    // Which file this bundle offers a browser, without serving anything:
    // { ok, error, hasPage, entry, root, candidates }. A bundle that does not make
    // the choice itself leaves `entry` empty and lists every HTML file in
    // `candidates` — what a picker would show (design §5.3).
    Q_INVOKABLE QVariantMap pageInfo(const QString &bundleId, const QString &secret);

    // Opens that page for a browser on this device and hands back the URL to give
    // it: { ok, error, url, token, entry, candidates }. `entry` may name one of
    // pageInfo's candidates rather than letting the bundle choose.
    Q_INVOKABLE QVariantMap previewPage(const QString &bundleId, const QString &secret,
                                        const QString &name, const QString &entry);

    // Ends a preview session; its URL stops answering at once.
    Q_INVOKABLE void endPreview(const QString &token);

    // How many previews are open, for the cover and the keep-alive policy.
    Q_INVOKABLE int previewSessions() const;

signals:
    // A preview opened, closed, or expired. The app holds the device awake while
    // one is being served, exactly as it does while a share is being served.
    void previewSessionsChanged();

private:
    bool collect(const QString &path, const QString &name, int depth,
                 Kelp::SourceNode *node, int *fileCount, int *skippedLinks,
                 QString *error);

    // Serves an already-sealed bundle and mints a link for it: the LAN address is
    // added as `l=` and any relays as `r=`. Shared by seal() and republish(), so
    // a freshly sealed bundle and a restored one are published the same way.
    Kelp::Link::Bundle publish(const QByteArray &rootId, const QByteArray &secret,
                               const QString &name, int objects, int files, quint64 bytes,
                               bool page);

    // Takes a bundle out of service on both paths — the relay tunnel first, then
    // the LAN listener. Shared by unshare() and pause(), which differ only in
    // whether the caller means it as "gone" or "not right now".
    void stopServing(const QString &bundleId);

    // The bundle id and content key behind a share row — the two things a stored
    // share has to offer that a link's hints do not. False with a reason if either
    // is unusable.
    bool bundleKey(const QString &bundleId, const QString &secret, QByteArray *id,
                   QByteArray *key, QString *error) const;

    KelpObjectStore *m_store;
    KelpServer *m_server;
    KelpSettings *m_settings = nullptr;
    KelpPreviewServer *m_preview = nullptr;

    QStringList m_relays;
    // One outgoing tunnel per shared bundle, keyed by the base32 bundle id.
    QHash<QString, KelpTunnel *> m_tunnels;
};

#endif // KELPBUNDLES_H
