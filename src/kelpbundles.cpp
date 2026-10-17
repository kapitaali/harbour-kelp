#include "kelpbundles.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>

#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_web.h"
#include "kelpbuild.h"
#include "kelppreviewserver.h"
#include "kelpserver.h"
#include "kelpsettings.h"
#include "kelpstore.h"
#include "kelptunnel.h"

namespace {

// Deep enough for any real folder; there to stop a symlink loop.
const int kMaxDepth = 64;

// A link is a capability — whoever holds it can read the bundle — so writing one
// into the log is a deliberate act, never an accident. It is part of the dev-extras
// bundle: in a release build it never happens at all, and in a dev build it is on
// by default (Settings → "Include links in the log" turns it off). KELP_DEBUG_LINK
// forces it on for a one-off run, so headless scripts keep working without touching
// the UI. Logging a link makes the log a secret: it holds the key.
bool linksInLog(const KelpSettings *settings)
{
    if (!KelpBuild::devExtras())
        return false;
    return (settings && settings->includeLinksInLog())
           || !qgetenv("KELP_DEBUG_LINK").isEmpty();
}

// The object ids the shares on this device are made of. Strict on purpose: if one
// share cannot be walked, the whole computation fails. "Not in this set" has to
// mean "safe to delete", and an object this device merely cannot account for —
// because a manifest is unreadable, or its stored key is wrong — is not junk.
bool liveObjects(KelpObjectStore *store, const QVariantList &shares,
                 QSet<QByteArray> *live, QString *error)
{
    for (int i = 0; i < shares.size(); ++i) {
        const QVariantMap row = shares.at(i).toMap();
        const QString bundleId = row.value(QStringLiteral("bundleId")).toString();
        if (bundleId.isEmpty())
            continue;

        QByteArray rootId;
        QString why;
        if (!Kelp::Link::base32Decode(bundleId, &rootId, &why)) {
            *error = QStringLiteral("share %1: %2").arg(bundleId, why);
            return false;
        }

        const QString secretText = row.value(QStringLiteral("secret")).toString();
        QByteArray secret;
        if (!Kelp::Link::base64UrlDecode(secretText, &secret, &why)
            || secret.size() != Kelp::Crypto::KeySize) {
            *error = QStringLiteral("share %1: its stored key is not usable")
                         .arg(bundleId);
            return false;
        }

        QVector<QByteArray> ids;
        if (!Kelp::bundleObjects(store, Kelp::contentKey(secret), rootId, &ids, &why)) {
            *error = QStringLiteral("share %1 cannot be read, so nothing was "
                                    "deleted: %2").arg(bundleId, why);
            return false;
        }
        for (int j = 0; j < ids.size(); ++j)
            live->insert(ids.at(j));
    }
    return true;
}

} // namespace

KelpBundles::KelpBundles(KelpObjectStore *store, KelpServer *server, QObject *parent)
    : QObject(parent), m_store(store), m_server(server)
{
}

// ── Preview (design §5.1) ────────────────────────────────────────────────────

void KelpBundles::setPreviewServer(KelpPreviewServer *preview)
{
    m_preview = preview;
    if (m_preview) {
        // The server drops an idle session by itself; the app has to hear about it
        // or it would hold the keep-alive request forever.
        connect(m_preview, &KelpPreviewServer::sessionsChanged,
                this, &KelpBundles::previewSessionsChanged);
    }
}

bool KelpBundles::bundleKey(const QString &bundleId, const QString &secret, QByteArray *id,
                            QByteArray *key, QString *error) const
{
    QString why;
    if (!Kelp::Link::base32Decode(bundleId, id, &why)) {
        *error = QStringLiteral("this share has no usable id");
        return false;
    }

    QByteArray master;
    if (!Kelp::Link::base64UrlDecode(secret, &master, &why)
        || master.size() != Kelp::Crypto::KeySize) {
        *error = QStringLiteral("this share's stored key is not usable");
        return false;
    }

    *key = Kelp::contentKey(master);
    return true;
}

