#include "kelppreviewserver.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_object.h"
#include "kelp_web.h"
#include "kelpfetch.h"

namespace {

// A request line plus headers; anything longer is not a request we will read.
const int kMaxRequestBytes = 8 * 1024;

// Connections that have not asked for anything yet, and how long one may take to
// send its request. The audience is a browser on this device, so these are
// tighter than the LAN listener's.
const int kMaxPendingConnections = 16;
const int kRequestTimeoutMs = 10000;

// A page that is open but idle stops being served after this long. The page is
// holding a decrypted view of a bundle, so it should not outlive the visit that
// opened it; the browser fetches everything it needs within seconds.
const int kDefaultIdleMs = 30 * 60 * 1000;
const int kSweepIntervalMs = 60 * 1000;

// How many objects one session may ask its source for at once, and how long a
// single answer may take. The bound is the fetch loop's, so a page with forty
// chunks open does not open forty sockets to the sender. The deadline is the
// same twenty seconds a recipient's fetch gives a silent sender: a browser
// asking for an object deserves an answer, not a request that hangs.
const int kMaxOnDemandInflight = 4;
const int kDefaultFetchMs = 20000;

const QByteArray kNotAllHere = QByteArrayLiteral("this bundle is not all here yet\n");
const QByteArray kBadObject = QByteArrayLiteral(
    "the sender served something that did not match its address\n");
const QByteArray kTooLarge = QByteArrayLiteral(
    "this page is too large to keep in memory; ask the sender to share it as a folder\n");
const QByteArray kNoSuchPreview = QByteArrayLiteral(
    "no such preview: it has ended, or the address is wrong. Open the link again from Kelp.\n");

// A session token is 32 random bytes in hex. The shape matters when the browser
// asks for a root-absolute path: a first segment of that shape is a token even
// when this server has never minted it, so an unknown token stays a 404 instead
// of becoming a file name.
const char kPreviewCookie[] = "kelp_preview";

bool isTokenText(const QByteArray &value)
{
    if (value.size() != 64)
        return false;
    for (int i = 0; i < value.size(); ++i) {
        const char c = value.at(i);
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f') && (c < 'A' || c > 'F'))
            return false;
    }
    return true;
}

// The cookie a served page sets so that its own root-absolute URLs ("/style.css",
// "/sub/") still carry the capability when the browser asks for them. The token
// in the path is still the primary address; this is only how that address follows
// a page into the URL space the page itself wrote.
QByteArray previewCookie(const QList<QByteArray> &headerLines)
{
    for (int i = 1; i < headerLines.size(); ++i) {
        const QByteArray line = headerLines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        if (line.left(colon).trimmed().toLower() != QByteArrayLiteral("cookie"))
            continue;

        const QList<QByteArray> parts = line.mid(colon + 1).split(';');
        for (int j = 0; j < parts.size(); ++j) {
            const QByteArray part = parts.at(j).trimmed();
            const int equals = part.indexOf('=');
            if (equals <= 0)
                continue;
            if (part.left(equals).trimmed() != kPreviewCookie)
                continue;

            QByteArray value = part.mid(equals + 1).trimmed();
            if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
                value = value.mid(1, value.size() - 2);
            return value;
        }
    }
    return QByteArray();
}

QByteArray previewCookieHeader(const QString &token)
{
    return "Set-Cookie: " + QByteArray(kPreviewCookie) + '=' + token.toLatin1()
        + "; Path=/; HttpOnly; SameSite=Strict\r\n";
}

// The policy is what makes "this page works offline and cannot phone home" true in
// any browser. Inline style and script are allowed because a shared page is often
// one file; everything else must come from this origin. frame-ancestors stops the
// bundle being framed by another page, base-uri stops a <base> tag re-pointing
// relative URLs, and connect-src 'self' stops fetch()/XHR from leaving.
const char *kCsp =
    "default-src 'self'; img-src 'self' data:; media-src 'self'; "
    "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
    "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'";

QString directoryOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

// A `Range: bytes=…` header, understood just far enough to seek inside a file:
// "a-b", "a-" and "-n". Anything else — a multi-range request, a bad number — is
// ignored, and the whole file is served, which is always a valid answer.
struct ByteRange {
    bool present = false;
    bool ok = false;
    bool unsatisfiable = false;
    quint64 start = 0;
    quint64 length = 0;
};

ByteRange parseRange(const QByteArray &value, quint64 size)
{
    ByteRange range;
    if (value.isEmpty())
        return range;
    range.present = true;

    if (!value.startsWith(QByteArrayLiteral("bytes=")))
        return range;
    const QByteArray spec = value.mid(6).trimmed();
    if (spec.contains(','))
        return range;

    const int dash = spec.indexOf('-');
    if (dash < 0)
        return range;
    const QByteArray first = spec.left(dash).trimmed();
    const QByteArray last = spec.mid(dash + 1).trimmed();

    if (first.isEmpty()) {
        bool ok = false;
        const quint64 n = last.toULongLong(&ok);
        if (!ok || n == 0)
            return range;
        range.start = n >= size ? 0 : size - n;
        range.length = size - range.start;
        range.ok = true;
        return range;
    }

    bool ok = false;
    const quint64 start = first.toULongLong(&ok);
    if (!ok)
        return range;
    if (start >= size) {
        range.unsatisfiable = true;
        return range;
    }

    quint64 end = size - 1;
    if (!last.isEmpty()) {
        const quint64 parsed = last.toULongLong(&ok);
        if (!ok)
            return range;
        end = parsed;
    }
    if (end >= size)
        end = size - 1;
    if (end < start)
        return range;

    range.start = start;
    range.length = end - start + 1;
    range.ok = true;
    return range;
}

} // namespace

