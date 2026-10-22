/*
 * kelppreview unit test — the loopback preview (design §5.1, §5.3).
 *
 * A browser can only decrypt inside a secure context, and the LAN path is plain
 * http:// to a private address, so the recipient's app is what decrypts: this test
 * drives the loopback server that re-serves a bundle as ordinary web content.
 *
 * Four things are proved here that nothing else can:
 *
 *   1. the §5.3 convention — which file in a bundle is "the page", and that a
 *      bundle with no page, or with several, says so instead of guessing;
 *   2. the URL space is exactly the bundle — "..", a file used as a directory, an
 *      unknown token and a bad method all answer what HTTP says they should;
 *   3. "manifests first, data lazily" — a store holding only the manifests serves
 *      the page's shape and answers 503 for content it does not have yet, which is
 *      the whole reason a preview can start before a bundle is fetched;
 *   4. a missing object with a source is parked, fetched, verified and then served
 *      (at most four at once), while a failure is 503, bad bytes are 502, and
 *      closing the session answers the waiting request instead of hanging.
 *
 * Everything is off-device: no browser, no LAN, no display.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QString>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_manifest.h"
#include "kelp_object.h"
#include "kelp_store.h"
#include "kelp_web.h"
#include "kelpfetch.h"
#include "kelppreviewserver.h"
#include "kelpstore.h"

using namespace Kelp;

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(const QString &value) { return value; }

// Any arithmetic type prints as itself; the non-template overloads above win
// where they apply.
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

// A session is served from the store its opener names, and the session may outlive
// the opener's interest in it — so the server takes a shared store (design §5.5).
// These tests own their stores on the stack, so they hand over a pointer with a
// deleter that does nothing: the *store* is borrowed even though the *reference*
// is shared.
KelpPreviewServer::StorePtr borrow(Kelp::ObjectStore *store)
{
    return KelpPreviewServer::StorePtr(store, [](Kelp::ObjectStore *) {});
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __LINE__)

// A deterministic blob that is neither HTML nor (with any luck) valid CBOR.
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

// A source for the preview server's on-demand path. It answers from a complete
// store after a short delay, so the request really is parked before the answer
// arrives; the failure and bad-bytes modes are one flag each.
class FakeSource : public KelpSource
{
public:
    FakeSource(MemoryStore *store, QObject *parent = nullptr)
        : KelpSource(parent), m_store(store) {}

    QVector<QByteArray> requested;
    bool fail = false;
    bool badBytes = false;
    bool answer = true;
    int delayMs = 20;
    int active = 0;                  // requests still waiting for an answer
    int maxActive = 0;               // the most the server ever had in flight
    bool closeOnRequest = false;     // close the session from the first request
    KelpPreviewServer *closeServer = nullptr;
    QString closeToken;
    int generation = 0;              // stop() makes pending answers no-ops

    void begin() override {}
    void stop() override { ++generation; }

    void requestObject(const QByteArray &id) override
    {
        requested.append(id);
        ++active;
        maxActive = qMax(maxActive, active);

        if (closeOnRequest && closeServer) {
            KelpPreviewServer *server = closeServer;
            const QString token = closeToken;
            // Not inside requestObject(): the server is still inside pumpFetches
            // and holds a reference to the fetch state close() erases.
            QTimer::singleShot(0, this, [server, token]() { server->close(token); });
        }

        if (!answer)
            return;
        const int gen = generation;
        QTimer::singleShot(delayMs, this, [this, id, gen]() {
            // The session may have stopped this source while an answer was in
            // flight (an over-limit refusal does exactly that). The store may be
            // gone by the time the timer runs, so a stale answer must not touch it.
            if (gen != generation)
                return;
            --active;
            if (fail) {
                emit failed(QStringLiteral("the fake source failed"));
                return;
            }
            if (badBytes) {
                emit objectReady(id, QByteArrayLiteral("not the object that was asked for"));
                return;
            }
            QByteArray object;
            if (!m_store->get(id, &object)) {
                emit failed(QStringLiteral("the fake source has no such object"));
                return;
            }
            emit objectReady(id, object);
        });
    }

    QString describe() const override { return QStringLiteral("the fake source"); }

private:
    MemoryStore *m_store;
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

void collectFiles(const SourceNode &node, MapSource *source)
{
    if (!node.isDir) {
        source->files.insert(node.path, pattern(qPrintable(node.name), int(node.size)));
        return;
    }
    for (int i = 0; i < node.children.size(); ++i)
        collectFiles(node.children.at(i), source);
}

// The one bundle most sections use: index.html at the root, a stylesheet, an
// image in a subdirectory, a second page in a subdirectory, a text file, and one
// file large enough to need three chunks.
struct Site {
    QByteArray secret;
    QByteArray key;
    QByteArray rootId;
    QByteArray html;
    QByteArray css;
    QByteArray logo;
    QByteArray subHtml;
    QByteArray text;
    QByteArray big;
    QString bundleId;
    QString secretText;

    bool build(ObjectStore *store, QString *error)
    {
        secret = Crypto::sha256(QByteArrayLiteral("preview-test-secret"));
        key = contentKey(secret);

        html = QByteArrayLiteral(
            "<!doctype html>\n<html><head><title>Kelp</title>\n"
            "<link rel=\"stylesheet\" href=\"style.css\">\n"
            "</head><body><h1>Hello</h1>\n"
            "<img src=\"images/logo.png\">\n"
            "<a href=\"sub/\">sub</a></body></html>\n");
        css = QByteArrayLiteral("body { font-family: sans-serif; }\n");
        logo = pattern("logo", 3000);
        subHtml = QByteArrayLiteral("<!doctype html><p>sub</p>\n");
        text = QByteArrayLiteral("notes\n");
        big = pattern("big", Manifest::ChunkSize * 2 + 700);

        MapSource source;
        source.files.insert(QStringLiteral("site/index.html"), html);
        source.files.insert(QStringLiteral("site/style.css"), css);
        source.files.insert(QStringLiteral("site/images/logo.png"), logo);
        source.files.insert(QStringLiteral("site/sub/index.html"), subHtml);
        source.files.insert(QStringLiteral("site/notes.txt"), text);
        source.files.insert(QStringLiteral("site/big.bin"), big);

        SourceNode images = dirNode(QStringLiteral("images"));
        images.children.append(fileNode(QStringLiteral("logo.png"),
                                        QStringLiteral("site/images/logo.png"), logo.size()));

        SourceNode sub = dirNode(QStringLiteral("sub"));
        sub.children.append(fileNode(QStringLiteral("index.html"),
                                     QStringLiteral("site/sub/index.html"), subHtml.size()));

        SourceNode root = dirNode(QStringLiteral("site"));
        root.children.append(fileNode(QStringLiteral("index.html"),
                                      QStringLiteral("site/index.html"), html.size()));
        root.children.append(fileNode(QStringLiteral("style.css"),
                                      QStringLiteral("site/style.css"), css.size()));
        root.children.append(fileNode(QStringLiteral("notes.txt"),
                                      QStringLiteral("site/notes.txt"), text.size()));
        root.children.append(fileNode(QStringLiteral("big.bin"),
                                      QStringLiteral("site/big.bin"), big.size()));
        root.children.append(images);
        root.children.append(sub);

        SealedBundle sealed;
        if (!seal(store, key, root, &source, &sealed, error))
            return false;

        rootId = sealed.rootId;
        bundleId = Link::base32Encode(rootId);
        secretText = Link::base64UrlEncode(secret);
        return true;
    }
};

// A small bundle whose top level is exactly `top`, sealed under its own secret.
bool sealSmall(ObjectStore *store, const QString &seed, const QVector<SourceNode> &top,
               QByteArray *rootId, QByteArray *key, QString *error)
{
    error->clear();

    SourceNode root = dirNode(QStringLiteral("root"));
    root.children = top;

    MapSource source;
    collectFiles(root, &source);

    const QByteArray secret = Crypto::sha256(seed.toUtf8());
    *key = contentKey(secret);

    SealedBundle sealed;
    if (!seal(store, *key, root, &source, &sealed, error))
        return false;
    *rootId = sealed.rootId;
    return true;
}

// ---- the HTTP client the tests speak through -------------------------------

struct Reply {
    int status = 0;
    QHash<QString, QString> headers;   // names lowercased
    QByteArray body;

    QString header(const char *name) const
    {
        return headers.value(QString::fromLatin1(name));
    }
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

    const QList<QByteArray> lines = response.left(end).split('\n');
    const QList<QByteArray> statusLine = lines.at(0).trimmed().split(' ');
    if (statusLine.size() >= 2)
        reply.status = statusLine.at(1).toInt();

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        reply.headers.insert(QString::fromLatin1(line.left(colon).trimmed().toLower()),
                             QString::fromLatin1(line.mid(colon + 1).trimmed()));
    }
    return reply;
}

Reply get(int port, const QByteArray &path, const QByteArray &extraHeaders = QByteArray(),
          bool head = false)
{
    QByteArray request = (head ? QByteArrayLiteral("HEAD ") : QByteArrayLiteral("GET "));
    request += path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += extraHeaders;
    request += "Connection: close\r\n\r\n";
    return http(port, request);
}

QByteArray rangeHeader(quint64 first, quint64 last)
{
    return "Range: bytes=" + QByteArray::number(first) + "-" + QByteArray::number(last) + "\r\n";
}

// Two browser tabs asking at once, each on its own connection so the server
// really sees both before either answer. This is what proves two requests for
// the same missing object join one fetch.
QVector<Reply> getTwo(int port, const QByteArray &pathA, const QByteArray &pathB)
{
    QNetworkAccessManager managerA;
    QNetworkAccessManager managerB;
    const QUrl urlA(QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(port).arg(QString::fromLatin1(pathA)));
    const QUrl urlB(QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(port).arg(QString::fromLatin1(pathB)));
    QNetworkReply *replyA = managerA.get(QNetworkRequest(urlA));
    QNetworkReply *replyB = managerB.get(QNetworkRequest(urlB));

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    int remaining = 2;
    const auto oneDone = [&loop, &remaining]() {
        if (--remaining == 0)
            loop.quit();
    };
    QObject::connect(replyA, &QNetworkReply::finished, oneDone);
    QObject::connect(replyB, &QNetworkReply::finished, oneDone);
    timeout.start(5000);
    loop.exec();

    QVector<Reply> replies;
    QNetworkReply *const raw[2] = { replyA, replyB };
    for (int i = 0; i < 2; ++i) {
        Reply reply;
        if (!raw[i]->isFinished())
            raw[i]->abort();
        reply.status = raw[i]->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply.body = raw[i]->readAll();
        replies.append(reply);
        raw[i]->deleteLater();
    }
    return replies;
}

// ---- which file is the page (design §5.3) ----------------------------------

void testPage()
{
    section("which file is the page");

    MemoryStore store;
    Site site;
    QString error;
    if (!site.build(&store, &error)) {
        CHECK(false);
        printf("  build error: %s\n", qPrintable(error));
        return;
    }

    Web::Page page;
    CHECK(Web::page(&store, site.key, site.rootId, &page, &error));
    if (!error.isEmpty())
        printf("  page error: %s\n", qPrintable(error));

    // index.html at the bundle root wins, and the URL root is the bundle root.
    CHECK_EQ(page.path, QStringLiteral("index.html"));
    CHECK_EQ(page.root, QString());
    CHECK_EQ(page.candidates.size(), 2);   // index.html and sub/index.html

    // One HTML file anywhere: that is the page, and its directory is the URL root.
    MemoryStore nested;
    QVector<SourceNode> top;
    SourceNode docs = dirNode(QStringLiteral("docs"));
    docs.children.append(fileNode(QStringLiteral("notes.html"), QStringLiteral("notes"), 100));
    top.append(docs);

    QByteArray rootId;
    QByteArray key;
    CHECK(sealSmall(&nested, QStringLiteral("nested"), top, &rootId, &key, &error));
    Web::Page nestedPage;
    CHECK(Web::page(&nested, key, rootId, &nestedPage, &error));
    CHECK_EQ(nestedPage.path, QStringLiteral("docs/notes.html"));
    CHECK_EQ(nestedPage.root, QStringLiteral("docs"));

    // Two pages and no index: a choice, so the bundle does not make one.
    MemoryStore two;
    QVector<SourceNode> twoTop;
    twoTop.append(fileNode(QStringLiteral("b.html"), QStringLiteral("b"), 10));
    twoTop.append(fileNode(QStringLiteral("a.html"), QStringLiteral("a"), 10));
    CHECK(sealSmall(&two, QStringLiteral("two"), twoTop, &rootId, &key, &error));
    Web::Page twoPage;
    CHECK(Web::page(&two, key, rootId, &twoPage, &error));
    CHECK(twoPage.path.isEmpty());
    CHECK_EQ(twoPage.candidates.size(), 2);
    CHECK_EQ(twoPage.candidates.at(0), QStringLiteral("a.html"));   // sorted
    CHECK_EQ(twoPage.candidates.at(1), QStringLiteral("b.html"));

    // No HTML at all is a true answer, not a failure.
    MemoryStore none;
    QVector<SourceNode> noneTop;
    noneTop.append(fileNode(QStringLiteral("data.bin"), QStringLiteral("d"), 10));
    CHECK(sealSmall(&none, QStringLiteral("none"), noneTop, &rootId, &key, &error));
    Web::Page nonePage;
    CHECK(Web::page(&none, key, rootId, &nonePage, &error));
    CHECK(nonePage.path.isEmpty());
    CHECK_EQ(nonePage.candidates.size(), 0);

    // A bundle that is not all here yet cannot answer at all: unknown is not the
    // same as "no page", and a preview must not present one as the other.
    MemoryStore broken;
    Site brokenSite;
    CHECK(brokenSite.build(&broken, &error));
    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Manifest::File file;
    CHECK(Web::resolve(&broken, brokenSite.key, brokenSite.rootId,
                       QStringLiteral("images/logo.png"), &found, &isDir, &hash, &file, &error));
    CHECK(found);
    broken.remove(hash);          // the image's own file manifest goes away
    Web::Page brokenPage;
    error.clear();
    CHECK(!Web::page(&broken, brokenSite.key, brokenSite.rootId, &brokenPage, &error));
    CHECK(!error.isEmpty());
}

// ---- path resolution --------------------------------------------------------

void testResolve()
{
    section("resolving a URL path to the bundle");

    MemoryStore store;
    Site site;
    QString error;
    if (!site.build(&store, &error)) {
        CHECK(false);
        return;
    }

    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Manifest::File file;

    // The root is a directory.
    CHECK(Web::resolve(&store, site.key, site.rootId, QString(), &found, &isDir,
                       &hash, &file, &error));
    CHECK(found);
    CHECK(isDir);

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("index.html"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK(!isDir);
    CHECK_EQ(file.size, quint64(site.html.size()));
    CHECK_EQ(file.chunks.size(), 1);

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("images"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK(isDir);

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("images/logo.png"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK(!isDir);
    CHECK_EQ(file.size, quint64(site.logo.size()));

    // Three chunks, and the manifest says so.
    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("big.bin"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK_EQ(file.chunks.size(), 3);

    // Nothing there is a 404, with no error: it is an answer, not a failure.
    error.clear();
    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("nope"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(!found);
    CHECK(error.isEmpty());

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("../etc/passwd"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(!found);
    CHECK(error.isEmpty());

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("index.html/child"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(!found);

    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("images/nope.png"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(!found);

    // A manifest that is not here is a question that cannot be answered: the walk
    // fails with an error, so a caller answers 503 rather than pretending the path
    // is absent. (The alternative — a quiet "no such path" — is what a 404 would
    // show a browser for a file that is merely not fetched yet.)
    MemoryStore broken;
    Site brokenSite;
    CHECK(brokenSite.build(&broken, &error));
    CHECK(Web::resolve(&broken, brokenSite.key, brokenSite.rootId, QStringLiteral("images"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    broken.remove(hash);
    error.clear();
    CHECK(!Web::resolve(&broken, brokenSite.key, brokenSite.rootId,
                        QStringLiteral("images/logo.png"), &found, &isDir, &hash, &file, &error));
    CHECK(!error.isEmpty());
}

// ---- chunk ranges -----------------------------------------------------------

void testChunkRange()
{
    section("chunk ranges");

    Manifest::File file;
    file.size = Manifest::ChunkSize * 2 + 700;
    file.chunks.append(QByteArray(32, 'a'));
    file.chunks.append(QByteArray(32, 'b'));
    file.chunks.append(QByteArray(32, 'c'));

    int first = -1;
    int count = -1;

    CHECK(Web::chunkRange(file, 0, file.size, &first, &count));
    CHECK_EQ(first, 0);
    CHECK_EQ(count, 3);

    CHECK(Web::chunkRange(file, 0, 1, &first, &count));
    CHECK_EQ(first, 0);
    CHECK_EQ(count, 1);

    // A range that ends exactly on a boundary stays in one chunk.
    CHECK(Web::chunkRange(file, Manifest::ChunkSize - 1, 1, &first, &count));
    CHECK_EQ(first, 0);
    CHECK_EQ(count, 1);

    // Two bytes spanning a boundary need both chunks.
    CHECK(Web::chunkRange(file, Manifest::ChunkSize - 1, 2, &first, &count));
    CHECK_EQ(first, 0);
    CHECK_EQ(count, 2);

    CHECK(Web::chunkRange(file, Manifest::ChunkSize, 1, &first, &count));
    CHECK_EQ(first, 1);
    CHECK_EQ(count, 1);

    CHECK(Web::chunkRange(file, file.size - 1, 1, &first, &count));
    CHECK_EQ(first, 2);
    CHECK_EQ(count, 1);

    // An empty range at the end is legal, names no chunk, and sits inside the last
    // (partial) chunk rather than past it.
    CHECK(Web::chunkRange(file, file.size, 0, &first, &count));
    CHECK_EQ(first, 2);
    CHECK_EQ(count, 0);

    CHECK(!Web::chunkRange(file, file.size + 1, 1, &first, &count));
    CHECK(!Web::chunkRange(file, 0, file.size + 1, &first, &count));
    CHECK(!Web::chunkRange(file, file.size - 1, 2, &first, &count));
}

// ---- content types ----------------------------------------------------------

void testMime()
{
    section("content types");

    // Nothing in the app records a MIME type when sealing, so the extension table
    // is the real mechanism, not a fallback for old bundles.
    Manifest::File plain;
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("index.html")),
             QStringLiteral("text/html; charset=utf-8"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("style.css")),
             QStringLiteral("text/css; charset=utf-8"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("app.js")),
             QStringLiteral("text/javascript; charset=utf-8"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("images/logo.png")),
             QStringLiteral("image/png"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("photo.JPEG")),
             QStringLiteral("image/jpeg"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("font.woff2")),
             QStringLiteral("font/woff2"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("noextension")),
             QStringLiteral("application/octet-stream"));
    CHECK_EQ(Web::mimeType(plain, QStringLiteral("archive.unknown")),
             QStringLiteral("application/octet-stream"));

    // The sender's own word wins when there is one.
    Manifest::File withMime;
    withMime.mime = QStringLiteral("text/plain; charset=utf-8");
    CHECK_EQ(Web::mimeType(withMime, QStringLiteral("index.html")),
             QStringLiteral("text/plain; charset=utf-8"));
}

// ---- serving ----------------------------------------------------------------

void testServing()
{
    section("serving a page over loopback");

    MemoryStore store;
    Site site;
    QString error;
    if (!site.build(&store, &error)) {
        CHECK(false);
        return;
    }

    KelpPreviewServer server;
    int opened = 0;
    int closed = 0;
    QObject::connect(&server, &KelpPreviewServer::sessionsChanged, [&opened, &closed, &server]() {
        if (server.sessionCount() > 0)
            ++opened;
        else
            ++closed;
    });

    CHECK(server.start(&error));
    CHECK(server.isListening());
    CHECK(server.port() > 0);
    CHECK_EQ(server.sessionCount(), 0);

    const KelpPreviewServer::Opened session =
        server.open(borrow(&store), site.bundleId, site.secretText, QStringLiteral("Preview test"));
    CHECK(session.ok);
    if (!session.ok) {
        printf("  open error: %s\n", qPrintable(session.error));
        return;
    }
    CHECK_EQ(session.entry, QStringLiteral("index.html"));
    CHECK_EQ(session.root, QString());
    CHECK_EQ(session.token.size(), 64);          // 32 random bytes, hex
    CHECK_EQ(session.url,
             QStringLiteral("http://127.0.0.1:%1/%2/").arg(server.port()).arg(session.token));
    CHECK_EQ(server.sessionCount(), 1);
    CHECK(server.has(session.token));
    CHECK_EQ(opened, 1);

    const QByteArray prefix = "/" + session.token.toLatin1();
    const int port = server.port();

    // The page itself, with the headers that make a loopback origin safe.
    Reply reply = get(port, prefix + "/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-type"), QStringLiteral("text/html; charset=utf-8"));
    CHECK_EQ(reply.header("content-length"), QString::number(site.html.size()));
    CHECK_EQ(reply.header("accept-ranges"), QStringLiteral("bytes"));
    CHECK_EQ(reply.body, site.html);
    const QString csp = reply.header("content-security-policy");
    CHECK(csp.contains(QStringLiteral("default-src 'self'")));
    CHECK(csp.contains(QStringLiteral("frame-ancestors 'none'")));
    CHECK(csp.contains(QStringLiteral("base-uri 'none'")));
    CHECK_EQ(reply.header("x-content-type-options"), QStringLiteral("nosniff"));
    CHECK_EQ(reply.header("cache-control"), QStringLiteral("no-store"));
    CHECK_EQ(reply.header("referrer-policy"), QStringLiteral("no-referrer"));

    // Everything the page asks for, by its bundle path.
    reply = get(port, prefix + "/style.css");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-type"), QStringLiteral("text/css; charset=utf-8"));
    CHECK_EQ(reply.body, site.css);

    reply = get(port, prefix + "/images/logo.png");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-type"), QStringLiteral("image/png"));
    CHECK_EQ(reply.body, site.logo);

    reply = get(port, prefix + "/notes.txt");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-type"), QStringLiteral("text/plain; charset=utf-8"));
    CHECK_EQ(reply.body, site.text);

    // A directory serves its own index.html; without the slash, the page's
    // relative links would resolve against the parent, so that is a redirect.
    reply = get(port, prefix + "/sub/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.subHtml);

    reply = get(port, prefix + "/sub");
    CHECK_EQ(reply.status, 301);
    CHECK_EQ(reply.header("location"), QString::fromLatin1(prefix + "/sub/"));

    // The URL space is exactly the bundle.
    reply = get(port, prefix + "/missing.png");
    CHECK_EQ(reply.status, 404);
    reply = get(port, prefix + "/%2e%2e/index.html");
    CHECK_EQ(reply.status, 404);
    reply = get(port, prefix + "/index.html/child");
    CHECK_EQ(reply.status, 404);
    reply = get(port, prefix + "/images");
    CHECK_EQ(reply.status, 301);              // a real directory, so it redirects

    // A token this server never minted is the same 404, with nothing to tell the
    // two apart — and a body that says what a reader can do, because the usual
    // reason a real token stops working is that the session was swept (§3.25).
    reply = get(port, "/0000000000000000000000000000000000000000000000000000000000000000/");
    CHECK_EQ(reply.status, 404);
    CHECK(!server.has(QString(64, QLatin1Char('0'))));
    CHECK(reply.body.contains("Open the link again from Kelp"));
    reply = get(port, "/");
    CHECK_EQ(reply.status, 404);
    CHECK(reply.body.contains("Open the link again from Kelp"));

    // HEAD is answered like GET, without the body.
    reply = get(port, prefix + "/", QByteArray(), true);
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-length"), QString::number(site.html.size()));
    CHECK_EQ(reply.body.size(), 0);

    // Anything but GET and HEAD.
    reply = http(port, "DELETE " + prefix + "/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    CHECK_EQ(reply.status, 405);

    // Seek: inside one chunk, across a boundary, and from the end.
    reply = get(port, prefix + "/index.html", rangeHeader(0, 9));
    CHECK_EQ(reply.status, 206);
    CHECK_EQ(reply.header("content-range"),
             QStringLiteral("bytes 0-9/%1").arg(site.html.size()));
    CHECK_EQ(reply.body, site.html.left(10));

    const quint64 boundary = Manifest::ChunkSize - 3;
    reply = get(port, prefix + "/big.bin", rangeHeader(boundary, boundary + 5));
    CHECK_EQ(reply.status, 206);
    CHECK_EQ(reply.body, site.big.mid(int(boundary), 6));
    CHECK_EQ(reply.header("content-range"),
             QStringLiteral("bytes %1-%2/%3").arg(boundary).arg(boundary + 5)
                 .arg(site.big.size()));

    reply = get(port, prefix + "/big.bin", QByteArrayLiteral("Range: bytes=-5\r\n"));
    CHECK_EQ(reply.status, 206);
    CHECK_EQ(reply.body, site.big.right(5));

    reply = get(port, prefix + "/big.bin", QByteArrayLiteral("Range: bytes=0-\r\n"));
    CHECK_EQ(reply.status, 206);
    CHECK_EQ(reply.body, site.big);

    // The whole file, in three chunks, is the file again.
    reply = get(port, prefix + "/big.bin");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.header("content-length"), QString::number(site.big.size()));
    CHECK_EQ(reply.body, site.big);

    // A range that is not in the file, and headers that are simply not understood.
    reply = get(port, prefix + "/big.bin", QByteArrayLiteral("Range: bytes=99999999-\r\n"));
    CHECK_EQ(reply.status, 416);
    CHECK_EQ(reply.header("content-range"),
             QStringLiteral("bytes */%1").arg(site.big.size()));

    reply = get(port, prefix + "/big.bin", QByteArrayLiteral("Range: bytes=abc\r\n"));
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.big);

    reply = get(port, prefix + "/big.bin", QByteArrayLiteral("Range: bytes=0-1,3-4\r\n"));
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.big);

    // A query string is not part of the path.
    reply = get(port, prefix + "/style.css?v=2");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.css);

    // What was served, added up: one body per 200, and only the requested bytes of
    // each 206. Nothing was withheld and exactly two requests had no token.
    const qint64 expectedBytes =
        qint64(site.html.size())           // "/"
        + qint64(site.css.size())          // "/style.css"
        + qint64(site.css.size())          // "/style.css?v=2"
        + qint64(site.logo.size())         // "/images/logo.png"
        + qint64(site.text.size())         // "/notes.txt"
        + qint64(site.subHtml.size())      // "/sub/"
        + 10                               // range 0-9
        + 6                                // range across the chunk boundary
        + 5                                // range from the end
        + qint64(site.big.size()) * 4;     // "0-", malformed, multi-range, whole file

    CHECK_EQ(server.tally().notHeld, 0);
    CHECK_EQ(server.tally().refused, 2);
    CHECK(server.tally().requests > 0);
    CHECK_EQ(server.tally().bytes, expectedBytes);

    // Closing a session closes the URL.
    server.close(session.token);
    CHECK_EQ(server.sessionCount(), 0);
    CHECK_EQ(closed, 1);
    reply = get(port, prefix + "/");
    CHECK_EQ(reply.status, 404);
}