QVariantMap KelpBundles::pageInfo(const QString &bundleId, const QString &secret)
{
    QVariantMap info;
    info.insert(QStringLiteral("ok"), false);
    info.insert(QStringLiteral("hasPage"), false);
    info.insert(QStringLiteral("entry"), QString());

    QByteArray id;
    QByteArray key;
    QString error;
    if (!bundleKey(bundleId, secret, &id, &key, &error)) {
        info.insert(QStringLiteral("error"), error);
        return info;
    }

    Kelp::Web::Page page;
    if (!Kelp::Web::page(m_store, key, id, &page, &error)) {
        // Every file manifest has to be readable to answer this at all, so a share
        // whose objects are not all here yet has no knowable page. That is an
        // error, not "no page": the two must not look alike.
        info.insert(QStringLiteral("error"), error);
        return info;
    }

    info.insert(QStringLiteral("ok"), true);
    info.insert(QStringLiteral("hasPage"), !page.path.isEmpty());
    info.insert(QStringLiteral("entry"), page.path);
    info.insert(QStringLiteral("root"), page.root);
    info.insert(QStringLiteral("candidates"), page.candidates);
    return info;
}

QVariantMap KelpBundles::previewPage(const QString &bundleId, const QString &secret,
                                     const QString &name, const QString &entry)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);

    if (!m_preview) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("this build cannot serve a preview"));
        return result;
    }

    // The listener comes up on the first preview and stays up: binding a loopback
    // port is cheap, and a second page should not pay for it again.
    QString error;
    if (!m_preview->start(&error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    const KelpPreviewServer::Opened opened = entry.isEmpty()
        ? m_preview->open(bundleId, secret, name)
        : m_preview->openEntry(bundleId, secret, name, entry);

    result.insert(QStringLiteral("ok"), opened.ok);
    result.insert(QStringLiteral("error"), opened.error);
    result.insert(QStringLiteral("url"), opened.url);
    result.insert(QStringLiteral("token"), opened.token);
    result.insert(QStringLiteral("entry"), opened.entry);
    result.insert(QStringLiteral("candidates"), opened.candidates);
    if (!opened.ok)
        return result;

    // The URL is a read capability for the *decrypted* bundle while the session
    // lives, so it is written down only under the same gate a link is under — and
    // never in a release build, whatever that gate says.
    if (linksInLog(m_settings))
        qInfo() << "KelpBundles: preview served at" << opened.url
                << "from share" << name;

    emit previewSessionsChanged();
    return result;
}

void KelpBundles::endPreview(const QString &token)
{
    if (!m_preview || token.isEmpty())
        return;
    m_preview->close(token);
}

int KelpBundles::previewSessions() const
{
    return m_preview ? m_preview->sessionCount() : 0;
}

QVariantMap KelpBundles::storeInfo()
{
    QVariantMap info;
    info.insert(QStringLiteral("ok"), true);
    if (!m_store) {
        info.insert(QStringLiteral("ok"), false);
        return info;
    }
    info.insert(QStringLiteral("path"), m_store->rootPath());
    info.insert(QStringLiteral("objects"), m_store->objectCount());
    info.insert(QStringLiteral("bytes"), m_store->diskUsage());
    info.insert(QStringLiteral("serving"), m_server ? m_server->lanAddress() : QString());
    return info;
}

QVariantMap KelpBundles::storeUsage(const QVariantList &shares)
{
    QVariantMap usage;
    usage.insert(QStringLiteral("ok"), false);

    if (!m_store) {
        usage.insert(QStringLiteral("error"), QStringLiteral("no object store"));
        return usage;
    }

    const QVector<QByteArray> ids = m_store->objectIds();

    QSet<QByteArray> live;
    QString error;
    if (!liveObjects(m_store, shares, &live, &error)) {
        usage.insert(QStringLiteral("error"), error);
        return usage;
    }

    qint64 bytes = 0;
    int inUseObjects = 0;
    qint64 inUseBytes = 0;
    for (int i = 0; i < ids.size(); ++i) {
        const qint64 size = m_store->objectSize(ids.at(i));
        bytes += size;
        if (live.contains(ids.at(i))) {
            ++inUseObjects;
            inUseBytes += size;
        }
    }

    usage.insert(QStringLiteral("ok"), true);
    usage.insert(QStringLiteral("objects"), ids.size());
    usage.insert(QStringLiteral("bytes"), QVariant::fromValue(bytes));
    usage.insert(QStringLiteral("inUseObjects"), inUseObjects);
    usage.insert(QStringLiteral("inUseBytes"), QVariant::fromValue(inUseBytes));
    usage.insert(QStringLiteral("unusedObjects"), ids.size() - inUseObjects);
    usage.insert(QStringLiteral("unusedBytes"), QVariant::fromValue(bytes - inUseBytes));
    return usage;
}

QVariantMap KelpBundles::pruneStore(const QVariantList &shares)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);

    if (!m_store) {
        result.insert(QStringLiteral("error"), QStringLiteral("no object store"));
        return result;
    }

    QSet<QByteArray> live;
    QString error;
    if (!liveObjects(m_store, shares, &live, &error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    int removed = 0;
    qint64 freed = 0;
    const QVector<QByteArray> ids = m_store->objectIds();
    for (int i = 0; i < ids.size(); ++i) {
        if (live.contains(ids.at(i)))
            continue;
        const qint64 size = m_store->objectSize(ids.at(i));
        if (!m_store->remove(ids.at(i)))
            continue;
        ++removed;
        freed += size;
    }

    qInfo() << "KelpBundles: cleaned up" << removed << "unused object(s)," << freed
            << "bytes";

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("removed"), removed);
    result.insert(QStringLiteral("freedBytes"), QVariant::fromValue(freed));
    return result;
}