KelpPreviewServer::KelpPreviewServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)),
      m_sweeper(new QTimer(this)), m_fetchSweeper(new QTimer(this)),
      m_idleTimeoutMs(kDefaultIdleMs), m_fetchTimeoutMs(kDefaultFetchMs)
{
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onConnection()));

    m_sweeper->setInterval(kSweepIntervalMs);
    connect(m_sweeper, &QTimer::timeout, this, &KelpPreviewServer::sweep);

    // The idle sweep runs once a minute, which is too coarse for a request a
    // browser is waiting on. This one only enforces the on-demand deadline.
    m_fetchSweeper->setInterval(1000);
    connect(m_fetchSweeper, &QTimer::timeout, this, [this]() {
        timeoutFetches(QDateTime::currentMSecsSinceEpoch());
    });
}

KelpPreviewServer::~KelpPreviewServer()
{
    // The sessions go with the server, and so do the sources they borrowed for
    // on-demand fetches. Parked sockets are the app's own browser and go too.
    for (QHash<QString, FetchState>::iterator it = m_fetches.begin();
         it != m_fetches.end(); ++it) {
        if (it.value().source) {
            it.value().source->disconnect(this);
            it.value().source->stop();
            delete it.value().source;
        }
    }
    m_fetches.clear();
    m_parked.clear();
}

bool KelpPreviewServer::start(QString *error)
{
    if (m_server->isListening())
        return true;

    // Loopback only, on an ephemeral port: this is for the browser on *this*
    // device, and every URL carries a token because other processes can reach it.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        if (error)
            *error = QStringLiteral("cannot listen on 127.0.0.1: %1").arg(m_server->errorString());
        return false;
    }

    m_sweeper->start();
    m_fetchSweeper->start();
    qInfo() << "KelpPreview: listening on 127.0.0.1" << m_server->serverPort();
    return true;
}

bool KelpPreviewServer::isListening() const
{
    return m_server->isListening();
}

int KelpPreviewServer::port() const
{
    return m_server->isListening() ? int(m_server->serverPort()) : 0;
}

void KelpPreviewServer::setIdleTimeout(int ms)
{
    m_idleTimeoutMs = ms;
}

void KelpPreviewServer::setFetchTimeout(int ms)
{
    m_fetchTimeoutMs = ms;
    // A test may lower the deadline far below a second; checking more often
    // than the deadline would be wasted work in production.
    m_fetchSweeper->setInterval(qMax(50, qMin(1000, ms)));
}

KelpPreviewServer::Opened KelpPreviewServer::open(const StorePtr &store,
                                                  const QString &bundleId,
                                                  const QString &secret,
                                                  const QString &name,
                                                  KelpSource *source,
                                                  quint64 storeLimit)
{
    Opened opened;

    if (!m_server->isListening()) {
        opened.error = QStringLiteral("the preview listener is not running");
        return opened;
    }
    if (!store) {
        opened.error = QStringLiteral("there is no store to serve this page from");
        return opened;
    }

    QByteArray id;
    QByteArray master;
    QString error;
    if (!Kelp::Link::base32Decode(bundleId, &id, &error)) {
        opened.error = QStringLiteral("this is not a bundle id");
        return opened;
    }
    if (!Kelp::Link::base64UrlDecode(secret, &master, &error)) {
        opened.error = QStringLiteral("this is not a link secret");
        return opened;
    }

    const QByteArray key = Kelp::contentKey(master);

    Kelp::Web::Page page;
    if (!Kelp::Web::page(store.data(), key, id, &page, &error)) {
        opened.error = error;
        return opened;
    }
    opened.candidates = page.candidates;
    if (page.path.isEmpty()) {
        opened.error = QStringLiteral("this bundle has no web page in it");
        return opened;
    }

    return openPath(store, id, key, name, page.path, source, storeLimit);
}

