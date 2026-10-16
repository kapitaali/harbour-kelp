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
 * No network, no device, no event-loop traffic: it inspects the requests
 * begin()/requestObject() would send, and runs the sender's decode step from
 * libkelp on the header value.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QNetworkRequest>
#include <QString>

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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpfetch unit test\n");

    testLanAuthHeader();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
