/*
 * kelpfetch unit test — the app layer's side of the wire agreement.
 *
 * libkelp's tests prove the crypto and the formats; this proves the one thing
 * only the app can get wrong: that a LAN request built by LanSource carries
 * "Authorization: Bearer <base64url(swarmToken(secret))>", exactly what the
 * sender (kelpserver.cpp) decodes and compares byte for byte.
 *
 * The bug this guards against handed the raw 32 token bytes to the header,
 * which the sender silently answered 401 — indistinguishable from a wrong link
 * by design (no enumeration oracle), so nothing failed loudly.
 *
 * The first half inspects the requests begin()/requestObject() would send, and
 * runs the sender's decode step from libkelp on the header value. The second
 * half runs a real local server to pin the reachability rule: a peer that
 * accepts the connection but never answers is called unreachable, and one that
 * answers — even with a refusal — is not.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QList>
#include <QNetworkRequest>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelpfetch.h"

using namespace Kelp;

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(const QString &value) { return value; }
QString show(int value) { return QString::number(value); }

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

QByteArray testHash(const char *seed)
{
    return Crypto::sha256(QByteArray(seed));
}

void testLanAuthHeader()
{
    section("LAN Authorization header");

    const QByteArray secret = testHash("lan-secret");
    const QByteArray swarm = swarmToken(secret);

    Link::Bundle link;
    link.bundleId = testHash("bundle");
    link.secret = secret;
    link.lan = QStringLiteral("192.168.1.7:7373");
    link.name = QStringLiteral("samples");

    QString error;
    KelpSource *source = KelpSource::create(link, nullptr, &error);
    CHECK(source != nullptr);
    if (!error.isEmpty())
        printf("  create error: %s\n", qPrintable(error));

    // Through the factory, the way the client builds it: this is where the raw
    // bytes used to be handed to the header.
    LanSource *lan = qobject_cast<LanSource *>(source);
    CHECK(lan != nullptr);
    if (!lan) {
        delete source;
        return;
    }
    CHECK_EQ(lan->describe(), link.lan);

    const QByteArray expected =
        QByteArrayLiteral("Bearer ") + Link::base64UrlEncode(swarm).toLatin1();

    // 32 bytes are 43 base64url characters unpadded, plus "Bearer ". Raw bytes
    // would be 7 + 32 — a different length, before even looking at content.
    CHECK_EQ(expected.size(), 7 + 43);

    const QString bundleIdText = QString::fromLatin1(Link::base32Encode(link.bundleId));

    const QNetworkRequest info = lan->infoRequest();
    CHECK_EQ(info.rawHeader("Authorization"), expected);
    CHECK_EQ(info.url().host(), QStringLiteral("192.168.1.7"));
    CHECK_EQ(info.url().port(), 7373);
    CHECK_EQ(info.url().path(),
             QStringLiteral("/v1/bundle/%1/info").arg(bundleIdText));

    const QByteArray objectId = testHash("object");
    const QNetworkRequest object = lan->objectRequest(objectId);
    CHECK_EQ(object.rawHeader("Authorization"), expected);
    CHECK_EQ(object.url().path(),
             QStringLiteral("/v1/bundle/%1/object/%2")
                 .arg(bundleIdText,
                      QString::fromLatin1(Link::base32Encode(objectId))));

    // The sender's side of the agreement (kelpserver.cpp): strip "Bearer ",
    // base64url-decode, compare bytes.
    const QByteArray header = info.rawHeader("Authorization");
    CHECK(header.startsWith(QByteArrayLiteral("Bearer ")));
    QByteArray presented;
    QString tokenError;
    CHECK(Link::base64UrlDecode(QString::fromLatin1(header.mid(7)),
                                &presented, &tokenError));
    CHECK_EQ(presented, swarm);

    // The bug, pinned: the raw token bytes are not a header, and whatever they
    // decode to (or fail to) is not the token — which is why the sender 401'd.
    CHECK(QByteArrayLiteral("Bearer ") + swarm != expected);
    QByteArray rawPresented;
    const bool rawDecodes = Link::base64UrlDecode(QString::fromLatin1(swarm),
                                                  &rawPresented, &tokenError);
    CHECK(!rawDecodes || rawPresented != swarm);

    delete source;

    // A link naming neither an address nor a relay is not fetchable.
    Link::Bundle bare;
    bare.bundleId = testHash("bare");
    bare.secret = testHash("bare-secret");
    CHECK(KelpSource::create(bare, nullptr, &error) == nullptr);
    CHECK(!error.isEmpty());
}

// A link whose only address is a port on the loopback, so a test can point it at
// a server of its own making.
Link::Bundle localLink(quint16 port, const char *seed)
{
    Link::Bundle link;
    link.bundleId = testHash(seed);
    link.secret = Crypto::sha256(QByteArray(seed) + QByteArrayLiteral("-secret"));
    link.lan = QStringLiteral("127.0.0.1:%1").arg(port);
    link.name = QStringLiteral("probe");
    return link;
}

// Runs begin() to a verdict and returns what the source reported, plus how long
// it took. `guardMs` is a backstop so a regression hangs the test rather than CI.
QString fetchVerdict(KelpSource *source, int guardMs, qint64 *elapsedMs)
{
    QString message;
    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QObject::connect(source, &KelpSource::failed, &loop, [&](const QString &text) {
        message = text;
        loop.quit();
    });
    QTimer::singleShot(guardMs, &loop, &QEventLoop::quit);
    source->begin();
    loop.exec();
    *elapsedMs = clock.elapsed();
    return message;
}

void testUnreachableSender()
{
    section("a sender that never answers is called unreachable");

    // Accept the connection, then say nothing: exactly the shape of an address
    // that can be dialled but not heard from — a host behind a NAT with no way
    // in. A refused connection fails on its own; this is the case that used to
    // hang until KelpClient's 20 s silence deadline and then blame the sender
    // for "stopping" when it had never been heard from at all.
    QTcpServer blackhole;
    CHECK(blackhole.listen(QHostAddress::LocalHost, 0));
    QList<QTcpSocket *> held;                // keep accepted sockets open
    QObject::connect(&blackhole, &QTcpServer::newConnection, [&]() {
        while (blackhole.hasPendingConnections())
            held.append(blackhole.nextPendingConnection());
    });

    QString error;
    KelpSource *source =
        KelpSource::create(localLink(blackhole.serverPort(), "blackhole"), nullptr, &error);
    LanSource *lan = qobject_cast<LanSource *>(source);
    CHECK(lan != nullptr);
    if (!lan) {
        delete source;
        return;
    }
    lan->setConnectTimeout(1000);        // seconds in the app; milliseconds here

    qint64 elapsed = 0;
    const QString message = fetchVerdict(lan, 8000, &elapsed);

    CHECK(!message.isEmpty());
    CHECK(message.contains(QStringLiteral("could not reach the sender")));
    CHECK(!message.contains(QStringLiteral("stopped answering")));
    CHECK(elapsed < 5000);               // our timeout fired, not the guard

    delete source;
}

void testReachableButRejecting()
{
    section("a reachable sender is not called unreachable");

    // A server that answers — with a refusal, which is all a wrong token gets by
    // design. The point is the *shape* of the report: the sender was heard, so
    // it is named, not accused of being unreachable.
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        while (server.hasPendingConnections()) {
            QTcpSocket *peer = server.nextPendingConnection();
            QObject::connect(peer, &QTcpSocket::readyRead, peer, [peer]() {
                if (peer->property("answered").toBool())
                    return;
                peer->setProperty("answered", true);
                peer->readAll();         // the GET request
                peer->write("HTTP/1.1 401 Unauthorized\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n\r\n");
                peer->flush();
            });
        }
    });

    QString error;
    KelpSource *source =
        KelpSource::create(localLink(server.serverPort(), "reject"), nullptr, &error);
    LanSource *lan = qobject_cast<LanSource *>(source);
    CHECK(lan != nullptr);
    if (!lan) {
        delete source;
        return;
    }
    // Longer than the answer takes: proof the reachability probe stopped rather
    // than the timeout simply not having fired yet.
    lan->setConnectTimeout(3000);

    qint64 elapsed = 0;
    const QString message = fetchVerdict(lan, 6000, &elapsed);

    CHECK(!message.isEmpty());
    CHECK(message.contains(QStringLiteral("did not recognise")));
    CHECK(!message.contains(QStringLiteral("could not reach")));
    CHECK(elapsed < 3000);               // answered, so it did not wait the probe

    delete source;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpfetch unit test\n");

    testLanAuthHeader();
    testUnreachableSender();
    testReachableButRejecting();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