KelpPreviewServer::Opened KelpPreviewServer::openEntry(const StorePtr &store,
                                                       const QString &bundleId,
                                                       const QString &secret,
                                                       const QString &name,
                                                       const QString &entry,
                                                       KelpSource *source,
                                                       quint64 storeLimit)
{
    Opened opened;

    if (!m_server->isListening()) {
        opened.error = QStringLiteral("the preview listener is not running");
        return opened;
    }
    if (!store) {
        opened.error = QStringLiteral("there is no store to serve this page from");
        return opened;
    }

    QByteArray id;
    QByteArray master;
    QString error;
    if (!Kelp::Link::base32Decode(bundleId, &id, &error)
        || !Kelp::Link::base64UrlDecode(secret, &master, &error)) {
        opened.error = QStringLiteral("this is not a bundle id and secret");
        return opened;
    }
    const QByteArray key = Kelp::contentKey(master);

    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Kelp::Manifest::File file;
    if (!Kelp::Web::resolve(store.data(), key, id, entry, &found, &isDir, &hash, &file, &error)) {
        opened.error = error;
        return opened;
    }
    if (!found || isDir) {
        opened.error = QStringLiteral("this bundle has no such page");
        return opened;
    }

    return openPath(store, id, key, name, entry, source, storeLimit);
}

KelpPreviewServer::Opened KelpPreviewServer::openPath(const StorePtr &store, const QByteArray &id,
                                                      const QByteArray &key, const QString &name,
                                                      const QString &entry, KelpSource *source,
                                                      quint64 storeLimit)
{
    Opened opened;

    Session session;
    session.store = store;
    session.bundleId = id;
    session.key = key;
    session.name = name;
    session.root = directoryOf(entry);
    session.entry = session.root.isEmpty() ? entry : entry.mid(session.root.size() + 1);
    session.storeLimit = storeLimit;
    session.lastMs = QDateTime::currentMSecsSinceEpoch();

    const QString token = QString::fromLatin1(Kelp::Crypto::randomBytes(32).toHex());
    if (token.isEmpty()) {
        opened.error = QStringLiteral("cannot make a preview token");
        return opened;
    }

    m_sessions.insert(token, session);

    // A source, when the caller has one, is this session's alone: the page is
    // the only thing it fetches for, and it is stopped when the session ends.
    // The callbacks are connected with the token rather than the session, whose
    // hash entry may move as sessions come and go.
    if (source) {
        source->setParent(this);
        FetchState fetch;
        fetch.source = source;
        m_fetches.insert(token, fetch);
        connect(source, &KelpSource::objectReady, this,
                [this, token](const QByteArray &objectId, const QByteArray &bytes) {
                    onObject(token, objectId, bytes);
                });
        connect(source, &KelpSource::failed, this,
                [this, token](const QString &message) {
                    onFetchFailed(token, message);
                });
    }

    opened.ok = true;
    opened.token = token;
    opened.url = QStringLiteral("http://127.0.0.1:%1/%2/")
                     .arg(m_server->serverPort()).arg(token);
    opened.entry = entry;
    opened.root = session.root;

    // Deliberately without the token: the URL is a read capability for the
    // decrypted bundle while the session lives, and the log is not the place for
    // one — the same rule a link's fragment is under.
    qInfo() << QStringLiteral("KelpPreview: serving \"%1\" from \"%2\" on 127.0.0.1:%3")
                   .arg(entry, name)
                   .arg(m_server->serverPort());

    emit sessionsChanged();
    return opened;
}

void KelpPreviewServer::close(const QString &token)
{
    const bool had = m_sessions.remove(token) > 0;

    // A parked browser request outlives the session only long enough to be told
    // the preview is gone; the source stops with it.
    dropFetch(token, true);

    if (!had)
        return;
    qInfo() << "KelpPreview: closed a preview session";
    emit sessionsChanged();
}

int KelpPreviewServer::sessionCount() const
{
    return m_sessions.size();
}

bool KelpPreviewServer::has(const QString &token) const
{
    return m_sessions.contains(token);
}

void KelpPreviewServer::onConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();

        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_pending.remove(socket);
            // A browser that gives up while an object is being fetched must not
            // be answered later, and its waiting slot must not keep a fetch
            // alive after every reader has gone.
            removeParked(socket);
            socket->deleteLater();
        });

        if (m_pending.size() >= kMaxPendingConnections) {
            respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                    QByteArrayLiteral("too many unfinished requests\n"), false);
            continue;
        }

        Pending entry;
        entry.accepted = QDateTime::currentMSecsSinceEpoch();
        m_pending.insert(socket, entry);

        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            readRequest(socket);
        });
    }
}

