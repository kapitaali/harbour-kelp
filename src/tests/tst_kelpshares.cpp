/*
 * kelpshares unit test — the share list has to survive a restart.
 *
 * A restart used to forget every share silently. Persisting the rows is what
 * stops that, but the stored rows are not harmless data: each one carries a
 * share's secret (the decryption key that is also in the link's fragment). So
 * the two things this test pins down are that a round trip is faithful — the
 * link can be minted again from what was written — and that the file is not
 * readable by anyone but its owner.
 *
 * QStandardPaths test mode redirects the app data directory to a throwaway
 * location, so this writes a real file without touching the user's.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileDevice>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "kelplog.h"
#include "kelpshares.h"

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

QVariantMap makeRow(const QString &name, const QString &bundleId, const QString &secret,
                    int objects, int files, qint64 bytes, bool swarm)
{
    QVariantMap row;
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("kind"), QStringLiteral("file"));
    row.insert(QStringLiteral("files"), files);
    row.insert(QStringLiteral("size"), QStringLiteral("2.0 MB"));
    row.insert(QStringLiteral("bytes"), bytes);
    row.insert(QStringLiteral("path"), QStringLiteral("/home/defaultuser/") + name);
    row.insert(QStringLiteral("swarm"), swarm);
    row.insert(QStringLiteral("objects"), objects);
    row.insert(QStringLiteral("bundleId"), bundleId);
    row.insert(QStringLiteral("secret"), secret);
    return row;
}

void testShares()
{
    const QString path = KelpShares::filePath();
    QFile::remove(path);

    section("missing file is an empty list");
    {
        KelpShares shares;
        // Beside the log, in the directory Sailjail provisions.
        CHECK_EQ(KelpShares::filePath(),
                 KelpLog::dataDirectory() + QStringLiteral("/shares.json"));
        CHECK(!QFile::exists(path));
        CHECK_EQ(shares.load().size(), 0);
    }

    section("round trip");
    {
        KelpShares shares;
        QVariantList rows;
        rows.append(makeRow(QStringLiteral("holiday.jpg"), QStringLiteral("aaaa"),
                            QStringLiteral("c2VjcmV0LWE"), 11, 1, 2136168, false));
        rows.append(makeRow(QStringLiteral("receipts"), QStringLiteral("bbbb"),
                            QStringLiteral("c2VjcmV0LWI"), 7, 3, 4096, true));
        CHECK(shares.save(rows));
        CHECK(QFile::exists(path));
    }

    // Loading in a fresh object is the restart path: it must read from disk, not
    // from the rows the writer held in memory.
    section("reads back after a restart");
    {
        KelpShares shares;
        const QVariantList rows = shares.load();
        CHECK_EQ(rows.size(), 2);

        const QVariantMap first = rows.at(0).toMap();
        CHECK_EQ(first.value(QStringLiteral("name")).toString(),
                 QStringLiteral("holiday.jpg"));
        CHECK_EQ(first.value(QStringLiteral("kind")).toString(), QStringLiteral("file"));
        CHECK_EQ(first.value(QStringLiteral("files")).toInt(), 1);
        CHECK_EQ(first.value(QStringLiteral("objects")).toInt(), 11);
        CHECK_EQ(first.value(QStringLiteral("bytes")).toLongLong(), Q_INT64_C(2136168));
        CHECK_EQ(first.value(QStringLiteral("swarm")).toBool(), false);
        // The secrets are the load-bearing part: without them the link cannot be
        // minted again, and the share is unrecoverable.
        CHECK_EQ(first.value(QStringLiteral("bundleId")).toString(), QStringLiteral("aaaa"));
        CHECK_EQ(first.value(QStringLiteral("secret")).toString(),
                 QStringLiteral("c2VjcmV0LWE"));

        const QVariantMap second = rows.at(1).toMap();
        CHECK_EQ(second.value(QStringLiteral("name")).toString(),
                 QStringLiteral("receipts"));
        CHECK_EQ(second.value(QStringLiteral("files")).toInt(), 3);
        CHECK_EQ(second.value(QStringLiteral("swarm")).toBool(), true);
        CHECK_EQ(second.value(QStringLiteral("secret")).toString(),
                 QStringLiteral("c2VjcmV0LWI"));
    }

    section("owner-only");
    {
        const QFileDevice::Permissions perms = QFile::permissions(path);
        const QFileDevice::Permissions others =
            QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther;
        CHECK((perms & others) == 0);
        CHECK((perms & QFileDevice::ReadOwner) != 0);
    }

    section("empty list clears the file");
    {
        KelpShares shares;
        CHECK(shares.save(QVariantList()));
        CHECK_EQ(shares.load().size(), 0);
    }

    section("a corrupt file is not fatal");
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ this is not json");
        file.close();

        KelpShares shares;
        // Startup must not be wedged by a half-written file: it reads as empty.
        CHECK_EQ(shares.load().size(), 0);
    }

    QFile::remove(path);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpshares unit test\n");

    // Redirect the app data directory before anything asks for it, and use a
    // name of our own so a developer's real share list is never touched.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("harbour.kelp"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp-test"));

    testShares();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