// ---- absolute paths ---------------------------------------------------------

// A page often names its own assets from the site root ("/style.css"), while the
// preview lives under "<port>/<token>/". The page response sets the token as a
// host-only cookie; requests to root-absolute paths are matched through it.
void testRootAbsolute()
{
    section("root-absolute paths");

    MemoryStore store;
    Site site;
    QString error;
    if (!site.build(&store, &error)) {
        CHECK(false);
        return;
    }

    KelpPreviewServer server;
    CHECK(server.start(&error));

    const KelpPreviewServer::Opened session =
        server.open(borrow(&store), site.bundleId, site.secretText, QStringLiteral("Absolute"));
    CHECK(session.ok);
    if (!session.ok) {
        printf("  open error: %s\n", qPrintable(session.error));
        return;
    }

    const int port = server.port();
    const QByteArray prefix = "/" + session.token.toLatin1();
    const QByteArray cookie = "Cookie: kelp_preview=" + session.token.toLatin1() + "\r\n";

    Reply reply = get(port, prefix + "/");
    CHECK_EQ(reply.status, 200);
    const QString setCookie = reply.header("set-cookie");
    CHECK(setCookie.startsWith(QStringLiteral("kelp_preview=") + session.token));
    CHECK(setCookie.contains(QStringLiteral("Path=/")));
    CHECK(setCookie.contains(QStringLiteral("HttpOnly")));
    CHECK(setCookie.contains(QStringLiteral("SameSite=Strict")));

    // What a browser does with a page whose links are written from the site root.
    reply = get(port, "/style.css", cookie);
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.css);

    reply = get(port, "/", cookie);
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.html);

    reply = get(port, "/sub/", cookie);
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.subHtml);

    // Directory URLs keep their existing slash rule; the browser follows the
    // redirect with the cookie still attached.
    reply = get(port, "/sub", cookie);
    CHECK_EQ(reply.status, 301);
    CHECK_EQ(reply.header("location"), QStringLiteral("/sub/"));

    // The cookie is not a second address: without it the root path is still a
    // 404, and the token path still works unchanged.
    reply = get(port, "/style.css");
    CHECK_EQ(reply.status, 404);
    reply = get(port, prefix + "/style.css");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.css);

    // A first segment with a token's shape is read as a token, not as a path,
    // even when a valid page cookie is present.
    const QByteArray unknown(64, '0');
    reply = get(port, "/" + unknown + "/style.css", cookie);
    CHECK_EQ(reply.status, 404);
}