void KelpPreviewServer::readRequest(QTcpSocket *socket)
{
    QHash<QTcpSocket *, Pending>::iterator it = m_pending.find(socket);
    if (it == m_pending.end())
        return;
    Pending &pending = it.value();
    pending.buffer += socket->readAll();

    const int end = pending.buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (pending.buffer.size() > kMaxRequestBytes) {
            m_pending.remove(socket);
            respond(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                    QByteArrayLiteral("request headers too large\n"), false);
        }
        return;
    }

    const QByteArray request = pending.buffer.left(end);
    m_pending.remove(socket);
    handle(socket, request);
}

void KelpPreviewServer::sweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // A fetch that has gone quiet is a 503 for the requests waiting on it; a
    // request that has waited too long in all is a 503 even if its object is
    // still queued behind others.
    timeoutFetches(now);

    QList<QTcpSocket *> stale;
    QHash<QTcpSocket *, Pending>::const_iterator it = m_pending.constBegin();
    for (; it != m_pending.constEnd(); ++it) {
        if (now - it.value().accepted > kRequestTimeoutMs)
            stale.append(it.key());
    }
    for (int i = 0; i < stale.size(); ++i) {
        QTcpSocket *socket = stale.at(i);
        m_pending.remove(socket);
        respond(socket, 408, "Request Timeout", "text/plain; charset=utf-8",
                QByteArrayLiteral("waited too long for a request\n"), false);
    }

    QStringList gone;
    QHash<QString, Session>::const_iterator session = m_sessions.constBegin();
    for (; session != m_sessions.constEnd(); ++session) {
        if (now - session.value().lastMs > m_idleTimeoutMs)
            gone.append(session.key());
    }
    for (int i = 0; i < gone.size(); ++i) {
        m_sessions.remove(gone.at(i));
        dropFetch(gone.at(i), true);
    }
    if (!gone.isEmpty()) {
        qInfo() << "KelpPreview:" << gone.size() << "idle session(s) dropped";
        emit sessionsChanged();
    }
}

void KelpPreviewServer::handle(QTcpSocket *socket, const QByteArray &request)
{
    const QList<QByteArray> lines = request.split('\n');
    const QList<QByteArray> requestLine = lines.at(0).trimmed().split(' ');
    if (requestLine.size() < 2) {
        respond(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                QByteArrayLiteral("malformed request line\n"), false);
        return;
    }

    const QByteArray method = requestLine.at(0).trimmed();
    const bool headOnly = (method == QByteArrayLiteral("HEAD"));
    if (method != QByteArrayLiteral("GET") && !headOnly) {
        respond(socket, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                QByteArrayLiteral("only GET and HEAD are served\n"), headOnly);
        return;
    }

    QByteArray rawTarget = requestLine.at(1).trimmed();
    const int query = rawTarget.indexOf('?');
    if (query >= 0)
        rawTarget = rawTarget.left(query);

    QByteArray rangeHeader;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == QByteArrayLiteral("range"))
            rangeHeader = value;
    }

    // The token is normally the first path segment ("/<token>/…"). A page's own
    // root-absolute URLs ("/style.css", "/sub/") do not carry it, so the page
    // response also sets the same token as a loopback cookie; those requests are
    // matched through it. Either way the token is the whole capability, and an
    // unknown token remains the same 404.
    const QList<QByteArray> raw = rawTarget.split('/');
    if (raw.isEmpty() || !raw.at(0).isEmpty() || raw.size() < 2) {
        ++m_tally.refused;
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8", kNoSuchPreview,
                headOnly);
        return;
    }

    QString token;
    int pathStart = 1;
    if (!raw.at(1).isEmpty()) {
        const QString candidate = QString::fromLatin1(raw.at(1)).toLower();
        if (m_sessions.contains(candidate)) {
            token = candidate;
            pathStart = 2;
        } else if (isTokenText(raw.at(1))) {
            ++m_tally.refused;
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8", kNoSuchPreview,
                    headOnly);
            return;
        }
    }

    if (token.isEmpty()) {
        const QByteArray cookie = previewCookie(lines);
        const QString candidate = QString::fromLatin1(cookie).toLower();
        if (cookie.isEmpty() || !m_sessions.contains(candidate)) {
            ++m_tally.refused;
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8", kNoSuchPreview,
                    headOnly);
            return;
        }
        token = candidate;
    }

    QHash<QString, Session>::iterator found = m_sessions.find(token);
    if (found == m_sessions.end()) {
        // The token is the whole of the address: without it there is nothing to
        // tell an unknown preview from a wrong one, and nothing to enumerate. The
        // body says what a reader can do about it: a page that worked a moment ago
        // has usually just been idle past its timeout (§3.25).
        ++m_tally.refused;
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8", kNoSuchPreview,
                headOnly);
        return;
    }

    Session &session = found.value();
    session.lastMs = QDateTime::currentMSecsSinceEpoch();

    // Percent-decode segment by segment: a "%2F" is a byte in a name, not a
    // separator, and nothing that fails the manifest's own name rules becomes a
    // path — ".." included, so the URL space cannot leave the bundle.
    QString urlPath;
    for (int i = pathStart; i < raw.size(); ++i) {
        if (raw.at(i).isEmpty())
            continue;
        const QString segment = QUrl::fromPercentEncoding(raw.at(i));
        if (!Kelp::Manifest::isValidName(segment)) {
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("no such file\n"), headOnly);
            return;
        }
        if (!urlPath.isEmpty())
            urlPath += QLatin1Char('/');
        urlPath += segment;
    }

    const Target target = resolve(&session, urlPath, rawTarget.endsWith('/'));

    if (target.status == 301) {
        QByteArray extra = "Location: " + rawTarget + "/\r\n";
        extra += "Content-Type: text/plain; charset=utf-8\r\n";
        extra += "Content-Length: 0\r\n";
        writeHead(socket, 301, "Moved Permanently", extra, headOnly);
        socket->disconnectFromHost();
        return;
    }

    if (target.status == 404) {
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such file in this bundle\n"), headOnly);
        return;
    }

    if (target.status == 503) {
        ++m_tally.notHeld;
        respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                kNotAllHere, headOnly);
        return;
    }

    serve(socket, &session, token, target, rangeHeader, headOnly);
}

