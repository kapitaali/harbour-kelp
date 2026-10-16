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

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
