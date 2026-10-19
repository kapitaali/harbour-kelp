/*
 * kelpbundles unit test — the two gates that decide what leaves this device.
 *
 * A link is handed to whoever the user sends it to (and to whoever they forward
 * it to), so the publisher must be able to withhold this device's local address.
 * The switch in Settings is what decides that; this test drives the real publish
 * path (KelpBundles::seal -> KelpServer) to prove the address really is left out
 * of the link when it is off — and that it comes back when it is on.
 *
 * The subtle case is a listener that is *already* up from an earlier share: the
 * address must still be withheld from the next link, so the gate has to be on
 * what goes into the link, not merely on whether a socket exists.
 *
 * The other gate is the log: a link carries the decryption key, so writing one
 * into the log is a deliberate act — the user's switch in a dev build, never in a
 * release whatever the switch says (src/kelpbuild.h).
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QList>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "kelp_link.h"
#include "kelpbuild.h"
#include "kelpbundles.h"
#include "kelpfiles.h"
#include "kelplog.h"
#include "kelppreviewserver.h"
#include "kelpserver.h"
#include "kelpsettings.h"
#include "kelpstore.h"

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;
// Set by the message handler the link-log section installs; never holds the link
// itself, only the fact that one was about to be written.
bool g_sawLink = false;

QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(const QString &value) { return value; }
QString show(int value) { return QString::number(value); }
QString show(qint64 value) { return QString::number(value); }
QString show(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }

void section(const char *name)
{
    g_section = QString::fromLatin1(name);
    printf("\n== %s\n", name);
}

void check(bool condition, const char *expression, int line)
{
    ++g_checks;
    if (condition)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s\n", qPrintable(g_section), line, expression);
}

template <typename A, typename B>
void checkEqual(const A &actual, const B &expected, const char *expression, int line)
{
    ++g_checks;
    if (actual == expected)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s\n    actual:   %s\n    expected: %s\n",
           qPrintable(g_section), line, expression,
           qPrintable(show(actual)), qPrintable(show(expected)));
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __LINE__)

// True when the link carries a LAN address. The address rides sealed in `q=`,
// so this decodes the link rather than looking for a readable `l=`.
bool linkNamesLan(const QString &link)
{
    Kelp::Link::Bundle parsed;
    QString error;
    if (!Kelp::Link::decode(link, &parsed, &error))
        return false;
    return !parsed.lan.isEmpty();
}

QString writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(content);
    file.close();
    return path;
}

// One HTTP GET by hand, so the test talks to the real listener exactly as a
// recipient does. Kelp binds a specific interface, not every address, so the
// caller has to dial the address the server reports — loopback would be refused.
//
// The listener lives in this same thread, so the event loop has to keep turning
// while we wait, or it never accepts the connection and every reply times out.
struct HttpReply {
    int status = 0;
    QByteArray body;
};

HttpReply httpGet(const QString &host, quint16 port, const QString &path,
                  const QString &token)
{
    HttpReply reply;

    QByteArray request = "GET " + path.toUtf8() + " HTTP/1.1\r\n";
    request += "Host: " + host.toUtf8() + "\r\n";
    if (!token.isEmpty())
        request += "Authorization: Bearer " + token.toUtf8() + "\r\n";
    request += "Connection: close\r\n\r\n";

    QTcpSocket socket;
    QByteArray response;
    QEventLoop loop;

    QObject::connect(&socket, &QTcpSocket::connected, [&socket, &request]() {
        socket.write(request);
        socket.flush();
    });
    QObject::connect(&socket, &QTcpSocket::readyRead,
                     [&socket, &response]() { response += socket.readAll(); });

    // The listener answers and hangs up, so the response ends with the connection.
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    // Sailfish's Qt is 5.6: the error signal has no `errorOccurred` name yet and is
    // overloaded, so the socket-error one is picked by hand.
    QObject::connect(&socket,
                     static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                         &QAbstractSocket::error),
                     &loop, &QEventLoop::quit);

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    socket.connectToHost(host, port);
    timeout.start(5000);
    loop.exec();
    response += socket.readAll();

    const int statusEnd = response.indexOf("\r\n");
    if (statusEnd > 0) {
        const QList<QByteArray> parts = response.left(statusEnd).split(' ');
        if (parts.size() >= 2)
            reply.status = parts.at(1).toInt();
    }
    const int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart >= 0)
        reply.body = response.mid(bodyStart + 4);
    return reply;
}

} // namespace

// Seals one small file from this store's own directory, so each case is
// independent.
static QVariantMap sealOne(KelpBundles &bundles, const QString &dir, const QString &name)
{
    const QString file = writeFile(dir + QStringLiteral("/") + name + QStringLiteral(".txt"),
                                   (name + QStringLiteral(": some bytes\n")).toUtf8());
    return bundles.seal(QStringList() << file, name, false);
}

static void testPublish()
{
    // The switch persists, and this test turns it off — so a previous run leaves
    // include_local_address=false on disk. Start from a clean slate so the
    // "included by default" case really tests the default.
    QFile::remove(KelpSettings::filePath());

    const QString base = KelpLog::dataDirectory() + QStringLiteral("/bundles-test");
    QDir().mkpath(base);

    section("local address included by default");
    {
        QDir().mkpath(base + "/a");
        KelpObjectStore store(base + "/a/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);

        const QVariantMap result = sealOne(bundles, base + "/a", "bundle-a");
        CHECK(result.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(settings.includeLanAddress(), true);

        // The listener bound a real interface, and the link names it.
        CHECK(server.isListening());
        CHECK(!result.value(QStringLiteral("lan")).toString().isEmpty());
        CHECK(linkNamesLan(result.value(QStringLiteral("link")).toString()));

        // The address in the link is the one the server reports.
        Kelp::Link::Bundle parsed;
        CHECK(Kelp::Link::decode(result.value(QStringLiteral("link")).toString(),
                                 &parsed, &error));
        CHECK_EQ(parsed.lan, result.value(QStringLiteral("lan")).toString());
    }

    section("local address excluded");
    {
        QDir().mkpath(base + "/b");
        KelpObjectStore store(base + "/b/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);
        settings.setIncludeLanAddress(false);

        const QVariantMap result = sealOne(bundles, base + "/b", "bundle-b");
        CHECK(result.value(QStringLiteral("ok")).toBool());

        // Not served, and the link carries no address at all.
        CHECK_EQ(server.isListening(), false);
        CHECK_EQ(result.value(QStringLiteral("lan")).toString(), QString());
        CHECK(!linkNamesLan(result.value(QStringLiteral("link")).toString()));

        // It is still a link: the content address and the key are there, so a
        // relay can carry it.
        Kelp::Link::Bundle parsed;
        CHECK(Kelp::Link::decode(result.value(QStringLiteral("link")).toString(),
                                 &parsed, &error));
        CHECK_EQ(parsed.lan, QString());
        CHECK_EQ(parsed.secret.size(), 32);
    }

    section("excluded even when a listener is already up");
    {
        QDir().mkpath(base + "/c");
        KelpObjectStore store(base + "/c/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);

        // Share once with the address on: this binds the listener.
        settings.setIncludeLanAddress(true);
        const QVariantMap on = sealOne(bundles, base + "/c", "on");
        CHECK(server.isListening());
        CHECK(linkNamesLan(on.value(QStringLiteral("link")).toString()));

        // Turn it off: the listener stays up for the first share, but the next
        // link must not reveal the address.
        settings.setIncludeLanAddress(false);
        const QVariantMap off = sealOne(bundles, base + "/c", "off");
        CHECK(server.isListening());
        CHECK_EQ(off.value(QStringLiteral("lan")).toString(), QString());
        CHECK(!linkNamesLan(off.value(QStringLiteral("link")).toString()));
    }

    // A link carries the decryption key, so writing one into the log is a
    // deliberate act: the user's switch decides it in a dev build, and a release
    // build never does it, whatever the switch says. The log is a Qt message
    // stream (KelpLog chains to it), so a message handler sees exactly what would
    // have been written — and, because it replaces the default handler, keeps the
    // link out of this test's own output.
    section("a link reaches the log only when the build allows it");
    {
        QDir().mkpath(base + "/d");
        KelpObjectStore store(base + "/d/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);

        // The one-off environment override is not part of this contract.
        qunsetenv("KELP_DEBUG_LINK");
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
            if (message.startsWith(QStringLiteral("KelpBundles: link")))
                g_sawLink = true;
        });

        settings.setIncludeLinksInLog(true);
        const QVariantMap on = sealOne(bundles, base + "/d", "on");
        CHECK(on.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(g_sawLink, KelpBuild::devExtras());

        g_sawLink = false;
        settings.setIncludeLinksInLog(false);
        const QVariantMap off = sealOne(bundles, base + "/d", "off");
        CHECK(off.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(g_sawLink, false);

        qInstallMessageHandler(nullptr);
    }

    QDir(base).removeRecursively();
    // Leave nothing behind: the preference this test toggles is written outside
    // the store directory.
    QFile::remove(KelpSettings::filePath());
}

/*
 * Storage management (design §12.7): the store is shared by every share and by
 * whatever a fetch leaves behind, and nothing ever deleted from it. These checks
 * drive the real store on disk — an object that is not reachable from a share is
 * the only thing a clean-up may touch, and a share that cannot be read must stop
 * the whole thing rather than have its objects mistaken for junk.
 */