KelpPreviewServer::Target KelpPreviewServer::resolve(Session *session, const QString &urlPath,
                                                     bool trailingSlash)
{
    Target target;
    target.status = 404;

    // An empty URL path is the URL root, which is the page itself.
    const QString relative = urlPath.isEmpty() ? session->entry : urlPath;
    const QString full = session->root.isEmpty()
        ? relative
        : (relative.isEmpty() ? session->root
                              : session->root + QLatin1Char('/') + relative);

    bool found = false;
    bool isDir = false;
    QByteArray hash;
    Kelp::Manifest::File file;
    QString error;

    if (!Kelp::Web::resolve(session->store.data(), session->key, session->bundleId, full,
                            &found, &isDir, &hash, &file, &error)) {
        // Not "no such path": the bundle is not all here yet, which is a 503.
        target.status = 503;
        return target;
    }

    if (!found)
        return target;

    if (isDir) {
        // A directory serves its own index.html, and the URL must carry the slash
        // or the page's relative links would resolve against the parent. The
        // trailing slash is a fact about the request, not about the path: it is
        // the only thing that tells "/sub/" from "/sub".
        if (!urlPath.isEmpty() && !trailingSlash) {
            target.status = 301;
            return target;
        }

        const QString index = full.isEmpty() ? QStringLiteral("index.html")
                                             : full + QLatin1String("/index.html");
        found = false;
        isDir = false;
        if (!Kelp::Web::resolve(session->store.data(), session->key, session->bundleId, index,
                                &found, &isDir, &hash, &file, &error)) {
            target.status = 503;
            return target;
        }
        if (!found || isDir)
            return target;

        target.status = 200;
        target.path = index;
        target.file = file;
        target.hash = hash;
        return target;
    }

    target.status = 200;
    target.path = full;
    target.file = file;
    target.hash = hash;
    return target;
}