void KelpBundles::setRelays(const QStringList &relays)
{
    if (m_relays == relays)
        return;
    m_relays = relays;
    qInfo() << "KelpBundles: relays set to" << m_relays;
}

void KelpBundles::unshare(const QString &bundleId)
{
    stopServing(bundleId);
    qInfo() << "KelpBundles: no longer serving" << bundleId;
}

void KelpBundles::pause(const QString &bundleId)
{
    stopServing(bundleId);
    qInfo() << "KelpBundles: paused" << bundleId << "- objects kept";
}

void KelpBundles::stopServing(const QString &bundleId)
{
    // The relay tunnel goes first: it is the one that reaches off-network, and
    // stopping it is what makes "Stop sharing" mean stopped everywhere.
    if (m_tunnels.contains(bundleId)) {
        KelpTunnel *tunnel = m_tunnels.take(bundleId);
        tunnel->stop();
        tunnel->deleteLater();
    }

    if (!m_server)
        return;

    QByteArray id;
    QString error;
    if (!Kelp::Link::base32Decode(bundleId, &id, &error)) {
        qWarning() << "KelpBundles: cannot stop serving" << bundleId << ":" << error;
        return;
    }
    m_server->unshare(id);
}

QVariantMap KelpBundles::republish(const QString &bundleId, const QString &secretText,
                                   const QString &name, int objects, int files,
                                   quint64 bytes)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);

    QByteArray rootId;
    QString error;
    if (!Kelp::Link::base32Decode(bundleId, &rootId, &error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    QByteArray secret;
    if (!Kelp::Link::base64UrlDecode(secretText, &secret, &error)
        || secret.size() != Kelp::Crypto::KeySize) {
        result.insert(QStringLiteral("error"), QStringLiteral("stored key is not usable"));
        return result;
    }

    const Kelp::Link::Bundle link = publish(rootId, secret, name, objects, files, bytes);
    const QString linkText = Kelp::Link::encode(link);

    qInfo() << "KelpBundles: restored" << name << "->" << objects << "objects,"
            << files << "files," << bytes << "bytes";

    // A restored share mints a fresh link (its address may have changed since the
    // row was written), so the link a script reads from the log is this one, by
    // the same rule seal() follows.
    if (linksInLog(m_settings))
        qInfo() << "KelpBundles: link" << linkText;

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("link"), linkText);
    result.insert(QStringLiteral("lan"), link.lan);
    result.insert(QStringLiteral("relay"),
                  m_tunnels.contains(QString::fromLatin1(Kelp::Link::base32Encode(rootId)))
                      ? m_relays.value(0) : QString());
    return result;
}

