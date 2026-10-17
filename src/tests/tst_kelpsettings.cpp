/*
 * kelpsettings unit test — the preference has to survive a restart.
 *
 * The "Include links in the log" switch persists, and its default follows the
 * dev-extras bundle (src/kelpbuild.h): on in a dev build, so a device with no
 * usable clipboard can be handed a link, and off in a release, where the switch
 * is not offered at all. Either way a preference that silently failed to be
 * stored — or was stored somewhere the sandbox hides — would look identical to a
 * working one until the next restart, which is what this test is for. Run it once
 * per build mode (the `.pro` follows `KELP_DEV_EXTRAS`, as the app does): the
 * default it checks is the build's, so both answers are covered.
 *
 * QStandardPaths test mode redirects the app data directory to a throwaway
 * location, so this reads and writes a real file without touching the user's.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QString>

#include "kelpbuild.h"
#include "kelplog.h"
#include "kelpsettings.h"

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

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

void testSettings()
{
    const QString path = KelpSettings::filePath();
    QFile::remove(path);

    section("defaults");
    {
        KelpSettings settings;
        // The default follows the build's dev-extras bundle: on in a dev build
        // (so a link can be read back where there is no clipboard), off in a
        // release, where the switch is not offered either.
        CHECK_EQ(settings.includeLinksInLog(), KelpBuild::devExtras());
        // The local address is exposed unless the user opts out: it is what the
        // same-network fast path needs.
        CHECK_EQ(settings.includeLanAddress(), true);
        // Lives in the directory Sailjail provisions (beside the log), not in
        // the ~/.config location the sandbox hides.
        CHECK_EQ(KelpSettings::filePath(),
                 KelpLog::dataDirectory() + QStringLiteral("/settings.conf"));
        CHECK(!QFile::exists(path));
    }

    section("round trip");
    int changes = 0;
    {
        KelpSettings settings;
        QObject::connect(&settings, &KelpSettings::includeLinksInLogChanged,
                         [&changes]() { ++changes; });

        // Toggle *away* from whatever the build's default is, so this exercises
        // the write path in a dev build and a release build alike.
        const bool other = !settings.includeLinksInLog();
        settings.setIncludeLinksInLog(other);
        CHECK_EQ(changes, 1);
        CHECK_EQ(settings.includeLinksInLog(), other);
        CHECK(QFile::exists(path));
        CHECK(readFile(path).contains(QByteArray("include_links=")
                                      + (other ? "true" : "false")));

        settings.setIncludeLinksInLog(other);   // unchanged: no signal, no write
        CHECK_EQ(changes, 1);
    }

    // A fresh instance after the first is gone is the restart path: it must read
    // the value back off disk, not from the object that wrote it.
    section("survives a restart");
    {
        KelpSettings settings;
        CHECK_EQ(settings.includeLinksInLog(), !KelpBuild::devExtras());

        const bool bundleDefault = KelpBuild::devExtras();
        settings.setIncludeLinksInLog(bundleDefault);
        CHECK_EQ(settings.includeLinksInLog(), bundleDefault);
        CHECK(readFile(path).contains(QByteArray("include_links=")
                                      + (bundleDefault ? "true" : "false")));
    }
    {
        KelpSettings settings;
        CHECK_EQ(settings.includeLinksInLog(), KelpBuild::devExtras());
    }

    section("local address");
    {
        // Still the default after the file exists but the key was never written.
        KelpSettings settings;
        CHECK_EQ(settings.includeLanAddress(), true);

        int changes = 0;
        QObject::connect(&settings, &KelpSettings::includeLanAddressChanged,
                         [&changes]() { ++changes; });

        settings.setIncludeLanAddress(false);
        CHECK_EQ(changes, 1);
        CHECK_EQ(settings.includeLanAddress(), false);
        CHECK(readFile(path).contains("include_local_address=false"));

        settings.setIncludeLanAddress(false);   // already false: no signal, no write
        CHECK_EQ(changes, 1);
    }
    {
        // Read back off disk, like the restart path above.
        KelpSettings settings;
        CHECK_EQ(settings.includeLanAddress(), false);
        settings.setIncludeLanAddress(true);
        CHECK_EQ(settings.includeLanAddress(), true);
    }

    section("download folder");
    {
        KelpSettings settings;
        CHECK_EQ(settings.downloadDirectory(), QString());
        settings.setDownloadDirectory(QStringLiteral("/home/defaultuser/Documents/inbox"));
        CHECK_EQ(settings.downloadDirectory(),
                 QStringLiteral("/home/defaultuser/Documents/inbox"));
    }
    {
        // Read back off disk, like the restart path above.
        KelpSettings settings;
        CHECK_EQ(settings.downloadDirectory(),
                 QStringLiteral("/home/defaultuser/Documents/inbox"));
        settings.setDownloadDirectory(QString());
        CHECK_EQ(settings.downloadDirectory(), QString());
    }

    QFile::remove(path);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpsettings unit test\n");

    // Redirect the app data directory before anything asks for it, and use a
    // name of our own so a developer's real preferences are never touched.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("harbour.kelp"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp-test"));

    testSettings();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
