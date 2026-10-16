#include "kelpbundles.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelpserver.h"
#include "kelpsettings.h"
#include "kelpstore.h"
#include "kelptunnel.h"

namespace {

// Deep enough for any real folder; there to stop a symlink loop.
const int kMaxDepth = 64;

} // namespace

KelpBundles::KelpBundles(KelpObjectStore *store, KelpServer *server, QObject *parent)
    : QObject(parent), m_store(store), m_server(server)
{
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

void KelpBundles::setRelays(const QStringList &relays)
{
    if (m_relays == relays)
        return;
    m_relays = relays;
    qInfo() << "KelpBundles: relays set to" << m_relays;
}

void KelpBundles::unshare(const QString &bundleId)
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
        qWarning() << "KelpBundles: cannot unshare" << bundleId << ":" << error;
        return;
    }
    m_server->unshare(id);
    qInfo() << "KelpBundles: no longer serving" << bundleId;
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

    qInfo() << "KelpBundles: restored" << name << "->" << objects << "objects,"
            << files << "files," << bytes << "bytes";

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("link"), Kelp::Link::encode(link));
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

    qInfo() << "KelpBundles: sealed" << name << "->" << sealed.objectCount << "objects,"
            << sealed.fileCount << "files," << sealed.totalBytes << "bytes in"
            << timer.elapsed() << "ms";

    const QString linkText = Kelp::Link::encode(link);

    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("link"), linkText);
    // A link is a capability — whoever holds it can read the bundle — so it is
    // never logged by default. It is written only when the user turns on
    // Settings → "Include links in the log" (which is what lets a device with no
    // usable clipboard be handed a link) or when KELP_DEBUG_LINK is set in the
    // environment for a one-off run. The env override stays so existing headless
    // scripts keep working without touching the UI.
    const bool logLink = (m_settings && m_settings->includeLinksInLog())
                         || !qgetenv("KELP_DEBUG_LINK").isEmpty();
    if (logLink)
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
    // has no address.
    if (m_server) {
        QString listenError;
        if (m_server->start(&listenError)) {
            m_server->share(rootId, Kelp::swarmToken(secret), objects, files, bytes);
            link.lan = m_server->lanAddress();
        } else {
            qWarning() << "KelpBundles: cannot serve" << name << "on the LAN:"
                       << listenError;
        }
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
        if (tunnel->start(m_relays, rootId, Kelp::relayToken(secret), objects, files,
                          bytes, &tunnelError)) {
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
