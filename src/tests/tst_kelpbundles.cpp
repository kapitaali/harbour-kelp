/*
 * kelpbundles unit test — the "Include my local address in the link" gate.
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
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "kelp_link.h"
#include "kelpbundles.h"
#include "kelplog.h"
#include "kelpserver.h"
#include "kelpsettings.h"
#include "kelpstore.h"

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

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
    return bundles.seal(QStringList() << file, name);
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

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
