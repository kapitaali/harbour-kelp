/*
 * kelpsettings unit test — the preference has to survive a restart.
 *
 * The whole point of the "Include links in the log" switch is that it persists:
 * a link carries the decryption key, so it is off by default, and a device with
 * no usable clipboard turns it on once and keeps it on across launches. A
 * preference that silently fails to be stored — or stored somewhere the sandbox
 * hides — would look identical to a working one until the next restart.
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
        CHECK_EQ(settings.includeLinksInLog(), false);
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

        settings.setIncludeLinksInLog(true);
        CHECK_EQ(changes, 1);
        CHECK_EQ(settings.includeLinksInLog(), true);
        CHECK(QFile::exists(path));
        CHECK(readFile(path).contains("include_links=true"));

        settings.setIncludeLinksInLog(true);   // already true: no signal, no write
        CHECK_EQ(changes, 1);
    }

    // A fresh instance after the first is gone is the restart path: it must read
    // the value back off disk, not from the object that wrote it.
    section("survives a restart");
    {
        KelpSettings settings;
        CHECK_EQ(settings.includeLinksInLog(), true);

        settings.setIncludeLinksInLog(false);
        CHECK_EQ(settings.includeLinksInLog(), false);
        CHECK(readFile(path).contains("include_links=false"));
    }
    {
        KelpSettings settings;
        CHECK_EQ(settings.includeLinksInLog(), false);
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