QVariantMap KelpBundles::linkInfo(const QString &link)
{
    QVariantMap info;
    info.insert(QStringLiteral("ok"), false);

    Kelp::Link::Bundle parsed;
    QString error;
    if (!Kelp::Link::decode(link, &parsed, &error)) {
        info.insert(QStringLiteral("error"), error);
        // The fields consumers read are inserted even on failure, so a binding
        // that re-evaluates mid-change never reads undefined — an unshareable
        // link has a defined (empty) "where", not a missing one.
        info.insert(QStringLiteral("bundleId"), QString());
        info.insert(QStringLiteral("relays"), QStringList());
        info.insert(QStringLiteral("lan"), QString());
        info.insert(QStringLiteral("name"), QString());
        return info;
    }

    info.insert(QStringLiteral("ok"), true);
    info.insert(QStringLiteral("bundleId"),
                QString::fromLatin1(Kelp::Link::base32Encode(parsed.bundleId)));
    info.insert(QStringLiteral("relays"), parsed.relays);
    info.insert(QStringLiteral("lan"), parsed.lan);
    info.insert(QStringLiteral("name"), parsed.name);
    return info;
}

bool KelpBundles::collect(const QString &path, const QString &name, int depth,
                          Kelp::SourceNode *node, int *fileCount, int *skippedLinks,
                          QString *error)
{
    if (depth > kMaxDepth) {
        *error = QStringLiteral("the folder tree in %1 is too deep").arg(path);
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        *error = QStringLiteral("%1 no longer exists").arg(path);
        return false;
    }

    node->name = name;

    if (info.isDir()) {
        node->isDir = true;
        const QDir dir(path);
        const QFileInfoList children =
            dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        for (int i = 0; i < children.size(); ++i) {
            // Following a symlink can walk straight back into the tree we are
            // reading, so links are counted and skipped rather than followed.
            if (children.at(i).isSymLink()) {
                ++(*skippedLinks);
                qWarning() << "KelpBundles: skipping symlink" << children.at(i).absoluteFilePath();
                continue;
            }
            Kelp::SourceNode child;
            if (!collect(children.at(i).absoluteFilePath(), children.at(i).fileName(),
                         depth + 1, &child, fileCount, skippedLinks, error))
                return false;
            node->children.append(child);
        }
        return true;
    }

    node->isDir = false;
    node->path = info.absoluteFilePath();
    node->size = info.size();
    ++(*fileCount);
    return true;
}