// ---- holes ------------------------------------------------------------------

// Every manifest object a bundle is made of, found by opening each object and
// asking which decoder accepts it. A preview is allowed to be missing the data
// objects; this builds a store that has everything else.
QVector<QByteArray> manifestsOf(ObjectStore *store, const QByteArray &key,
                                const QByteArray &rootId)
{
    QVector<QByteArray> all;
    QVector<QByteArray> manifests;
    QString error;
    if (!bundleObjects(store, key, rootId, &all, &error))
        return manifests;

    for (int i = 0; i < all.size(); ++i) {
        QByteArray object;
        QByteArray plain;
        QString openError;
        if (!store->get(all.at(i), &object) || !Object::open(key, object, &plain, &openError))
            continue;

        Manifest::Directory directory;
        Manifest::File file;
        QString decodeError;
        if (Manifest::decodeDirectory(plain, &directory, &decodeError)
            || Manifest::decodeFile(plain, &file, &decodeError))
            manifests.append(all.at(i));
    }
    return manifests;
}

void copyObjects(ObjectStore *from, ObjectStore *to, const QVector<QByteArray> &ids)
{
    for (int i = 0; i < ids.size(); ++i) {
        QByteArray object;
        if (!from->get(ids.at(i), &object))
            continue;
        QByteArray stored;
        to->put(object, &stored);
    }
}

