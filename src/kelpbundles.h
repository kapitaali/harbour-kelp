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
    //           totalBytes, skippedLinks, lan, relay, passBlob }.
    //
    // `page` says the thing being shared is a *webpage* (design §5.5): the link
    // then tells the recipient to open it in a browser and leave nothing behind,
    // and validation that it really holds a page is the caller's job
    // (KelpFiles::pageEntry), because only it can look at the folder.
    //
    // A non-empty `passphrase` makes a *passphrase link* (design §12.3): the
    // master secret is wrapped under a key derived from the passphrase, and the
    // link carries only the wrapped form, so the link by itself opens nothing.
    // The passphrase is used here and never stored — what the row keeps is the
    // wrapped blob (`passBlob`), which is enough to mint the same link again.
    Q_INVOKABLE QVariantMap seal(const QStringList &paths, const QString &name,
                                 bool page, const QString &passphrase = QString());

    // Serves a bundle that was shared in an earlier run — the objects and the
    // key are unchanged, only the address is minted afresh, because the LAN port
    // is ephemeral and will have moved. Restores the saved share list at startup
    // (see KelpShares). `page` is the row's own flag, so a restored webpage share
    // is still a webpage share, and `passBlob` is the row's wrapped secret, so a
    // restored passphrase share is still protected by the same passphrase.
    // Returns { ok, error, link, lan, relay }.
    Q_INVOKABLE QVariantMap republish(const QString &bundleId, const QString &secret,
                                      const QString &name, int objects, int files,
                                      quint64 bytes, bool page,
                                      const QString &passBlob = QString());

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
    // { ok, error, bundleId, relays, lan, name, page, needsPassphrase }. `page` is
    // the publisher's "open this in a browser" hint (design §5.5) and is false for
    // every link minted before it existed. The key is deliberately not returned —
    // it never needs to leave the link.
    //
    // For a passphrase link nothing but the content address and the relay names
    // can be read: the address, the label and the kind are all sealed under the
    // secret the passphrase unwraps (§12.3). `needsPassphrase` is what says so,
    // and unlockLink() is what opens the rest.
    Q_INVOKABLE QVariantMap linkInfo(const QString &link);

    // Opens a passphrase link with the passphrase the user typed, the way a
    // recipient does: it derives the key, unwraps the master secret and reads the
    // hints that were sealed under it. Returns the same fields as linkInfo() plus
    // the answer, { ok, error, bundleId, relays, lan, name, page,
    // needsPassphrase }, with `needsPassphrase` false on success. The passphrase
    // is not kept: a later open() is asked for it again.
    Q_INVOKABLE QVariantMap unlockLink(const QString &link, const QString &passphrase);

    /*
     * The browser's door to one of *this device's* shares (design §5.2, C4b).
     *
     * A recipient with no app opens https://<relay>/b/<bundle-id>#k=<secret> in a
     * browser; the relay serves a loader, and the browser derives the relay token
     * from the fragment and decrypts every object itself. This builds that URL
     * from the two things a share row keeps — its bundle id and its secret — and
     * from the relay the share is actually published on, which is the one named in
     * its own link's `r=` (Settings only decides where *future* shares go).
     *
     * The port is always written out: kelpRelayHostPort() supplies the tunnel's
     * default 9443, but a browser has no such default. https:// is the only form
     * that can work — WebCrypto and a service worker need a secure context — and
     * the relay speaks TLS either way.
     *
     * Returns { ok, error, url }. Never for a passphrase share: the app holds the
     * plaintext secret, and minting `#k=` would undo the passphrase (D4 refuses
     * `#p=` at the gateway), so the caller must not offer this there.
     */
    Q_INVOKABLE QVariantMap browserUrl(const QString &bundleId, const QString &secret,
                                       const QString &relay);

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
    // a freshly sealed bundle and a restored one are published the same way. A
    // non-empty `wrappedSecret` makes the link a passphrase link (`#p=`) instead
    // of an ordinary one (`#k=`).
    Kelp::Link::Bundle publish(const QByteArray &rootId, const QByteArray &secret,
                               const QString &name, int objects, int files, quint64 bytes,
                               bool page, const QByteArray &wrappedSecret = QByteArray());

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