void KelpPreviewServer::serve(QTcpSocket *socket, Session *session, const QString &token,
                              const Target &target, const QByteArray &rangeHeader, bool headOnly)
{
    const Kelp::Manifest::File &file = target.file;

    const ByteRange range = parseRange(rangeHeader, file.size);
    if (range.present && !range.ok && range.unsatisfiable) {
        const QByteArray extra = "Content-Range: bytes */" + QByteArray::number(file.size) + "\r\n";
        respond(socket, 416, "Range Not Satisfiable", "text/plain; charset=utf-8",
                QByteArrayLiteral("that range is not in this file\n"), headOnly, extra);
        return;
    }

    quint64 start = 0;
    quint64 length = file.size;
    bool partial = false;
    if (range.ok) {
        start = range.start;
        length = range.length;
        partial = true;
    }

    int first = 0;
    int count = 0;
    if (!Kelp::Web::chunkRange(file, start, length, &first, &count)) {
        respond(socket, 500, "Internal Server Error", "text/plain; charset=utf-8",
                QByteArrayLiteral("the manifest does not describe this file\n"), headOnly);
        return;
    }

    // Every object the answer needs is checked before a byte goes out: a missing
    // one means the bundle is not all here yet, and half a page is worse than an
    // honest 503. A session with a source does not stop there: it parks the
    // request, fetches exactly those objects, and answers once they are stored
    // and verified. Without a source this is the old 503, unchanged.
    QSet<QByteArray> missing;
    for (int i = first; i < first + count; ++i) {
        const QByteArray &id = file.chunks.at(i);
        if (!session->store->contains(id))
            missing.insert(id);
    }
    if (!missing.isEmpty()) {
        QHash<QString, FetchState>::const_iterator fetch = m_fetches.constFind(token);
        if (fetch == m_fetches.constEnd() || !fetch.value().source) {
            ++m_tally.notHeld;
            respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                    kNotAllHere, headOnly);
            return;
        }
        parkRequest(socket, token, target, rangeHeader, headOnly, missing);
        return;
    }

    ++m_tally.requests;

    QByteArray extra = "Content-Type: " + Kelp::Web::mimeType(file, target.path).toUtf8() + "\r\n";
    extra += "Content-Length: " + QByteArray::number(length) + "\r\n";
    extra += "Accept-Ranges: bytes\r\n";
    // The page may name its own assets by an absolute path. This host-only cookie
    // keeps those requests inside the same capability; it is scoped to loopback
    // and lasts only as long as the browser session.
    extra += previewCookieHeader(token);
    if (partial) {
        extra += "Content-Range: bytes " + QByteArray::number(start) + "-"
               + QByteArray::number(start + length - 1) + "/"
               + QByteArray::number(file.size) + "\r\n";
    }

    writeHead(socket, partial ? 206 : 200, partial ? "Partial Content" : "OK", extra, headOnly);
    if (headOnly) {
        socket->disconnectFromHost();
        return;
    }

    // Decrypted straight to the socket, one chunk at a time: a bundle too large to
    // hold in memory is still previewable, and nothing decrypted is ever written
    // down.
    quint64 skip = start % quint64(Kelp::Manifest::ChunkSize);
    quint64 remaining = length;
    for (int i = first; i < first + count && remaining > 0; ++i) {
        QByteArray object;
        if (!session->store->get(file.chunks.at(i), &object)) {
            socket->abort();               // the store lost it since the check
            return;
        }
        QByteArray plaintext;
        QString error;
        if (!Kelp::Object::open(session->key, object, &plaintext, &error)) {
            socket->abort();
            return;
        }
        if (skip > 0) {
            plaintext.remove(0, int(skip));
            skip = 0;
        }
        if (quint64(plaintext.size()) > remaining)
            plaintext.truncate(int(remaining));
        socket->write(plaintext);
        m_tally.bytes += plaintext.size();
        remaining -= quint64(plaintext.size());
    }

    socket->disconnectFromHost();
}

void KelpPreviewServer::parkRequest(QTcpSocket *socket, const QString &token,
                                    const Target &target, const QByteArray &rangeHeader,
                                    bool headOnly, const QSet<QByteArray> &missing)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end() || !found.value().source) {
        ++m_tally.notHeld;
        respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                kNotAllHere, headOnly);
        return;
    }

    ParkedRequest request;
    request.socket = socket;
    request.token = token;
    request.path = target.path;
    request.file = target.file;
    request.rangeHeader = rangeHeader;
    request.headOnly = headOnly;
    request.parkedMs = QDateTime::currentMSecsSinceEpoch();
    request.outstanding = missing;
    m_parked.insert(socket, request);

    FetchState &fetch = found.value();
    for (QSet<QByteArray>::const_iterator it = missing.constBegin();
         it != missing.constEnd(); ++it) {
        fetch.waiters[*it].insert(socket);
        if (!fetch.inFlight.contains(*it) && !fetch.queued.contains(*it)) {
            fetch.queued.insert(*it);
            fetch.queue.enqueue(*it);
        }
    }

    qInfo() << "KelpPreview: waiting for" << missing.size()
            << "object(s) before answering a request";
    pumpFetches(token);
}

void KelpPreviewServer::pumpFetches(const QString &token)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end())
        return;
    FetchState &fetch = found.value();

    for (;;) {
        if (!fetch.source || fetch.inFlight.size() >= kMaxOnDemandInflight)
            return;

        // A queued id nobody is waiting for any more is not worth a request.
        while (!fetch.queue.isEmpty()) {
            const QByteArray head = fetch.queue.head();
            if (fetch.waiters.contains(head) && !fetch.waiters.value(head).isEmpty())
                break;
            fetch.queued.remove(fetch.queue.dequeue());
        }
        if (fetch.queue.isEmpty())
            return;

        const QByteArray id = fetch.queue.dequeue();
        fetch.queued.remove(id);
        if (fetch.inFlight.contains(id))
            continue;

        fetch.inFlight.insert(id);
        fetch.startedMs.insert(id, QDateTime::currentMSecsSinceEpoch());
        fetch.source->requestObject(id);
    }
}