static void testStoreManagement()
{
    QFile::remove(KelpSettings::filePath());

    const QString base = KelpLog::dataDirectory() + QStringLiteral("/storage-test");
    QDir().mkpath(base);

    section("storage: what the shares need, and what is left over");
    {
        QDir().mkpath(base + "/a");
        KelpObjectStore store(base + "/a/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);

        const QVariantMap sealed = sealOne(bundles, base + "/a", "bundle-a");
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        const int objects = sealed.value(QStringLiteral("objectCount")).toInt();
        CHECK(objects > 0);
        CHECK_EQ(store.objectCount(), objects);

        // The row the Bundles page holds, in the shape the storage calls want.
        QVariantMap row;
        row.insert(QStringLiteral("bundleId"), sealed.value(QStringLiteral("bundleId")));
        row.insert(QStringLiteral("secret"), sealed.value(QStringLiteral("secret")));
        QVariantList shares;
        shares << row;

        // Everything here belongs to the one share, so nothing is unused.
        QVariantMap usage = bundles.storeUsage(shares);
        CHECK(usage.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(usage.value(QStringLiteral("objects")).toInt(), objects);
        CHECK_EQ(usage.value(QStringLiteral("inUseObjects")).toInt(), objects);
        CHECK_EQ(usage.value(QStringLiteral("unusedObjects")).toInt(), 0);

        // A stray object — what a fetch leaves behind, say — is not named by any
        // share, so it is the only thing a clean-up may touch.
        QByteArray strayId;
        CHECK(store.put(QByteArray("leftover from something else"), &strayId));
        CHECK(store.contains(strayId));

        usage = bundles.storeUsage(shares);
        CHECK_EQ(usage.value(QStringLiteral("objects")).toInt(), objects + 1);
        CHECK_EQ(usage.value(QStringLiteral("unusedObjects")).toInt(), 1);
        CHECK(usage.value(QStringLiteral("unusedBytes")).toLongLong() > 0);

        // Clean up: the stray goes, every object the share needs stays.
        const QVariantMap pruned = bundles.pruneStore(shares);
        CHECK(pruned.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(pruned.value(QStringLiteral("removed")).toInt(), 1);
        CHECK_EQ(store.objectCount(), objects);
        CHECK(!store.contains(strayId));

        // The share is still whole, root and all.
        Kelp::Link::Bundle parsed;
        CHECK(Kelp::Link::decode(sealed.value(QStringLiteral("link")).toString(),
                                 &parsed, &error));
        CHECK(store.contains(parsed.bundleId));

        usage = bundles.storeUsage(shares);
        CHECK_EQ(usage.value(QStringLiteral("inUseObjects")).toInt(), objects);
        CHECK_EQ(usage.value(QStringLiteral("unusedObjects")).toInt(), 0);

        // Nothing left to do is not a failure: the second clean-up removes zero
        // and says so.
        const QVariantMap again = bundles.pruneStore(shares);
        CHECK(again.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(again.value(QStringLiteral("removed")).toInt(), 0);

        // A shard that loses its last object goes with it: the store is a tree
        // of one-directory-per-byte-value, and pruning must not leave a
        // growing pile of empty ones behind (§12.7).
        QDir(base + "/lone/objects").removeRecursively();
        KelpObjectStore lone(base + "/lone/objects");
        CHECK(lone.initialise(&error));
        QByteArray loneId;
        CHECK(lone.put(QByteArray("the only object in this store"), &loneId));
        const QString loneShard = QFileInfo(lone.pathForId(loneId)).absolutePath();
        CHECK(QDir(loneShard).exists());
        CHECK(lone.remove(loneId));
        CHECK(!QDir(loneShard).exists());

        // A shard left empty by an older build goes on the next clean-up, not
        // only when its last object is removed: the sweep is what makes an
        // already-untidy store tidy again.
        const QString staleShard = store.rootPath() + QStringLiteral("/00");
        QDir().mkpath(staleShard);
        CHECK(QDir(staleShard).exists());
        const QVariantMap swept = bundles.pruneStore(shares);
        CHECK(swept.value(QStringLiteral("ok")).toBool());
        CHECK(!QDir(staleShard).exists());
    }

    section("storage: a share that cannot be read stops the clean-up");
    {
        QDir().mkpath(base + "/b");
        KelpObjectStore store(base + "/b/objects");
        QString error;
        CHECK(store.initialise(&error));

        KelpServer server(&store);
        KelpBundles bundles(&store, &server);
        KelpSettings settings;
        bundles.setSettings(&settings);

        const QVariantMap sealed = sealOne(bundles, base + "/b", "bundle-b");
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        const int objects = sealed.value(QStringLiteral("objectCount")).toInt();

        QByteArray strayId;
        CHECK(store.put(QByteArray("another leftover"), &strayId));

        // A row whose stored key no longer opens the manifests. The app cannot tell
        // this share's objects from junk, so it must refuse to delete anything —
        // including the stray — rather than guess.
        QVariantMap row;
        row.insert(QStringLiteral("bundleId"), sealed.value(QStringLiteral("bundleId")));
        row.insert(QStringLiteral("secret"),
                   Kelp::Link::base64UrlEncode(QByteArray(32, '\0')));
        QVariantList shares;
        shares << row;

        const QVariantMap usage = bundles.storeUsage(shares);
        CHECK_EQ(usage.value(QStringLiteral("ok")).toBool(), false);
        CHECK(!usage.value(QStringLiteral("error")).toString().isEmpty());

        const QVariantMap pruned = bundles.pruneStore(shares);
        CHECK_EQ(pruned.value(QStringLiteral("ok")).toBool(), false);
        CHECK_EQ(pruned.value(QStringLiteral("removed")).toInt(), 0);
        CHECK_EQ(store.objectCount(), objects + 1);
        CHECK(store.contains(strayId));
    }

    QDir(base).removeRecursively();
    QFile::remove(KelpSettings::filePath());
}

/*
 * The publisher's own view of a fetch (§3.14). The server is stateless, so before
 * this a recipient that fetched every object and one that never connected left
 * exactly the same trace: none. These checks drive the real listener over a real
 * socket, so the tally is built by the same code the device runs.
 */
static void testServingVisibility()
{
    QFile::remove(KelpSettings::filePath());

    const QString base = KelpLog::dataDirectory() + QStringLiteral("/serving-test");
    QDir().mkpath(base + "/a");

    KelpObjectStore store(base + "/a/objects");
    QString error;
    CHECK(store.initialise(&error));

    KelpServer server(&store);
    KelpBundles bundles(&store, &server);
    KelpSettings settings;
    bundles.setSettings(&settings);

    section("serving: what each peer asked for is recorded");
    {
        const QVariantMap sealed = sealOne(bundles, base + "/a", "served");
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        CHECK(server.isListening());

        const QString bundleIdText = sealed.value(QStringLiteral("bundleId")).toString();
        QByteArray rootId;
        QString why;
        CHECK(Kelp::Link::base32Decode(bundleIdText, &rootId, &why));

        QByteArray secret;
        CHECK(Kelp::Link::base64UrlDecode(sealed.value(QStringLiteral("secret")).toString(),
                                          &secret, &why));
        const QString token = Kelp::Link::base64UrlEncode(Kelp::swarmToken(secret));

        const QString lan = server.lanAddress();
        const int colon = lan.lastIndexOf(QLatin1Char(':'));
        CHECK(colon > 0);
        const QString host = lan.left(colon);
        const quint16 port = quint16(lan.mid(colon + 1).toUInt());

        // A fetch opens with /info. That is the "someone is here" signal, and it
        // carries the name so the log line is readable.
        const HttpReply info =
            httpGet(host, port, QStringLiteral("/v1/bundle/%1/info").arg(bundleIdText),
                    token);
        CHECK_EQ(info.status, 200);
        CHECK(info.body.contains("\"objects\""));

        QHash<QString, KelpServer::Traffic> traffic = server.traffic();
        CHECK_EQ(traffic.size(), 1);
        const QString peer = traffic.keys().value(0);
        CHECK_EQ(traffic.value(peer).requests, qint64(1));
        CHECK_EQ(traffic.value(peer).bundle, QStringLiteral("served"));
        CHECK_EQ(traffic.value(peer).objects, qint64(0));
        CHECK_EQ(traffic.value(peer).bytes, qint64(0));

        // One object, and the counters say exactly what left the device.
        const HttpReply object =
            httpGet(host, port,
                    QStringLiteral("/v1/bundle/%1/object/%2")
                        .arg(bundleIdText,
                             QString::fromLatin1(Kelp::Link::base32Encode(rootId))),
                    token);
        CHECK_EQ(object.status, 200);
        CHECK(!object.body.isEmpty());

        traffic = server.traffic();
        CHECK_EQ(traffic.value(peer).objects, qint64(1));
        CHECK_EQ(traffic.value(peer).bytes, qint64(object.body.size()));
        CHECK_EQ(traffic.value(peer).requests, qint64(1));

        // A wrong token gives nothing away and is counted, not served.
        const HttpReply refused =
            httpGet(host, port, QStringLiteral("/v1/bundle/%1/info").arg(bundleIdText),
                    Kelp::Link::base64UrlEncode(QByteArray(32, 'x')));
        CHECK_EQ(refused.status, 401);
        CHECK(refused.body.isEmpty() || !refused.body.contains("\"objects\""));

        traffic = server.traffic();
        CHECK_EQ(traffic.value(peer).refused, qint64(1));
        CHECK_EQ(traffic.value(peer).requests, qint64(1));
        CHECK_EQ(traffic.value(peer).objects, qint64(1));

        // An unknown bundle is a refusal too, and is not counted as traffic.
        const HttpReply unknown =
            httpGet(host, port,
                    QStringLiteral("/v1/bundle/%1/info")
                        .arg(QString::fromLatin1(Kelp::Link::base32Encode(
                            QByteArray(32, 'z')))),
                    token);
        CHECK_EQ(unknown.status, 401);
        CHECK_EQ(server.traffic().value(peer).refused, qint64(2));
    }

    QDir(base).removeRecursively();
    QFile::remove(KelpSettings::filePath());
}

/*
 * Pause and resume (user-requested): a share can be silenced without being
 * deleted. These checks drive the real listener, so "paused" is proved by a
 * recipient's request being refused, not by a flag the app keeps to itself — and
 * "resumed" by the same request working again.
 */
static void testPauseResume()
{
    QFile::remove(KelpSettings::filePath());

    const QString base = KelpLog::dataDirectory() + QStringLiteral("/pause-test");
    QDir().mkpath(base + "/a");

    KelpObjectStore store(base + "/a/objects");
    QString error;
    CHECK(store.initialise(&error));

    KelpServer server(&store);
    KelpBundles bundles(&store, &server);
    KelpSettings settings;
    bundles.setSettings(&settings);

    const QVariantMap sealed = sealOne(bundles, base + "/a", "pausable");
    CHECK(sealed.value(QStringLiteral("ok")).toBool());
    const QString bundleIdText = sealed.value(QStringLiteral("bundleId")).toString();
    const int objects = sealed.value(QStringLiteral("objectCount")).toInt();

    QByteArray rootId;
    QString why;
    CHECK(Kelp::Link::base32Decode(bundleIdText, &rootId, &why));

    QByteArray secret;
    CHECK(Kelp::Link::base64UrlDecode(sealed.value(QStringLiteral("secret")).toString(),
                                      &secret, &why));
    const QString token = Kelp::Link::base64UrlEncode(Kelp::swarmToken(secret));

    const QString lan = server.lanAddress();
    const int colon = lan.lastIndexOf(QLatin1Char(':'));
    CHECK(colon > 0);
    const QString host = lan.left(colon);
    const quint16 port = quint16(lan.mid(colon + 1).toUInt());
    const QString infoPath = QStringLiteral("/v1/bundle/%1/info").arg(bundleIdText);

    QVariantMap row;
    row.insert(QStringLiteral("bundleId"), bundleIdText);
    row.insert(QStringLiteral("secret"), sealed.value(QStringLiteral("secret")));
    QVariantList shares;
    shares << row;

    section("pause: a fetch is refused, the objects stay");
    {
        CHECK_EQ(httpGet(host, port, infoPath, token).status, 200);

        bundles.pause(bundleIdText);

        // A recipient now gets the same 401 it would get for a bundle that was
        // never here — pausing gives nothing away.
        CHECK_EQ(httpGet(host, port, infoPath, token).status, 401);

        // Nothing was thrown away, so the share can be read back and served again.
        CHECK(store.contains(rootId));
        CHECK_EQ(store.objectCount(), objects);
        // A paused share is not "gone" for storage purposes: its objects are still
        // in use, so a clean-up must leave them alone.
        CHECK_EQ(bundles.storeUsage(shares).value(QStringLiteral("unusedObjects")).toInt(), 0);
    }

    section("resume: serving again on a fresh link");
    {
        const QVariantMap resumed = bundles.republish(
            bundleIdText, sealed.value(QStringLiteral("secret")).toString(),
            QStringLiteral("pausable"), objects,
            sealed.value(QStringLiteral("fileCount")).toInt(),
            sealed.value(QStringLiteral("totalBytes")).toLongLong(),
            false);
        CHECK(resumed.value(QStringLiteral("ok")).toBool());
        CHECK(!resumed.value(QStringLiteral("lan")).toString().isEmpty());
        CHECK(linkNamesLan(resumed.value(QStringLiteral("link")).toString()));
        CHECK_EQ(httpGet(host, port, infoPath, token).status, 200);
    }

    QDir(base).removeRecursively();
    QFile::remove(KelpSettings::filePath());
}

// The preview API: which file a bundle offers a browser, and serving it on
// loopback (design §5.1, §5.3). The server has its own suite for what it does
// with a request; this is the seam the UI actually calls.
static void testPreview()
{
    const QString base = KelpLog::dataDirectory() + QStringLiteral("/bundles-test");
    QDir().mkpath(base + "/preview/site");

    KelpObjectStore store(base + "/preview/objects");
    QString error;
    CHECK(store.initialise(&error));

    KelpServer server(&store);
    KelpBundles bundles(&store, &server);
    KelpSettings settings;
    bundles.setSettings(&settings);

    KelpPreviewServer preview;
    bundles.setPreviewServer(&preview);

    // A folder with a page in it, sealed through the same path the Add page uses.
    writeFile(base + "/preview/site/index.html",
              "<!doctype html><title>Kelp</title><p>hello from a sealed bundle</p>\n");
    writeFile(base + "/preview/site/style.css", "p { color: teal; }\n");

    section("preview: which file is the page");
    const QVariantMap sealed = bundles.seal(QStringList() << (base + "/preview/site"),
                                            QStringLiteral("site"), false);
    CHECK(sealed.value(QStringLiteral("ok")).toBool());
    const QString bundleId = sealed.value(QStringLiteral("bundleId")).toString();
    const QString secret = sealed.value(QStringLiteral("secret")).toString();
    CHECK(!bundleId.isEmpty());
    CHECK(!secret.isEmpty());

    // Asked without serving anything, and before any listener exists.
    CHECK(!preview.isListening());
    const QVariantMap info = bundles.pageInfo(bundleId, secret);
    CHECK(info.value(QStringLiteral("ok")).toBool());
    CHECK(info.value(QStringLiteral("hasPage")).toBool());
    CHECK_EQ(info.value(QStringLiteral("entry")).toString(), QStringLiteral("site/index.html"));

    // A share whose stored key is unusable has no *knowable* page, which is not
    // the same as a bundle with no page: the answer must not look like "none".
    const QVariantMap broken = bundles.pageInfo(bundleId, QStringLiteral("not-a-key"));
    CHECK(!broken.value(QStringLiteral("ok")).toBool());
    CHECK(!broken.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(!broken.value(QStringLiteral("hasPage")).toBool());

    section("preview: serving the page");
    CHECK_EQ(bundles.previewSessions(), 0);
    const QVariantMap opened = bundles.previewPage(bundleId, secret,
                                                   QStringLiteral("site"), QString());
    CHECK(opened.value(QStringLiteral("ok")).toBool());
    if (!opened.value(QStringLiteral("ok")).toBool()) {
        printf("  preview error: %s\n",
               qPrintable(opened.value(QStringLiteral("error")).toString()));
        return;
    }
    CHECK_EQ(bundles.previewSessions(), 1);
    CHECK(preview.isListening());
    CHECK(preview.port() > 0);
    CHECK_EQ(opened.value(QStringLiteral("entry")).toString(), QStringLiteral("site/index.html"));

    // The URL the UI would hand the browser is the loopback listener this device
    // is actually running, and the page really comes back through it.
    const QUrl url(opened.value(QStringLiteral("url")).toString());
    CHECK_EQ(url.host(), QStringLiteral("127.0.0.1"));
    CHECK_EQ(url.port(), preview.port());
    CHECK(url.path().endsWith(QStringLiteral("/")));

    const HttpReply page = httpGet(url.host(), quint16(url.port()), url.path(), QString());
    CHECK_EQ(page.status, 200);
    CHECK(page.body.contains("hello from a sealed bundle"));

    // The page's own stylesheet, at the path the page names it by.
    const HttpReply css = httpGet(url.host(), quint16(url.port()),
                                  url.path() + QStringLiteral("style.css"), QString());
    CHECK_EQ(css.status, 200);
    CHECK_EQ(css.body, QByteArray("p { color: teal; }\n"));

    // A path that is not in the bundle is a 404, and the token is the whole
    // address: an unknown one is the same 404.
    const HttpReply missing = httpGet(url.host(), quint16(url.port()),
                                      url.path() + QStringLiteral("nope.png"), QString());
    CHECK_EQ(missing.status, 404);
    const HttpReply stranger = httpGet(url.host(), quint16(url.port()),
                                       QStringLiteral("/0123456789/"), QString());
    CHECK_EQ(stranger.status, 404);

    // Ending the session closes the URL at once: the capability is the session.
    bundles.endPreview(opened.value(QStringLiteral("token")).toString());
    CHECK_EQ(bundles.previewSessions(), 0);
    const HttpReply after = httpGet(url.host(), quint16(url.port()), url.path(), QString());
    CHECK_EQ(after.status, 404);

    QDir(base + "/preview").removeRecursively();
}

/*
 * Webpage shares (design §5.5): a publisher's declared kind, and the check made
 * before a folder can be declared one. The kind rides sealed in the link's `q=`
 * block, so it costs the link text nothing and survives a link that has no
 * address at all; the check is the preview's own rule (§5.3) applied to names, so
 * a folder the app calls a webpage is one a recipient's preview can find.
 */
static void testWebpageShare()
{
    QFile::remove(KelpSettings::filePath());

    const QString base = KelpLog::dataDirectory() + QStringLiteral("/bundles-test");
    QDir().mkpath(base + "/web/site/pages");
    QDir().mkpath(base + "/web/plain");

    KelpObjectStore store(base + "/web/objects");
    QString error;
    CHECK(store.initialise(&error));

    KelpServer server(&store);
    KelpBundles bundles(&store, &server);
    KelpSettings settings;
    bundles.setSettings(&settings);

    KelpFiles files;

    writeFile(base + "/web/site/index.html", "<!doctype html><title>site</title>\n");
    writeFile(base + "/web/site/pages/about.html", "<p>about</p>\n");
    writeFile(base + "/web/plain/notes.txt", "no page here\n");

    section("a folder is checked before it can be called a webpage");
    {
        const QVariantMap good = files.pageEntry(base + "/web/site");
        CHECK(good.value(QStringLiteral("ok")).toBool());
        CHECK(good.value(QStringLiteral("hasPage")).toBool());
        CHECK_EQ(good.value(QStringLiteral("entry")).toString(),
                 QStringLiteral("index.html"));
        CHECK_EQ(good.value(QStringLiteral("candidates")).toStringList().size(), 2);

        const QVariantMap none = files.pageEntry(base + "/web/plain");
        CHECK(none.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(none.value(QStringLiteral("hasPage")).toBool(), false);

        // A folder that is not there, and a file where a folder belongs: the
        // question could not be answered, which is not the same as "no page".
        const QVariantMap missing = files.pageEntry(base + "/web/nothing");
        CHECK_EQ(missing.value(QStringLiteral("ok")).toBool(), false);
        CHECK(!missing.value(QStringLiteral("error")).toString().isEmpty());
        const QVariantMap notAFolder = files.pageEntry(base + "/web/site/index.html");
        CHECK_EQ(notAFolder.value(QStringLiteral("ok")).toBool(), false);
        CHECK(!notAFolder.value(QStringLiteral("error")).toString().isEmpty());
    }

    section("a webpage share says so in the link");
    {
        const QVariantMap sealed = bundles.seal(QStringList() << (base + "/web/site"),
                                                QStringLiteral("site"), true);
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(sealed.value(QStringLiteral("page")).toBool(), true);

        const QVariantMap info =
            bundles.linkInfo(sealed.value(QStringLiteral("link")).toString());
        CHECK(info.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(info.value(QStringLiteral("page")).toBool(), true);
        CHECK(!info.value(QStringLiteral("lan")).toString().isEmpty());

        // The sealed bundle's own page is the folder's, one level down: sealing
        // puts the picked entry under its own name.
        const QVariantMap page = bundles.pageInfo(
            sealed.value(QStringLiteral("bundleId")).toString(),
            sealed.value(QStringLiteral("secret")).toString());
        CHECK(page.value(QStringLiteral("hasPage")).toBool());
        CHECK_EQ(page.value(QStringLiteral("entry")).toString(),
                 QStringLiteral("site/index.html"));

        // A page share with no address and no label: the `q=` block is minted for
        // the kind alone, so "this is a page" survives a relay-only link.
        settings.setIncludeLanAddress(false);
        const QVariantMap bare = bundles.seal(QStringList() << (base + "/web/site"),
                                              QString(), true);
        CHECK(bare.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(bare.value(QStringLiteral("lan")).toString(), QString());
        Kelp::Link::Bundle parsed;
        CHECK(Kelp::Link::decode(bare.value(QStringLiteral("link")).toString(),
                                 &parsed, &error));
        CHECK_EQ(parsed.lan, QString());
        CHECK_EQ(parsed.name, QString());
        CHECK_EQ(parsed.page, true);
        settings.setIncludeLanAddress(true);
    }

    section("an ordinary share is not called a webpage");
    {
        const QVariantMap sealed = bundles.seal(QStringList() << (base + "/web/site"),
                                                QStringLiteral("site-as-files"), false);
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(sealed.value(QStringLiteral("page")).toBool(), false);
        const QVariantMap info =
            bundles.linkInfo(sealed.value(QStringLiteral("link")).toString());
        CHECK_EQ(info.value(QStringLiteral("page")).toBool(), false);

        // It still holds a page — that is a fact about its contents, and the
        // recipient-side offer to open it (design §5.4) rests on it.
        const QVariantMap page = bundles.pageInfo(
            sealed.value(QStringLiteral("bundleId")).toString(),
            sealed.value(QStringLiteral("secret")).toString());
        CHECK(page.value(QStringLiteral("hasPage")).toBool());
    }

    section("a restored webpage share is still a webpage share");
    {
        const QVariantMap sealed = bundles.seal(QStringList() << (base + "/web/site"),
                                                QStringLiteral("web-restored"), true);
        const QVariantMap again = bundles.republish(
            sealed.value(QStringLiteral("bundleId")).toString(),
            sealed.value(QStringLiteral("secret")).toString(),
            QStringLiteral("web-restored"),
            sealed.value(QStringLiteral("objectCount")).toInt(),
            sealed.value(QStringLiteral("fileCount")).toInt(),
            sealed.value(QStringLiteral("totalBytes")).toLongLong(), true);
        CHECK(again.value(QStringLiteral("ok")).toBool());
        CHECK(linkNamesLan(again.value(QStringLiteral("link")).toString()));
        const QVariantMap info =
            bundles.linkInfo(again.value(QStringLiteral("link")).toString());
        CHECK_EQ(info.value(QStringLiteral("page")).toBool(), true);
    }

    section("a link that cannot be read still answers the question");
    {
        const QVariantMap info = bundles.linkInfo(QStringLiteral("https://example.invalid/"));
        CHECK_EQ(info.value(QStringLiteral("ok")).toBool(), false);
        CHECK_EQ(info.value(QStringLiteral("page")).toBool(), false);
    }

    section("a passphrase share: the link opens nothing on its own");
    {
        const QVariantMap sealed = bundles.seal(QStringList() << (base + "/web/site"),
                                                QStringLiteral("protected-site"), false,
                                                QStringLiteral("correct horse battery staple"));
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        const QString link = sealed.value(QStringLiteral("link")).toString();
        CHECK(link.contains(QStringLiteral("#p=")));
        CHECK(!link.contains(QStringLiteral("#k=")));
        CHECK_EQ(link.count(QStringLiteral("#")), 1);

        // Nothing readable is left: not the address, not the label — and not even
        // that this folder holds a page. Everything but the content address and
        // the relay names is sealed under the key the passphrase unwraps.
        const QString lan = sealed.value(QStringLiteral("lan")).toString();
        CHECK(!lan.isEmpty());
        CHECK(!link.contains(lan));
        CHECK(!link.contains(QStringLiteral("protected-site")));

        // What the row keeps is the wrapped blob, and the link carries the same
        // bytes: that is what lets a restart mint the same link again without
        // anyone re-typing the passphrase.
        const QString passBlob = sealed.value(QStringLiteral("passBlob")).toString();
        CHECK(!passBlob.isEmpty());
        QByteArray blob;
        CHECK(Kelp::Link::base64UrlDecode(passBlob, &blob, &error));
        CHECK_EQ(blob.size(), int(Kelp::Link::WrappedSize));
        CHECK_EQ(blob.size(), 81);
        CHECK_EQ(blob.left(1), QByteArray(1, char(Kelp::Link::WrappedVersion)));
        int iterations = 0;
        QByteArray salt;
        CHECK(Kelp::Link::wrappedParameters(blob, &iterations, &salt));
        CHECK_EQ(iterations, int(Kelp::Link::DefaultIterations));
        CHECK_EQ(iterations, 600000);
        CHECK_EQ(salt.size(), int(Kelp::Link::SaltSize));

        // What the link can still say about itself: the content address, the relay
        // names, and that it needs a passphrase. Everything else is empty because
        // decode() has no key to open it with.
        const QVariantMap info = bundles.linkInfo(link);
        CHECK(info.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(info.value(QStringLiteral("needsPassphrase")).toBool(), true);
        CHECK_EQ(info.value(QStringLiteral("bundleId")).toString(),
                 sealed.value(QStringLiteral("bundleId")).toString());
        CHECK_EQ(info.value(QStringLiteral("lan")).toString(), QString());
        CHECK_EQ(info.value(QStringLiteral("name")).toString(), QString());
        CHECK_EQ(info.value(QStringLiteral("page")).toBool(), false);

        // Unlock: a wrong passphrase is refused, the right one opens exactly the
        // hints that were sealed under the key.
        const QVariantMap wrong = bundles.unlockLink(link, QStringLiteral("wrong"));
        CHECK_EQ(wrong.value(QStringLiteral("ok")).toBool(), false);
        CHECK(!wrong.value(QStringLiteral("error")).toString().isEmpty());
        CHECK_EQ(wrong.value(QStringLiteral("needsPassphrase")).toBool(), true);

        const QVariantMap opened =
            bundles.unlockLink(link, QStringLiteral("correct horse battery staple"));
        CHECK(opened.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(opened.value(QStringLiteral("needsPassphrase")).toBool(), false);
        CHECK_EQ(opened.value(QStringLiteral("lan")).toString(), lan);
        CHECK_EQ(opened.value(QStringLiteral("name")).toString(),
                 QStringLiteral("protected-site"));
        CHECK_EQ(opened.value(QStringLiteral("bundleId")).toString(),
                 sealed.value(QStringLiteral("bundleId")).toString());

        // An ordinary share carries no wrapped secret at all, and is not something
        // to unlock.
        const QVariantMap ordinary = bundles.seal(QStringList() << (base + "/web/site"),
                                                  QStringLiteral("plain-site"), false);
        CHECK(ordinary.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(ordinary.value(QStringLiteral("passBlob")).toString(), QString());
        CHECK(linkNamesLan(ordinary.value(QStringLiteral("link")).toString()));
        CHECK_EQ(bundles
                     .unlockLink(ordinary.value(QStringLiteral("link")).toString(),
                                 QStringLiteral("anything"))
                     .value(QStringLiteral("ok"))
                     .toBool(),
                 false);
    }

    section("a restored passphrase share is still protected, by the same blob");
    {
        const QVariantMap sealed = bundles.seal(QStringList() << (base + "/web/site"),
                                                QStringLiteral("protected-restored"), false,
                                                QStringLiteral("a long enough passphrase"));
        CHECK(sealed.value(QStringLiteral("ok")).toBool());
        const QString passBlob = sealed.value(QStringLiteral("passBlob")).toString();
        CHECK(!passBlob.isEmpty());

        const QVariantMap again = bundles.republish(
            sealed.value(QStringLiteral("bundleId")).toString(),
            sealed.value(QStringLiteral("secret")).toString(),
            QStringLiteral("protected-restored"),
            sealed.value(QStringLiteral("objectCount")).toInt(),
            sealed.value(QStringLiteral("fileCount")).toInt(),
            sealed.value(QStringLiteral("totalBytes")).toLongLong(), false, passBlob);
        CHECK(again.value(QStringLiteral("ok")).toBool());
        const QString link = again.value(QStringLiteral("link")).toString();
        CHECK(link.contains(QStringLiteral("#p=")));

        // The blob is the row's, byte for byte, and the link still carries it in
        // the wrapped form rather than the key.
        Kelp::Link::Bundle parsed;
        CHECK(Kelp::Link::decode(link, &parsed, &error));
        CHECK_EQ(Kelp::Link::base64UrlEncode(parsed.wrappedSecret), passBlob);
        CHECK_EQ(parsed.secret.size(), 0);
        CHECK(Kelp::Link::needsPassphrase(parsed));

        // The passphrase a recipient was told still opens it; nothing else does.
        const QVariantMap opened =
            bundles.unlockLink(link, QStringLiteral("a long enough passphrase"));
        CHECK(opened.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(opened.value(QStringLiteral("name")).toString(),
                 QStringLiteral("protected-restored"));
        CHECK_EQ(bundles.unlockLink(link, QStringLiteral("a long enough passphras"))
                     .value(QStringLiteral("ok"))
                     .toBool(),
                 false);

        // A stored blob that cannot be read is refused rather than published as an
        // ordinary link: failing open would hand out the key.
        const QVariantMap bad = bundles.republish(
            sealed.value(QStringLiteral("bundleId")).toString(),
            sealed.value(QStringLiteral("secret")).toString(),
            QStringLiteral("protected-restored"), 1, 1, 1, false,
            QStringLiteral("not-a-blob"));
        CHECK_EQ(bad.value(QStringLiteral("ok")).toBool(), false);
        CHECK(!bad.value(QStringLiteral("error")).toString().isEmpty());
        CHECK_EQ(bad.value(QStringLiteral("link")).toString(), QString());
    }

    section("a passphrase shorter than the floor is refused before sealing");
    {
        const QVariantMap refused = bundles.seal(QStringList() << (base + "/web/plain"),
                                                 QStringLiteral("too-short"), false,
                                                 QStringLiteral("short"));
        CHECK_EQ(refused.value(QStringLiteral("ok")).toBool(), false);
        CHECK(refused.value(QStringLiteral("error")).toString()
                  .contains(QStringLiteral("8")));
        CHECK_EQ(refused.value(QStringLiteral("link")).toString(), QString());
        CHECK_EQ(refused.value(QStringLiteral("bundleId")).toString(), QString());
    }

    QDir(base + "/web").removeRecursively();
    QFile::remove(KelpSettings::filePath());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpbundles unit test\n");

    // A throwaway data directory, so the real preferences and object store are
    // never touched.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("harbour.kelp"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp-bundles-test"));

    testPublish();
    testStoreManagement();
    testServingVisibility();
    testPauseResume();
    testPreview();
    testWebpageShare();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