QVariantMap KelpBundles::seal(const QStringList &paths, const QString &name)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);

    if (!m_store) {
        result.insert(QStringLiteral("error"), QStringLiteral("no object store"));
        return result;
    }
    if (paths.isEmpty()) {
        result.insert(QStringLiteral("error"), QStringLiteral("nothing to share"));
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    Kelp::SourceNode root;
    root.isDir = true;
    int fileCount = 0;
    int skippedLinks = 0;
    QString error;

    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i);
        Kelp::SourceNode child;
        if (!collect(path, QFileInfo(path).fileName(), 0, &child, &fileCount,
                     &skippedLinks, &error)) {
            qWarning() << "KelpBundles: cannot read" << path << ":" << error;
            result.insert(QStringLiteral("error"), error);
            return result;
        }
        root.children.append(child);
    }

    const QByteArray secret = Kelp::Crypto::randomBytes(Kelp::Crypto::KeySize);
    if (secret.size() != Kelp::Crypto::KeySize) {
        result.insert(QStringLiteral("error"), QStringLiteral("no entropy available"));
        return result;
    }
    const QByteArray key = Kelp::contentKey(secret);
    if (key.size() != Kelp::Crypto::KeySize) {
        result.insert(QStringLiteral("error"), QStringLiteral("key derivation failed"));
        return result;
    }

    KelpFileSource source;
    Kelp::SealedBundle sealed;
    if (!Kelp::seal(m_store, key, root, &source, &sealed, &error)) {
        qWarning() << "KelpBundles: sealing failed:" << error;
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    const Kelp::Link::Bundle link =
        publish(sealed.rootId, secret, name, sealed.objectCount, sealed.fileCount,
                sealed.totalBytes);

    const QString linkText = Kelp::Link::encode(link);

    qInfo() << "KelpBundles: sealed" << name << "->" << sealed.objectCount << "objects,"
            << sealed.fileCount << "files," << sealed.totalBytes << "bytes in"
            << timer.elapsed() << "ms";

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("link"), linkText);
    if (linksInLog(m_settings))
        qInfo() << "KelpBundles: link" << linkText;
    result.insert(QStringLiteral("relay"),
                  m_tunnels.contains(QString::fromLatin1(Kelp::Link::base32Encode(sealed.rootId)))
                      ? m_relays.value(0) : QString());
    result.insert(QStringLiteral("bundleId"),
                  QString::fromLatin1(Kelp::Link::base32Encode(sealed.rootId)));
    result.insert(QStringLiteral("secret"), Kelp::Link::base64UrlEncode(secret));
    result.insert(QStringLiteral("objectCount"), sealed.objectCount);
    result.insert(QStringLiteral("fileCount"), sealed.fileCount);
    result.insert(QStringLiteral("totalBytes"), sealed.totalBytes);
    result.insert(QStringLiteral("skippedLinks"), skippedLinks);
    result.insert(QStringLiteral("lan"), link.lan);
    return result;
}

Kelp::Link::Bundle KelpBundles::publish(const QByteArray &rootId, const QByteArray &secret,
                                        const QString &name, int objects, int files,
                                        quint64 bytes)
{
    Kelp::Link::Bundle link;
    link.bundleId = rootId;
    link.secret = secret;
    link.name = name;

    // Publish on the LAN before minting the link, so `l=` is in it. Failing to
    // listen is not fatal: the objects are sealed either way, the link simply
    // has no address. The whole block is skipped when the user has turned off
    // "Include my local address in the link": a link is handed to whoever the
    // recipient forwards it to, so nothing served here should be discoverable
    // from it, and the link then relies on a relay (Settings → Relay).
    const bool exposeLan = !m_settings || m_settings->includeLanAddress();
    if (m_server && exposeLan) {
        QString listenError;
        if (m_server->start(&listenError)) {
            m_server->share(rootId, Kelp::swarmToken(secret), name, objects, files,
                            bytes);
            link.lan = m_server->lanAddress();
        } else {
            qWarning() << "KelpBundles: cannot serve" << name << "on the LAN:"
                       << listenError;
        }
    } else if (m_server) {
        qInfo() << "KelpBundles: not serving" << name
                << "on the LAN: the local address is excluded";
    }

    // Publish on the relay(s), if any are configured, and put their addresses in
    // the link as `r=`. This is the path that works off-network: the phone dials
    // out and keeps the tunnel open (design §3.2). Failing to reach a relay is
    // not fatal — the link still works on the LAN. A bundle already tunnelled
    // (republish called twice) is left alone rather than tunnelled again.
    const QString tunnelKey = QString::fromLatin1(Kelp::Link::base32Encode(rootId));
    if (m_tunnels.contains(tunnelKey)) {
        link.relays = m_relays;
    } else if (!m_relays.isEmpty()) {
        link.relays = m_relays;
        KelpTunnel *tunnel = new KelpTunnel(m_store, this);
        QString tunnelError;
        if (tunnel->start(m_relays, rootId, Kelp::relayToken(secret), name, objects,
                          files, bytes, &tunnelError)) {
            m_tunnels.insert(tunnelKey, tunnel);
        } else {
            qWarning() << "KelpBundles: cannot publish" << name << "on a relay:"
                       << tunnelError;
            link.relays.clear();
            tunnel->deleteLater();
        }
    }

    return link;
}
