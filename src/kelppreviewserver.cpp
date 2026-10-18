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
      m_sweeper(new QTimer(this)), m_idleTimeoutMs(kDefaultIdleMs)
{
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onConnection()));

    m_sweeper->setInterval(kSweepIntervalMs);
    connect(m_sweeper, &QTimer::timeout, this, &KelpPreviewServer::sweep);
}

KelpPreviewServer::~KelpPreviewServer()
{
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

KelpPreviewServer::Opened KelpPreviewServer::open(const StorePtr &store,
                                                  const QString &bundleId,
                                                  const QString &secret,
                                                  const QString &name)
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

    return openPath(store, id, key, name, page.path);
}

KelpPreviewServer::Opened KelpPreviewServer::openEntry(const StorePtr &store,
                                                       const QString &bundleId,
                                                       const QString &secret,
                                                       const QString &name,
                                                       const QString &entry)
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

    return openPath(store, id, key, name, entry);
}

KelpPreviewServer::Opened KelpPreviewServer::openPath(const StorePtr &store, const QByteArray &id,
                                                      const QByteArray &key, const QString &name,
                                                      const QString &entry)
{
    Opened opened;

    Session session;
    session.store = store;
    session.bundleId = id;
    session.key = key;
    session.name = name;
    session.root = directoryOf(entry);
    session.entry = session.root.isEmpty() ? entry : entry.mid(session.root.size() + 1);
    session.lastMs = QDateTime::currentMSecsSinceEpoch();

    const QString token = QString::fromLatin1(Kelp::Crypto::randomBytes(32).toHex());
    if (token.isEmpty()) {
        opened.error = QStringLiteral("cannot make a preview token");
        return opened;
    }

    m_sessions.insert(token, session);

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
    if (m_sessions.remove(token) <= 0)
        return;
    qInfo() << "KelpPreview: closed a preview session";
    emit sessionsChanged();
}

int KelpPreviewServer::sessionCount() const
{
    return m_sessions.size();
}

void KelpPreviewServer::onConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();

        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_pending.remove(socket);
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
    for (int i = 0; i < gone.size(); ++i)
        m_sessions.remove(gone.at(i));
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

    // "/<token>/<path…>"; the target always begins with '/'.
    const QList<QByteArray> raw = rawTarget.split('/');
    if (raw.isEmpty() || !raw.at(0).isEmpty() || raw.size() < 2 || raw.at(1).isEmpty()) {
        ++m_tally.refused;
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such preview\n"), headOnly);
        return;
    }

    const QString token = QString::fromLatin1(raw.at(1)).toLower();
    QHash<QString, Session>::iterator found = m_sessions.find(token);
    if (found == m_sessions.end()) {
        // The token is the whole of the address: without it there is nothing to
        // tell an unknown preview from a wrong one, and nothing to enumerate.
        ++m_tally.refused;
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such preview\n"), headOnly);
        return;
    }

    Session &session = found.value();
    session.lastMs = QDateTime::currentMSecsSinceEpoch();

    // Percent-decode segment by segment: a "%2F" is a byte in a name, not a
    // separator, and nothing that fails the manifest's own name rules becomes a
    // path — ".." included, so the URL space cannot leave the bundle.
    QString urlPath;
    for (int i = 2; i < raw.size(); ++i) {
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
                QByteArrayLiteral("this bundle is not all here yet\n"), headOnly);
        return;
    }

    serve(socket, &session, target, rangeHeader, headOnly);
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

void KelpPreviewServer::serve(QTcpSocket *socket, Session *session, const Target &target,
                              const QByteArray &rangeHeader, bool headOnly)
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
    // honest 503.
    for (int i = first; i < first + count; ++i) {
        if (!session->store->contains(file.chunks.at(i))) {
            ++m_tally.notHeld;
            respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                    QByteArrayLiteral("this bundle is not all here yet\n"), headOnly);
            return;
        }
    }

    ++m_tally.requests;

    QByteArray extra = "Content-Type: " + Kelp::Web::mimeType(file, target.path).toUtf8() + "\r\n";
    extra += "Content-Length: " + QByteArray::number(length) + "\r\n";
    extra += "Accept-Ranges: bytes\r\n";
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