void KelpPreviewServer::onObject(const QString &token, const QByteArray &id,
                                 const QByteArray &bytes)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end())
        return;
    FetchState &fetch = found.value();
    if (!fetch.inFlight.remove(id))
        return;
    fetch.startedMs.remove(id);

    QHash<QString, Session>::iterator session = m_sessions.find(token);
    if (session == m_sessions.end()) {
        pumpFetches(token);
        return;
    }

    // A received page has a memory ceiling, and this session is now the thing
    // that stores its content. Refuse before storing, so the promise is real
    // even when the sender's `/info` understated the bundle. The whole session
    // stops: an object that does not fit is not a hole another request can fill.
    if (session.value().storeLimit > 0) {
        const qint64 used = session.value().store->byteCount();
        if (used >= 0
            && quint64(used) + quint64(bytes.size()) > session.value().storeLimit) {
            ++m_tally.overLimit;
            qWarning() << "KelpPreview: the view store reached its ceiling;"
                       << "stopping on-demand fetches";
            failAllFetches(token, kTooLarge);
            return;
        }
    }

    // The id is the sender's promise. What arrives is stored only if it hashes
    // to that id; a mismatch is a 502 because retrying the same address would
    // only repeat the lie, and the diagnosis differs from "not here yet".
    if (!Kelp::storeReceived(session.value().store.data(), id, bytes)) {
        ++m_tally.mismatched;
        qWarning() << "KelpPreview: an on-demand object did not match its id";
        failWaiters(token, id, 502, "Bad Gateway", kBadObject);
        pumpFetches(token);
        return;
    }
    ++m_tally.onDemand;

    const QSet<QTcpSocket *> sockets = fetch.waiters.take(id);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (QSet<QTcpSocket *>::const_iterator it = sockets.constBegin();
         it != sockets.constEnd(); ++it) {
        QTcpSocket *socket = *it;
        QHash<QTcpSocket *, ParkedRequest>::iterator parked = m_parked.find(socket);
        if (parked == m_parked.end())
            continue;
        parked.value().outstanding.remove(id);
        parked.value().parkedMs = now;       // progress: the wait starts again
        if (!parked.value().outstanding.isEmpty())
            continue;

        const ParkedRequest request = parked.value();
        removeParked(socket);

        Target target;
        target.status = 200;
        target.path = request.path;
        target.file = request.file;
        serve(socket, &session.value(), token, target, request.rangeHeader,
              request.headOnly);
    }

    pumpFetches(token);
}

void KelpPreviewServer::onFetchFailed(const QString &token, const QString &message)
{
    // The message can carry a source's own words, which for a relay includes an
    // address. The preview log says only that the attempt failed.
    Q_UNUSED(message);
    qWarning() << "KelpPreview: an on-demand fetch failed; answering 503";
    failAllFetches(token, kNotAllHere);
}

void KelpPreviewServer::failWaiters(const QString &token, const QByteArray &id, int status,
                                    const char *reason, const QByteArray &body)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end())
        return;
    FetchState &fetch = found.value();
    const QSet<QTcpSocket *> sockets = fetch.waiters.take(id);
    fetch.inFlight.remove(id);
    fetch.startedMs.remove(id);

    for (QSet<QTcpSocket *>::const_iterator it = sockets.constBegin();
         it != sockets.constEnd(); ++it) {
        QTcpSocket *socket = *it;
        QHash<QTcpSocket *, ParkedRequest>::iterator parked = m_parked.find(socket);
        if (parked == m_parked.end())
            continue;
        const bool headOnly = parked.value().headOnly;
        removeParked(socket);
        if (status == 503)
            ++m_tally.notHeld;
        respond(socket, status, reason, "text/plain; charset=utf-8", body, headOnly);
    }
}

void KelpPreviewServer::failAllFetches(const QString &token, const QByteArray &body)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end())
        return;
    FetchState &fetch = found.value();
    KelpSource *source = fetch.source;
    fetch.source = nullptr;

    const QList<QByteArray> ids = fetch.waiters.keys();
    for (int i = 0; i < ids.size(); ++i)
        failWaiters(token, ids.at(i), 503, "Service Unavailable", body);

    fetch.queue.clear();
    fetch.queued.clear();
    fetch.inFlight.clear();
    fetch.startedMs.clear();
    fetch.waiters.clear();

    if (source) {
        source->disconnect(this);
        source->stop();
        source->deleteLater();
    }
}

