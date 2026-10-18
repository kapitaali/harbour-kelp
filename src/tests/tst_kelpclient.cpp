/*
 * kelpclient unit test — the receiving end of a link (design §5.4, §5.5).
 *
 * KelpClient has two endings for one walk. An ordinary link is *saved*: the
 * objects are verified into the app's store and then written out as files. A
 * webpage link (the publisher's `p` hint, §5.5) is *viewed*: the objects go into
 * a memory store belonging to that view, nothing is written anywhere, and the
 * page is served to this device's browser from 127.0.0.1 through the same
 * preview listener a share uses.
 *
 * What only this suite can prove, and the reason it exists:
 *
 *   1. a view writes nothing — not to the download folder, not to the app store;
 *   2. the page ceiling is enforced from /info *before* any object is requested,
 *      and again against what actually arrives, because /info is a claim;
 *   3. a second view does not free the first session's store (the session holds
 *      its own reference), which is what makes "the session ends on its own idle
 *      timeout" safe rather than a use-after-free;
 *   4. a link that says "page" over a bundle with no page is refused with the
 *      §5.5 wording, and keeps nothing;
 *   5. the save path is unchanged by any of it.
 *
 * Everything is off-device: a stub publisher on 127.0.0.1 answers /info and
 * /object/<id> the way the wire spec says a sender does. No browser, no GUI, no
 * emulator.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_manifest.h"
#include "kelp_store.h"

#include "kelpclient.h"
#include "kelppreviewserver.h"

using namespace Kelp;

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(const QString &value) { return value; }

// Any arithmetic type prints as itself; the overloads above win where they apply.
template <typename T>
QString show(T value) { return QString::number(value); }

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

// ---- sealing a bundle into a store -----------------------------------------

QByteArray pattern(const char *seed, int size)
{
    QByteArray out(size, '\0');
    const unsigned base = static_cast<unsigned char>(seed[0]);
    for (int i = 0; i < size; ++i)
        out[i] = char((unsigned(i) * 7u + base * 13u) & 0xffu);
    return out;
}

// File content, in memory: the sealer reads one chunk at a time through this.
class MapSource : public ContentSource
{
public:
    QHash<QString, QByteArray> files;

    bool read(const QString &path, qint64 offset, qint64 length, QByteArray *out,
              QString *error) override
    {
        if (!files.contains(path)) {
            *error = QStringLiteral("no such test file: %1").arg(path);
            return false;
        }
        const QByteArray &data = files.value(path);
        if (offset < 0 || length < 0 || offset + length > data.size()) {
            *error = QStringLiteral("read out of range: %1").arg(path);
            return false;
        }
        *out = data.mid(int(offset), int(length));
        return true;
    }
};

SourceNode fileNode(const QString &name, const QString &path, qint64 size)
{
    SourceNode node;
    node.name = name;
    node.isDir = false;
    node.path = path;
    node.size = size;
    return node;
}

SourceNode dirNode(const QString &name)
{
    SourceNode node;
    node.name = name;
    node.isDir = true;
    return node;
}

struct Sealed {
    QByteArray rootId;
    QByteArray secret;
    QByteArray html;          // the page, when there is one (else empty)
    QString bundleId;
    QString secretText;
};

// A sealed folder named "site", holding a page (index.html + a stylesheet) or
// holding none (one text file), under a secret derived from `seed`.
bool sealSite(MemoryStore *store, const QString &seed, bool withPage, Sealed *out,
              QString *error)
{
    MapSource source;

    SourceNode root = dirNode(QStringLiteral("site"));

    if (withPage) {
        out->html = QByteArrayLiteral("<!doctype html>\n<title>") + seed.toUtf8()
                    + QByteArrayLiteral("</title>\n<p>this is the page</p>\n");
        source.files.insert(QStringLiteral("site/index.html"), out->html);
        root.children.append(fileNode(QStringLiteral("index.html"),
                                      QStringLiteral("site/index.html"),
                                      out->html.size()));

        const QByteArray css = QByteArrayLiteral("p { color: teal; }\n");
        source.files.insert(QStringLiteral("site/style.css"), css);
        root.children.append(fileNode(QStringLiteral("style.css"),
                                      QStringLiteral("site/style.css"), css.size()));
    } else {
        const QByteArray notes = QByteArrayLiteral("no page here, only notes\n");
        source.files.insert(QStringLiteral("site/notes.txt"), notes);
        root.children.append(fileNode(QStringLiteral("notes.txt"),
                                      QStringLiteral("site/notes.txt"), notes.size()));
    }

    out->secret = Crypto::sha256(seed.toUtf8());
    const QByteArray key = contentKey(out->secret);

    SealedBundle sealed;
    if (!seal(store, key, root, &source, &sealed, error))
        return false;

    out->rootId = sealed.rootId;
    out->bundleId = Link::base32Encode(sealed.rootId);
    out->secretText = Link::base64UrlEncode(out->secret);
    return true;
}

// A sealed single file, for the save path's regression check.
bool sealFile(MemoryStore *store, const QString &seed, QString *bundleId,
              QByteArray *rootId, QByteArray *secret, QString *error)
{
    MapSource source;
    const QByteArray data = pattern("single", 300);
    source.files.insert(QStringLiteral("site/note.txt"), data);

    SourceNode root = dirNode(QStringLiteral("site"));
    root.children.append(fileNode(QStringLiteral("note.txt"),
                                  QStringLiteral("site/note.txt"), data.size()));

    *secret = Crypto::sha256(seed.toUtf8());
    SealedBundle sealed;
    if (!seal(store, contentKey(*secret), root, &source, &sealed, error))
        return false;

    *rootId = sealed.rootId;
    *bundleId = Link::base32Encode(sealed.rootId);
    return true;
}

// ---- the stub publisher -----------------------------------------------------

// A sender, as the wire spec describes one: /v1/bundle/<id>/info and
// /v1/bundle/<id>/object/<id>, the bearer token ignored (this is about the
// client's decisions, not the sender's authentication). It counts what it was
// asked for, which is how "refused before a byte was fetched" is checked, and it
// can claim whatever size it likes — /info is a claim, not a promise.
class StubPublisher : public QObject
{
    Q_OBJECT

public:
    explicit StubPublisher(MemoryStore *store) : m_store(store)
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, &StubPublisher::onConnection);
    }

    quint16 port() const { return m_server.serverPort(); }
    int infoRequests() const { return m_infoRequests; }
    int objectRequests() const { return m_objectRequests; }

    void setClaimedBytes(qint64 bytes) { m_claimedBytes = bytes; }
    void setClaimedFiles(int files) { m_claimedFiles = files; }

private slots:
    void onConnection()
    {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *socket = m_server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { read(socket); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

private:
    void read(QTcpSocket *socket)
    {
        // Keep-alive: QNetworkAccessManager may send several requests down one
        // connection, so what is left over is kept for the next read.
        QByteArray buffer = socket->property("kelpBuffer").toByteArray();
        buffer += socket->readAll();
        for (;;) {
            const int end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                break;
            const QByteArray head = buffer.left(end);
            buffer.remove(0, end + 4);
            answer(socket, head);
        }
        socket->setProperty("kelpBuffer", buffer);
    }

    void answer(QTcpSocket *socket, const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = first.size() >= 2 ? QString::fromLatin1(first.at(1)) : QString();

        QByteArray body;
        int status = 200;

        if (path.endsWith(QStringLiteral("/info"))) {
            ++m_infoRequests;
            QJsonObject info;
            info.insert(QStringLiteral("v"), 1);
            info.insert(QStringLiteral("objects"), m_store->count());
            info.insert(QStringLiteral("files"),
                        m_claimedFiles >= 0 ? m_claimedFiles : 1);
            info.insert(QStringLiteral("bytes"), double(m_claimedBytes >= 0
                                                        ? m_claimedBytes
                                                        : m_store->byteCount()));
            body = QJsonDocument(info).toJson(QJsonDocument::Compact);
        } else {
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            QByteArray id;
            QString why;
            QByteArray object;
            if (slash < 0
                || !Link::base32Decode(path.mid(slash + 1), &id, &why)
                || !m_store->get(id, &object)) {
                status = 404;
                body = QByteArrayLiteral("no such object\n");
            } else {
                ++m_objectRequests;
                body = object;
            }
        }

        const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " OK\r\n"
            + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            + "Content-Type: application/octet-stream\r\n"
            + "\r\n" + body;
        socket->write(response);
        socket->flush();
    }

    QTcpServer m_server;
    MemoryStore *m_store;
    int m_infoRequests = 0;
    int m_objectRequests = 0;
    qint64 m_claimedBytes = -1;
    int m_claimedFiles = -1;
};

// ---- the HTTP client this test speaks through -------------------------------

struct Reply {
    int status = 0;
    QByteArray body;
};

Reply http(int port, const QByteArray &request)
{
    Reply reply;

    QTcpSocket socket;
    QByteArray response;
    QEventLoop loop;

    QObject::connect(&socket, &QTcpSocket::connected, [&socket, &request]() {
        socket.write(request);
        socket.flush();
    });
    QObject::connect(&socket, &QTcpSocket::readyRead,
                     [&socket, &response]() { response += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket,
                     static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                         &QAbstractSocket::error),
                     [&loop](QAbstractSocket::SocketError) { loop.quit(); });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);

    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    loop.exec();
    socket.abort();

    const int end = response.indexOf("\r\n\r\n");
    if (end < 0)
        return reply;

    reply.body = response.mid(end + 4);
    const QList<QByteArray> statusLine = response.left(end).split('\n').value(0).trimmed().split(' ');
    if (statusLine.size() >= 2)
        reply.status = statusLine.at(1).toInt();
    return reply;
}

Reply get(int port, const QByteArray &path)
{
    return http(port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
}

// ---- driving the client ------------------------------------------------------

// The client is asynchronous, so the test spins the event loop until it settles.
// Returns false if it is still busy when the time is up.
bool settle(KelpClient *client, int ms = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (client->busy() && timer.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !client->busy();
}

QString linkFor(const Sealed &sealed, const StubPublisher &stub, bool page)
{
    Link::Bundle link;
    link.bundleId = sealed.rootId;
    link.secret = sealed.secret;
    link.lan = QStringLiteral("127.0.0.1:%1").arg(stub.port());
    link.name = QStringLiteral("site");
    link.page = page;
    return Link::encode(link);
}

int entriesIn(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
}

// ---- the sections ------------------------------------------------------------

void testViewFetchesToMemory()
{
    section("view: a webpage link is fetched into memory and writes nothing");

    MemoryStore sealed;
    Sealed site;
    QString error;
    CHECK(sealSite(&sealed, QStringLiteral("client-view"), true, &site, &error));
    StubPublisher stub(&sealed);

    QTemporaryDir downloads;
    CHECK(downloads.isValid());

    MemoryStore appStore;                        // the app's own store
    KelpClient client(&appStore);
    client.setDirectory(downloads.path());

    CHECK(client.fetchForViewing(linkFor(site, stub, true)));
    CHECK(client.busy());
    CHECK(client.viewing());
    CHECK(settle(&client));
    CHECK_EQ(client.phase(), QStringLiteral("done"));
    if (client.phase() != QStringLiteral("done")) {
        printf("  view failed: %s\n", qPrintable(client.error()));
        return;
    }

    // The bundle is here, in the view's memory — and nowhere else at all.
    CHECK_EQ(client.viewBytes(), quint64(sealed.byteCount()));
    CHECK_EQ(appStore.count(), 0);
    CHECK(client.destination().isEmpty());
    CHECK_EQ(entriesIn(downloads.path()), 0);
    CHECK_EQ(stub.infoRequests(), 1);
    CHECK_EQ(stub.objectRequests(), sealed.count());

    // Served from that memory store, on loopback, under a token URL.
    KelpPreviewServer preview;
    client.setPreviewServer(&preview);
    const QVariantMap opened = client.openPageView();
    CHECK(opened.value(QStringLiteral("ok")).toBool());
    if (!opened.value(QStringLiteral("ok")).toBool()) {
        printf("  open failed: %s\n", qPrintable(opened.value(QStringLiteral("error")).toString()));
        return;
    }
    CHECK_EQ(opened.value(QStringLiteral("entry")).toString(), QStringLiteral("index.html"));
    CHECK_EQ(preview.sessionCount(), 1);

    const QUrl url(opened.value(QStringLiteral("url")).toString());
    CHECK_EQ(url.host(), QStringLiteral("127.0.0.1"));
    CHECK(url.port() > 0);

    Reply reply = get(url.port(), url.path().toLatin1() + "/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.html);

    // Pressing it again is the same session, not a second one and not a refetch.
    const int objectsBefore = stub.objectRequests();
    const QVariantMap again = client.openPageView();
    CHECK(again.value(QStringLiteral("ok")).toBool());
    CHECK_EQ(again.value(QStringLiteral("url")).toString(),
             opened.value(QStringLiteral("url")).toString());
    CHECK_EQ(preview.sessionCount(), 1);
    CHECK_EQ(stub.objectRequests(), objectsBefore);

    // Still nothing on disk, and nothing in the app's store.
    CHECK_EQ(entriesIn(downloads.path()), 0);
    CHECK_EQ(appStore.count(), 0);
}

void testCeilingBeforeFetching()
{
    section("view: the ceiling refuses an oversized page from /info alone");

    MemoryStore sealed;
    Sealed site;
    QString error;
    CHECK(sealSite(&sealed, QStringLiteral("client-ceiling"), true, &site, &error));
    StubPublisher stub(&sealed);
    stub.setClaimedBytes(64ll * 1024 * 1024);
    stub.setClaimedFiles(400);

    MemoryStore appStore;
    KelpClient client(&appStore);

    CHECK(client.fetchForViewing(linkFor(site, stub, true)));
    CHECK(settle(&client));
    CHECK_EQ(client.phase(), QStringLiteral("failed"));
    CHECK(client.error().contains(QStringLiteral("64 MB")));
    CHECK(client.error().contains(QStringLiteral("32 MB")));
    CHECK(client.error().contains(QStringLiteral("folder instead")));

    // The whole point: one /info, no objects, nothing kept.
    CHECK_EQ(stub.infoRequests(), 1);
    CHECK_EQ(stub.objectRequests(), 0);
    CHECK_EQ(client.viewBytes(), quint64(0));
    CHECK_EQ(appStore.count(), 0);
}

void testCeilingWhileStoring()
{
    section("view: the ceiling is enforced against what arrives, not just the claim");

    MemoryStore sealed;
    Sealed site;
    QString error;
    CHECK(sealSite(&sealed, QStringLiteral("client-lie"), true, &site, &error));
    CHECK(sealed.byteCount() > 200);

    StubPublisher stub(&sealed);
    // A claim well under the ceiling, so the /info check passes…
    stub.setClaimedBytes(sealed.byteCount() / 4);

    MemoryStore appStore;
    KelpClient client(&appStore);
    // …and a ceiling between the claim and the truth, so the storing-time check
    // is the one that has to catch it.
    client.setViewCeiling(quint64(sealed.byteCount() / 2));

    CHECK(client.fetchForViewing(linkFor(site, stub, true)));
    CHECK(settle(&client));
    CHECK_EQ(client.phase(), QStringLiteral("failed"));
    CHECK(client.error().contains(QStringLiteral("folder instead")));
    CHECK(stub.objectRequests() > 0);            // it started arriving, and was stopped
    CHECK_EQ(client.viewBytes(), quint64(0));    // and let go of what it had
    CHECK_EQ(appStore.count(), 0);
}

void testNoPage()
{
    section("view: a link that says page but holds none is refused, and keeps nothing");

    MemoryStore sealed;
    Sealed plain;
    QString error;
    CHECK(sealSite(&sealed, QStringLiteral("client-nopage"), false, &plain, &error));
    StubPublisher stub(&sealed);

    MemoryStore appStore;
    KelpClient client(&appStore);
    KelpPreviewServer preview;
    client.setPreviewServer(&preview);

    // The link claims a page; the bundle holds none. Fetching succeeds — it is a
    // perfectly good bundle — and the *view* is what refuses it.
    CHECK(client.fetchForViewing(linkFor(plain, stub, true)));
    CHECK(settle(&client));
    CHECK_EQ(client.phase(), QStringLiteral("done"));

    const QVariantMap opened = client.openPageView();
    CHECK(!opened.value(QStringLiteral("ok")).toBool());
    CHECK_EQ(opened.value(QStringLiteral("error")).toString(),
             QStringLiteral("This link says it is a page, but there is no web page in it."));
    CHECK_EQ(preview.sessionCount(), 0);
    CHECK_EQ(client.viewBytes(), quint64(0));
    CHECK_EQ(appStore.count(), 0);
}

void testSavePathUnchanged()
{
    section("save: the ordinary path still writes files, and keeps no view memory");

    MemoryStore sealed;
    QString bundleId;
    QByteArray rootId;
    QByteArray secret;
    QString error;
    CHECK(sealFile(&sealed, QStringLiteral("client-save"), &bundleId, &rootId, &secret, &error));

    Sealed file;
    file.rootId = rootId;
    file.secret = secret;
    file.bundleId = bundleId;
    file.secretText = Link::base64UrlEncode(secret);
    StubPublisher stub(&sealed);

    QTemporaryDir downloads;
    CHECK(downloads.isValid());

    MemoryStore appStore;
    KelpClient client(&appStore);
    client.setDirectory(downloads.path());

    CHECK(client.fetch(linkFor(file, stub, false)));
    CHECK(!client.viewing());
    CHECK(settle(&client));
    CHECK_EQ(client.phase(), QStringLiteral("done"));
    CHECK_EQ(client.files(), 1);
    CHECK(client.bytes() > 0);
    CHECK(!client.destination().isEmpty());

    // A save keeps verified objects in the app's store and writes the file out.
    CHECK(appStore.count() > 0);
    CHECK_EQ(QDir(downloads.path()).entryList(QDir::Files).size(), 1);
    CHECK_EQ(client.viewBytes(), quint64(0));
}

void testTwoSessions()
{
    section("view: a second view does not free the first session's store");

    MemoryStore sealedA;
    MemoryStore sealedB;
    Sealed a;
    Sealed b;
    QString error;
    CHECK(sealSite(&sealedA, QStringLiteral("client-a"), true, &a, &error));
    CHECK(sealSite(&sealedB, QStringLiteral("client-b"), true, &b, &error));
    CHECK(a.bundleId != b.bundleId);
    StubPublisher stubA(&sealedA);
    StubPublisher stubB(&sealedB);

    MemoryStore appStore;
    KelpClient client(&appStore);
    KelpPreviewServer preview;
    client.setPreviewServer(&preview);

    CHECK(client.fetchForViewing(linkFor(a, stubA, true)));
    CHECK(settle(&client));
    const QVariantMap first = client.openPageView();
    CHECK(first.value(QStringLiteral("ok")).toBool());
    if (!first.value(QStringLiteral("ok")).toBool())
        return;

    // A second webpage link replaces the client's own view store. The first
    // session must keep serving: it holds a reference of its own, which is what
    // makes the idle-timeout lifetime safe (C4a2-plan.md D4/D4a).
    CHECK(client.fetchForViewing(linkFor(b, stubB, true)));
    CHECK(settle(&client));
    const QVariantMap second = client.openPageView();
    CHECK(second.value(QStringLiteral("ok")).toBool());
    if (!second.value(QStringLiteral("ok")).toBool())
        return;

    CHECK(first.value(QStringLiteral("url")).toString()
          != second.value(QStringLiteral("url")).toString());
    CHECK_EQ(preview.sessionCount(), 2);

    const QUrl urlA(first.value(QStringLiteral("url")).toString());
    const QUrl urlB(second.value(QStringLiteral("url")).toString());

    Reply fromA = get(urlA.port(), urlA.path().toLatin1() + "/");
    CHECK_EQ(fromA.status, 200);
    CHECK_EQ(fromA.body, a.html);

    Reply fromB = get(urlB.port(), urlB.path().toLatin1() + "/");
    CHECK_EQ(fromB.status, 200);
    CHECK_EQ(fromB.body, b.html);

    // "Open another link" lets the client's own reference go. The session keeps
    // the memory until its idle timeout — the choice made in the plan, stated as
    // a check so it cannot drift silently.
    client.reset();
    CHECK(!client.viewing());
    CHECK_EQ(client.viewBytes(), quint64(0));

    Reply afterReset = get(urlB.port(), urlB.path().toLatin1() + "/");
    CHECK_EQ(afterReset.status, 200);
    CHECK_EQ(afterReset.body, b.html);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpclient unit test\n");

    testViewFetchesToMemory();
    testCeilingBeforeFetching();
    testCeilingWhileStoring();
    testNoPage();
    testSavePathUnchanged();
    testTwoSessions();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#include "tst_kelpclient.moc"
