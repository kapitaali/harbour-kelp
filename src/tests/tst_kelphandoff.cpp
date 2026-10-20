/*
 * kelphandoff unit test — the door between a kelp:// tap and the running app.
 *
 * The link is a secret (its fragment is the key) and the socket is a way to make
 * Kelp show a page, so this pins down the two halves of the bargain: the one
 * validation both ends apply (only kelp://, bounded, one line) and the wire
 * behaviour (a live instance takes the link and answers "ok"; anything else is
 * refused with "no"; a second listener cannot exist; the socket is owner-only).
 *
 * The "stale socket" case is a file left where the socket should be, which is
 * what a crash leaves behind: nobody answers, so the next process may replace it.
 *
 * QStandardPaths test mode and a name of its own keep this out of a real app's
 * data directory.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "kelphandoff.h"
#include "kelplog.h"

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

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

#define CHECK(cond) check((cond), #cond, __LINE__)

// A link-shaped string, no real secret: the fragment here is a fixed dummy.
QString link(int suffix = 0)
{
    return QStringLiteral("kelp://aaaaaaaabbbbbbbbccccccccdddddddd#k=dGVzdC1rZXk")
           + (suffix ? QString::number(suffix) : QString());
}

// One raw request, for the cases forward() refuses to send by itself. Returns
// everything the server said before closing the connection.
QByteArray talk(const QByteArray &line)
{
    QLocalSocket socket;
    socket.connectToServer(KelpHandoff::socketPath());
    if (!socket.waitForConnected(1000))
        return QByteArrayLiteral("<no connection>");

    socket.write(line);
    if (!socket.waitForBytesWritten(1000))
        return QByteArrayLiteral("<write failed>");

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);

    QByteArray answer;
    deadline.start(1500);
    while (deadline.isActive()) {
        answer += socket.readAll();
        if (answer.contains('\n') || socket.state() != QLocalSocket::ConnectedState)
            break;
        loop.exec();
    }
    answer += socket.readAll();
    return answer;
}

void testAcceptable()
{
    section("acceptable: only a kelp link, bounded and on one line");
    CHECK(KelpHandoff::acceptable(link()));
    CHECK(KelpHandoff::acceptable(QStringLiteral("KELP://abc")));
    CHECK(!KelpHandoff::acceptable(QString()));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("https://example/b/1#k=x")));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp:/abc")));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp://abc\nkelp://def")));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp://abc\r")));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp://abc def")));
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp://abc") + QChar(0x07)));
    // Over the cap.
    CHECK(!KelpHandoff::acceptable(QStringLiteral("kelp://")
                                   + QString(5000, QLatin1Char('a'))));
}

void testArguments()
{
    section("linkFromArguments: the first kelp link, and nothing else");
    QStringList args;
    args << QStringLiteral("/usr/bin/harbour-kelp") << link();
    CHECK(KelpHandoff::linkFromArguments(args) == link());

    // The program name is never a link, even if it looks like one.
    QStringList looksLikeALink;
    looksLikeALink << link();
    CHECK(KelpHandoff::linkFromArguments(looksLikeALink).isEmpty());

    // Other arguments are ignored; the link may come after them.
    QStringList mixed;
    mixed << QStringLiteral("/usr/bin/harbour-kelp")
          << QStringLiteral("--some-flag")
          << QStringLiteral("https://example/")
          << link(2);
    CHECK(KelpHandoff::linkFromArguments(mixed) == link(2));

    QStringList none;
    none << QStringLiteral("/usr/bin/harbour-kelp") << QStringLiteral("--help");
    CHECK(KelpHandoff::linkFromArguments(none).isEmpty());
}

void testSocket()
{
    const QString path = KelpHandoff::socketPath();
    QLocalServer::removeServer(path);
    QFile::remove(path);

    CHECK(KelpHandoff::socketPath()
          == KelpLog::dataDirectory() + QStringLiteral("/link.sock"));

    section("nobody listening");
    {
        KelpHandoff handoff;
        CHECK(!handoff.instanceRunning());
        QString error;
        CHECK(handoff.forward(link(), &error) == KelpHandoff::NoServer);
    }

    section("a live instance takes the link");
    {
        KelpHandoff handoff;
        QString error;
        CHECK(handoff.listen(true, &error));

        QStringList arrived;
        QObject::connect(&handoff, &KelpHandoff::linkArrived,
                         [&arrived](const QString &text) { arrived.append(text); });

        CHECK(handoff.instanceRunning());
        CHECK(handoff.forward(link(), &error) == KelpHandoff::Sent);
        CHECK(arrived.size() == 1);
        CHECK(arrived.value(0) == link());

        section("a second listener cannot exist");
        {
            KelpHandoff second;
            CHECK(!second.listen(false, &error));
        }

        section("the socket is owner-only");
        {
            CHECK(QFile::exists(path));
            const QFileDevice::Permissions permissions = QFile::permissions(path);
            CHECK((permissions & QFileDevice::ReadOwner) != 0);
            CHECK((permissions & QFileDevice::WriteOwner) != 0);
            CHECK((permissions & QFileDevice::ReadGroup) == 0);
            CHECK((permissions & QFileDevice::WriteGroup) == 0);
            CHECK((permissions & QFileDevice::ReadOther) == 0);
            CHECK((permissions & QFileDevice::WriteOther) == 0);
        }

        section("a non-link on the wire is refused, not taken");
        {
            CHECK(talk("https://example/\n") == QByteArrayLiteral("no\n"));
            CHECK(talk(QByteArray(5000, 'a') + '\n') == QByteArrayLiteral("no\n"));
            CHECK(talk("kelp://abc def\n") == QByteArrayLiteral("no\n"));
            CHECK(arrived.size() == 1);

            // Exactly one message per connection: a second line cannot ride
            // along behind the first.
            CHECK(talk((link(3) + QStringLiteral("\nkelp://evil#k=x\n")).toUtf8())
                  == QByteArrayLiteral("ok\n"));
            CHECK(arrived.size() == 2);
            CHECK(arrived.value(1) == link(3));
        }

        section("a live socket is not stolen even when replacement is allowed");
        {
            // Two cold launches can race: each finds nobody, then one binds. The
            // second asks to replace what it believed was a stale file — and must
            // find the fresh instance instead of removing its socket.
            KelpHandoff greedy;
            CHECK(!greedy.listen(true, &error));
            CHECK(handoff.forward(link(4), &error) == KelpHandoff::Sent);
            CHECK(arrived.size() == 3);
            CHECK(arrived.value(2) == link(4));
        }
    }

    section("the socket is given back when the instance goes");
    CHECK(!QFile::exists(path));
    {
        KelpHandoff handoff;
        CHECK(!handoff.instanceRunning());
        CHECK(handoff.forward(link()) == KelpHandoff::NoServer);
    }

    section("a stale file is not a live instance");
    {
        // What a crash leaves behind, and what connect-to-a-dead-socket looks
        // like: nothing answers, so the next process may take the path.
        QFile stale(path);
        CHECK(stale.open(QIODevice::WriteOnly));
        stale.write("stale");
        stale.close();
        CHECK(QFile::exists(path));

        KelpHandoff handoff;
        CHECK(!handoff.instanceRunning());
        CHECK(handoff.forward(link()) == KelpHandoff::NoServer);
        CHECK(!handoff.listen(false));
        CHECK(handoff.listen(true));
        CHECK(handoff.instanceRunning());
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelphandoff unit test\n");

    // Redirect the app data directory before anything asks for it, and use a
    // name of our own so a real socket and a real share list are never touched.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("harbour.kelp"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp-test"));

    testAcceptable();
    testArguments();
    testSocket();

    QFile::remove(KelpHandoff::socketPath());

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
