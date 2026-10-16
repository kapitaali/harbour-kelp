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
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
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

// True when the link names a LAN address, as `?l=...` or `&l=...` (relays come
// first in the query, so `l` is not necessarily first).
bool linkNamesLan(const QString &link)
{
    return link.contains(QStringLiteral("?l=")) || link.contains(QStringLiteral("&l="));
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

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