void KelpPreviewServer::removeParked(QTcpSocket *socket)
{
    if (m_parked.remove(socket) <= 0)
        return;

    for (QHash<QString, FetchState>::iterator fit = m_fetches.begin();
         fit != m_fetches.end(); ++fit) {
        QHash<QByteArray, QSet<QTcpSocket *>> &waiters = fit.value().waiters;
        for (QHash<QByteArray, QSet<QTcpSocket *>>::iterator it = waiters.begin();
             it != waiters.end(); ) {
            it.value().remove(socket);
            if (it.value().isEmpty())
                it = waiters.erase(it);
            else
                ++it;
        }
    }
}

void KelpPreviewServer::dropFetch(const QString &token, bool answerParked)
{
    QHash<QString, FetchState>::iterator found = m_fetches.find(token);
    if (found == m_fetches.end())
        return;

    FetchState &fetch = found.value();
    KelpSource *source = fetch.source;
    fetch.source = nullptr;

    QSet<QTcpSocket *> sockets;
    for (QHash<QByteArray, QSet<QTcpSocket *>>::const_iterator it = fetch.waiters.constBegin();
         it != fetch.waiters.constEnd(); ++it)
        sockets.unite(it.value());

    fetch.waiters.clear();
    fetch.inFlight.clear();
    fetch.queued.clear();
    fetch.queue.clear();
    fetch.startedMs.clear();
    m_fetches.erase(found);

    if (source) {
        source->disconnect(this);
        source->stop();
        source->deleteLater();
    }

    for (QSet<QTcpSocket *>::const_iterator it = sockets.constBegin();
         it != sockets.constEnd(); ++it) {
        QTcpSocket *socket = *it;
        QHash<QTcpSocket *, ParkedRequest>::iterator parked = m_parked.find(socket);
        if (parked == m_parked.end())
            continue;
        const bool headOnly = parked.value().headOnly;
        m_parked.remove(socket);
        if (answerParked)
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                    kNoSuchPreview, headOnly);
        else
            socket->abort();
    }
}

void KelpPreviewServer::timeoutFetches(qint64 now)
{
    QList<QPair<QString, QByteArray>> late;
    for (QHash<QString, FetchState>::const_iterator fit = m_fetches.constBegin();
         fit != m_fetches.constEnd(); ++fit) {
        const QHash<QByteArray, qint64> &started = fit.value().startedMs;
        for (QHash<QByteArray, qint64>::const_iterator it = started.constBegin();
             it != started.constEnd(); ++it) {
            if (now - it.value() > m_fetchTimeoutMs)
                late.append(qMakePair(fit.key(), it.key()));
        }
    }

    for (int i = 0; i < late.size(); ++i) {
        failWaiters(late.at(i).first, late.at(i).second, 503, "Service Unavailable",
                    kNotAllHere);
        pumpFetches(late.at(i).first);
    }

    // A request whose objects are still queued behind others has no in-flight
    // deadline of its own. The same twenty seconds bounds the whole wait.
    QList<QTcpSocket *> stale;
    for (QHash<QTcpSocket *, ParkedRequest>::const_iterator it = m_parked.constBegin();
         it != m_parked.constEnd(); ++it) {
        if (now - it.value().parkedMs > m_fetchTimeoutMs)
            stale.append(it.key());
    }
    for (int i = 0; i < stale.size(); ++i) {
        QTcpSocket *socket = stale.at(i);
        QHash<QTcpSocket *, ParkedRequest>::iterator parked = m_parked.find(socket);
        if (parked == m_parked.end())
            continue;
        const bool headOnly = parked.value().headOnly;
        removeParked(socket);
        ++m_tally.notHeld;
        respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                kNotAllHere, headOnly);
    }
}

void KelpPreviewServer::writeHead(QTcpSocket *socket, int status, const char *reason,
                                  const QByteArray &extraHeaders, bool headOnly)
{
    Q_UNUSED(headOnly);

    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    head += extraHeaders;
    head += securityHeaders();
    head += "Connection: close\r\n";
    head += "\r\n";

    socket->write(head);
}

void KelpPreviewServer::respond(QTcpSocket *socket, int status, const char *reason,
                                const char *contentType, const QByteArray &body,
                                bool headOnly, const QByteArray &extraHeaders)
{
    QByteArray extra = "Content-Type: " + QByteArray(contentType) + "\r\n";
    extra += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    extra += extraHeaders;

    writeHead(socket, status, reason, extra, headOnly);
    if (!headOnly)
        socket->write(body);
    socket->disconnectFromHost();
}

QByteArray KelpPreviewServer::securityHeaders()
{
    QByteArray headers;
    headers += "Content-Security-Policy: " + QByteArray(kCsp) + "\r\n";
    headers += "X-Content-Type-Options: nosniff\r\n";
    headers += "Referrer-Policy: no-referrer\r\n";
    headers += "Cache-Control: no-store\r\n";
    return headers;
}