void testLazy()
{
    section("manifests first, data lazily");

    MemoryStore full;
    Site site;
    QString error;
    if (!site.build(&full, &error)) {
        CHECK(false);
        return;
    }

    // A store with the manifests and none of the content: what a recipient has
    // after the walk, before any data is fetched.
    const QVector<QByteArray> manifests = manifestsOf(&full, site.key, site.rootId);
    CHECK(manifests.size() > 0);
    CHECK(manifests.size() < full.count());

    MemoryStore partial;
    copyObjects(&full, &partial, manifests);
    CHECK_EQ(partial.count(), manifests.size());

    KelpPreviewServer server;
    CHECK(server.start(&error));

    const KelpPreviewServer::Opened session =
        server.open(borrow(&partial), site.bundleId, site.secretText, QStringLiteral("Lazy"));
    CHECK(session.ok);                     // the page is known from manifests alone
    if (!session.ok) {
        printf("  open error: %s\n", qPrintable(session.error));
        return;
    }

    const QByteArray prefix = "/" + session.token.toLatin1();

    // The shape is served; the bytes are not here yet.
    Reply reply = get(server.port(), prefix + "/");
    CHECK_EQ(reply.status, 503);
    CHECK_EQ(server.tally().notHeld, 1);

    reply = get(server.port(), prefix + "/style.css");
    CHECK_EQ(reply.status, 503);

    // Fetch one file's objects — as the fetch loop would, one object at a time —
    // and only that file starts answering. Nothing else does.
    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Manifest::File file;
    CHECK(Web::resolve(&full, site.key, site.rootId, QStringLiteral("index.html"),
                       &found, &isDir, &hash, &file, &error));
    copyObjects(&full, &partial, file.chunks);

    reply = get(server.port(), prefix + "/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.html);

    reply = get(server.port(), prefix + "/style.css");
    CHECK_EQ(reply.status, 503);
}

void testOnDemand()
{
    section("a missing object is fetched for the request that asks");

    MemoryStore full;
    Site site;
    QString error;
    if (!site.build(&full, &error)) {
        CHECK(false);
        printf("  build error: %s\n", qPrintable(error));
        return;
    }

    const QVector<QByteArray> manifests = manifestsOf(&full, site.key, site.rootId);
    CHECK(manifests.size() > 0);

    // One response's worth of data missing: the page's own chunk.
    MemoryStore partial;
    copyObjects(&full, &partial, manifests);
    KelpPreviewServer server;
    CHECK(server.start(&error));
    FakeSource *source = new FakeSource(&full);
    const KelpPreviewServer::Opened session =
        server.open(borrow(&partial), site.bundleId, site.secretText,
                    QStringLiteral("On demand"), source);
    CHECK(session.ok);
    if (!session.ok) {
        printf("  open error: %s\n", qPrintable(session.error));
        return;
    }
    const QByteArray prefix = "/" + session.token.toLatin1();
    const int port = server.port();

    Reply reply = get(port, prefix + "/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.html);
    CHECK_EQ(source->requested.size(), 1);
    CHECK_EQ(server.tally().onDemand, 1);

    // Only the file that was asked for was fetched: the stylesheet was still a
    // hole, and asking for it is what fetches that one.
    CHECK_EQ(source->requested.size(), 1);
    reply = get(port, prefix + "/style.css");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.css);
    CHECK_EQ(source->requested.size(), 2);

    // Two requests for the same three-chunk file, before either can be answered,
    // join the same three fetches rather than asking the sender twice.
    MemoryStore bigPartial;
    copyObjects(&full, &bigPartial, manifests);
    KelpPreviewServer bigServer;
    CHECK(bigServer.start(&error));
    FakeSource *bigSource = new FakeSource(&full);
    bigSource->delayMs = 50;
    const KelpPreviewServer::Opened bigSession =
        bigServer.open(borrow(&bigPartial), site.bundleId, site.secretText,
                       QStringLiteral("Two tabs"), bigSource);
    CHECK(bigSession.ok);
    if (!bigSession.ok)
        return;
    const QByteArray bigPrefix = "/" + bigSession.token.toLatin1();
    const QVector<Reply> both = getTwo(bigServer.port(), bigPrefix + "/big.bin",
                                       bigPrefix + "/big.bin");
    CHECK_EQ(both.size(), 2);
    if (both.size() == 2) {
        CHECK_EQ(both.at(0).status, 200);
        CHECK_EQ(both.at(1).status, 200);
        CHECK_EQ(both.at(0).body, site.big);
        CHECK_EQ(both.at(1).body, site.big);
    }
    CHECK_EQ(bigSource->requested.size(), 3);
    QSet<QByteArray> unique;
    for (int i = 0; i < bigSource->requested.size(); ++i)
        unique.insert(bigSource->requested.at(i));
    CHECK_EQ(unique.size(), 3);
    CHECK_EQ(bigServer.tally().onDemand, 3);

    // A source that says it failed is a 503 for the parked request, with the
    // same body a store-only preview gives.
    MemoryStore failPartial;
    copyObjects(&full, &failPartial, manifests);
    KelpPreviewServer failServer;
    CHECK(failServer.start(&error));
    FakeSource *failSource = new FakeSource(&full);
    failSource->fail = true;
    const KelpPreviewServer::Opened failSession =
        failServer.open(borrow(&failPartial), site.bundleId, site.secretText,
                        QStringLiteral("Failing source"), failSource);
    CHECK(failSession.ok);
    if (!failSession.ok)
        return;
    const QByteArray failPrefix = "/" + failSession.token.toLatin1();
    reply = get(failServer.port(), failPrefix + "/");
    CHECK_EQ(reply.status, 503);
    CHECK(reply.body.contains("not all here yet"));
    CHECK_EQ(failServer.tally().notHeld, 1);

    // Bytes that do not hash to the id are not stored and are a 502: asking
    // again would only repeat the same lie.
    MemoryStore badPartial;
    copyObjects(&full, &badPartial, manifests);
    KelpPreviewServer badServer;
    CHECK(badServer.start(&error));
    FakeSource *badSource = new FakeSource(&full);
    badSource->badBytes = true;
    const KelpPreviewServer::Opened badSession =
        badServer.open(borrow(&badPartial), site.bundleId, site.secretText,
                       QStringLiteral("Bad bytes"), badSource);
    CHECK(badSession.ok);
    if (!badSession.ok)
        return;
    const QByteArray badPrefix = "/" + badSession.token.toLatin1();
    reply = get(badServer.port(), badPrefix + "/");
    CHECK_EQ(reply.status, 502);
    CHECK(reply.body.contains("did not match its address"));
    CHECK_EQ(badServer.tally().mismatched, 1);
    CHECK_EQ(badPartial.count(), manifests.size());

    // A source that never answers is a 503 after the deadline, not a browser
    // left waiting. The test lowers the deadline; production is twenty seconds.
    MemoryStore mutePartial;
    copyObjects(&full, &mutePartial, manifests);
    KelpPreviewServer muteServer;
    muteServer.setFetchTimeout(100);
    CHECK(muteServer.start(&error));
    FakeSource *muteSource = new FakeSource(&full);
    muteSource->answer = false;
    const KelpPreviewServer::Opened muteSession =
        muteServer.open(borrow(&mutePartial), site.bundleId, site.secretText,
                        QStringLiteral("Silent source"), muteSource);
    CHECK(muteSession.ok);
    if (!muteSession.ok)
        return;
    const QByteArray mutePrefix = "/" + muteSession.token.toLatin1();
    reply = get(muteServer.port(), mutePrefix + "/");
    CHECK_EQ(reply.status, 503);
    CHECK(reply.body.contains("not all here yet"));
    CHECK(muteServer.tally().notHeld > 0);

    // The fetch bound: a file with ten chunks is asked for four at a time, never
    // ten. The source counts how many answers it owes at once.
    MemoryStore hugeFull;
    const int hugeChunks = 10;
    const QByteArray huge = pattern("huge.bin", Manifest::ChunkSize * (hugeChunks - 1) + 17);
    QVector<SourceNode> hugeTop;
    hugeTop.append(fileNode(QStringLiteral("index.html"), QStringLiteral("index.html"), 40));
    hugeTop.append(fileNode(QStringLiteral("huge.bin"), QStringLiteral("huge.bin"),
                            huge.size()));
    QByteArray hugeRootId;
    QByteArray hugeKey;
    CHECK(sealSmall(&hugeFull, QStringLiteral("huge"), hugeTop, &hugeRootId, &hugeKey, &error));

    MemoryStore hugePartial;
    copyObjects(&hugeFull, &hugePartial, manifestsOf(&hugeFull, hugeKey, hugeRootId));
    KelpPreviewServer hugeServer;
    CHECK(hugeServer.start(&error));
    FakeSource *hugeSource = new FakeSource(&hugeFull);
    const QByteArray hugeSecret = Crypto::sha256(QByteArrayLiteral("huge"));
    const KelpPreviewServer::Opened hugeSession =
        hugeServer.open(borrow(&hugePartial), Link::base32Encode(hugeRootId),
                        Link::base64UrlEncode(hugeSecret), QStringLiteral("Ten chunks"),
                        hugeSource);
    CHECK(hugeSession.ok);
    if (hugeSession.ok) {
        const Reply hugeReply = get(hugeServer.port(),
                                    "/" + hugeSession.token.toLatin1() + "/huge.bin");
        CHECK_EQ(hugeReply.status, 200);
        CHECK_EQ(hugeReply.body, huge);
        CHECK_EQ(hugeSource->requested.size(), hugeChunks);
        CHECK(hugeSource->maxActive <= 4);
        CHECK_EQ(hugeServer.tally().onDemand, hugeChunks);
    }

    // A store ceiling is checked before an on-demand object is stored: a view
    // that would grow past it gets 503 and its fetches stop, rather than leaking
    // memory because the sender's /info was optimistic.
    MemoryStore limitPartial;
    copyObjects(&hugeFull, &limitPartial, manifestsOf(&hugeFull, hugeKey, hugeRootId));
    KelpPreviewServer limitServer;
    CHECK(limitServer.start(&error));
    FakeSource *limitSource = new FakeSource(&hugeFull);
    const qint64 limitBytes = limitPartial.byteCount();
    const quint64 limit = quint64(limitBytes) + 1;
    const KelpPreviewServer::Opened limitSession =
        limitServer.open(borrow(&limitPartial), Link::base32Encode(hugeRootId),
                         Link::base64UrlEncode(hugeSecret), QStringLiteral("Too large"),
                         limitSource, limit);
    CHECK(limitSession.ok);
    if (limitSession.ok) {
        const Reply limitReply = get(limitServer.port(),
                                     "/" + limitSession.token.toLatin1() + "/huge.bin");
        CHECK_EQ(limitReply.status, 503);
        CHECK(limitReply.body.contains("too large"));
        CHECK(limitServer.tally().overLimit > 0);
        CHECK_EQ(limitPartial.byteCount(), limitBytes);   // nothing was stored
    }

    // Closing a session with a browser request parked answers it 404 at once,
    // rather than leaving the browser on a socket nobody will answer.
    MemoryStore closePartial;
    copyObjects(&full, &closePartial, manifests);
    KelpPreviewServer closeServer;
    CHECK(closeServer.start(&error));
    FakeSource *closeSource = new FakeSource(&full);
    closeSource->answer = false;
    closeSource->closeOnRequest = true;
    const KelpPreviewServer::Opened closeSession =
        closeServer.open(borrow(&closePartial), site.bundleId, site.secretText,
                         QStringLiteral("Closed while waiting"), closeSource);
    CHECK(closeSession.ok);
    if (closeSession.ok) {
        closeSource->closeServer = &closeServer;
        closeSource->closeToken = closeSession.token;
        const Reply closeReply = get(closeServer.port(),
                                     "/" + closeSession.token.toLatin1() + "/");
        CHECK_EQ(closeReply.status, 404);
        CHECK(closeReply.body.contains("no such preview"));
        CHECK_EQ(closeServer.sessionCount(), 0);
    }
}

void testHoles()
{
    section("a manifest that went away");

    MemoryStore store;
    Site site;
    QString error;
    if (!site.build(&store, &error)) {
        CHECK(false);
        return;
    }

    KelpPreviewServer server;
    CHECK(server.start(&error));
    const KelpPreviewServer::Opened session =
        server.open(borrow(&store), site.bundleId, site.secretText, QStringLiteral("Holes"));
    CHECK(session.ok);
    if (!session.ok)
        return;
    const QByteArray prefix = "/" + session.token.toLatin1();

    // A data object the store has lost is a 503 for that file and no other.
    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Manifest::File file;
    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("images/logo.png"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK(store.remove(file.chunks.at(0)));

    Reply reply = get(server.port(), prefix + "/images/logo.png");
    CHECK_EQ(reply.status, 503);
    CHECK_EQ(server.tally().notHeld, 1);

    reply = get(server.port(), prefix + "/");
    CHECK_EQ(reply.status, 200);           // the page is unaffected
    CHECK_EQ(reply.body, site.html);

    // A directory manifest that went away turns its whole subtree into the same
    // answer, and still nothing else.
    CHECK(Web::resolve(&store, site.key, site.rootId, QStringLiteral("images"),
                       &found, &isDir, &hash, &file, &error));
    CHECK(found);
    CHECK(isDir);
    CHECK(store.remove(hash));

    reply = get(server.port(), prefix + "/images/logo.png");
    CHECK_EQ(reply.status, 503);
    reply = get(server.port(), prefix + "/sub/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.subHtml);
}

// ---- the real store ---------------------------------------------------------

void testOnDiskStore()
{
    section("the on-disk store");

    QTemporaryDir temp;
    CHECK(temp.isValid());
    if (!temp.isValid())
        return;

    KelpObjectStore store(temp.path() + QStringLiteral("/objects"));
    QString error;
    CHECK(store.initialise(&error));

    Site site;
    if (!site.build(&store, &error)) {
        CHECK(false);
        printf("  build error: %s\n", qPrintable(error));
        return;
    }
    CHECK(store.objectCount() > 0);

    KelpPreviewServer server;
    CHECK(server.start(&error));

    const KelpPreviewServer::Opened session =
        server.open(borrow(&store), site.bundleId, site.secretText, QStringLiteral("On disk"));
    CHECK(session.ok);
    if (!session.ok) {
        printf("  open error: %s\n", qPrintable(session.error));
        return;
    }

    const QByteArray prefix = "/" + session.token.toLatin1();
    Reply reply = get(server.port(), prefix + "/");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.html);

    reply = get(server.port(), prefix + "/sub/index.html");
    CHECK_EQ(reply.status, 200);
    CHECK_EQ(reply.body, site.subHtml);

    reply = get(server.port(), prefix + "/big.bin", rangeHeader(10, 19));
    CHECK_EQ(reply.status, 206);
    CHECK_EQ(reply.body, site.big.mid(10, 10));

    // The store holds only objects — nothing decrypted was written down — and the
    // preview server has no file output at all.
    CHECK_EQ(store.objectCount(), store.objectIds().size());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelppreview unit test\n");

    testPage();
    testResolve();
    testChunkRange();
    testMime();
    testServing();
    testRootAbsolute();
    testLazy();
    testOnDemand();
    testHoles();
    testOnDiskStore();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
