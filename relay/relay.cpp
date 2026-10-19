#include "relay.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "kelp_link.h"
#include "kelp_wire.h"
#include "kelptls.h"

namespace {

// A recipient request that gets no answer in this long is dropped, so a stalled
// tunnel cannot pin recipient sockets forever.
const qint64 kPendingTimeoutMs = 20000;

// A request line plus headers; anything longer is not a request we will read.
const int kMaxRequestBytes = 8 * 1024;

// The gateway's files are compiled into the binary (relay/gateway.qrc) so a
// relay is one file and serves the same loader wherever it runs.
QByteArray readResource(const char *path)
{
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Relay: cannot read" << path << "from the compiled-in resources";
        return QByteArray();
    }
    return file.readAll();
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return difference == 0;
}

// An HTTP request starts with a method: GET, POST, … — an ASCII capital. A wire
// frame starts with the high byte of a 4-byte length, which is 0 for anything
// smaller than 16 MiB (and we cap frames far below that). So the first byte
// tells the two apart with no ambiguity.
bool looksLikeHttp(char firstByte)
{
    return firstByte >= 'A' && firstByte <= 'Z';
}

} // namespace

Relay::Relay(QObject *parent)
    : QObject(parent),
      m_server(new QTcpServer(this)),
      m_sweeper(new QTimer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &Relay::onConnection);

    // The gateway's own files (design §5.2, C4b-plan.md).
    m_loaderHtml = readResource(":/kelp/gateway/loader.html");
    m_coreJs = readResource(":/kelp/gateway/core.js");
    m_pageJs = readResource(":/kelp/gateway/page.js");
    m_swJs = readResource(":/kelp/gateway/sw.js");

    m_sweeper->setInterval(5000);
    connect(m_sweeper, &QTimer::timeout, this, &Relay::sweep);
}

Relay::~Relay()
{
    qDeleteAll(m_connections);
}

bool Relay::start(quint16 port, const QByteArray &certPem, const QByteArray &keyPem,
                  QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    m_certificatePem = certPem;
    m_keyPem = keyPem;

    if (!m_server->listen(QHostAddress::Any, port)) {
        *error = QStringLiteral("cannot listen on %1: %2")
            .arg(port).arg(m_server->errorString());
        return false;
    }

    m_sweeper->start();
    qInfo() << "Relay: listening on" << m_server->serverPort();
    return true;
}

quint16 Relay::port() const { return m_server->serverPort(); }

void Relay::onConnection()
{
    while (m_server->hasPendingConnections())
        setup(m_server->nextPendingConnection());
}

void Relay::setup(QTcpSocket *socket)
{
    Connection *connection = new Connection;
    connection->socket = socket;
    connection->tls = new KelpTls(this);
    m_connections.append(connection);

    // A browser opens the gateway over this same port and speaks "http/1.1",
    // not "kelp/1" — a page that never loads is not a gateway. A publisher or
    // recipient tunnel still has to negotiate "kelp/1"; the role decision in
    // decide() is what keeps the two apart.
    connection->tls->setAlpnProtocols(QStringList()
                                      << QStringLiteral("kelp/1")
                                      << QStringLiteral("http/1.1"));

    QString error;
    if (!connection->tls->setCertificate(m_certificatePem, m_keyPem, &error)
        || !connection->tls->start(KelpTls::Server, socket, &error)) {
        qWarning() << "Relay: TLS setup failed:" << error;
        retire(connection);
        return;
    }

    connect(connection->tls, &KelpTls::readyRead, this, [this, connection]() {
        onData(connection);
    });
    // error() and closed() can both arrive; retire() is idempotent.
    connect(connection->tls, &KelpTls::error, this, [this, connection](const QString &message) {
        qWarning() << "Relay: connection error:" << message;
        retire(connection);
    });
    connect(connection->tls, &KelpTls::closed, this, [this, connection]() {
        retire(connection);
    });
}

void Relay::onData(Connection *connection)
{
    const QByteArray incoming = connection->tls->readAll();
    if (incoming.isEmpty())
        return;

    if (connection->role == Unknown) {
        connection->buffer += incoming;
        decide(connection);
        return;
    }
    if (connection->role == Tunnel) {
        feedReader(connection, incoming);
        return;
    }
    connection->buffer += incoming;
    // One connection may carry several requests — a browser reuses it, and a
    // client may have sent the next one already — so serve every whole one. A
    // request that has to ask the origin sets busy, which stops the loop until
    // the answer arrives.
    while (!connection->busy && tryRequest(connection)) {
    }
}

void Relay::decide(Connection *connection)
{
    if (connection->buffer.isEmpty())
        return;

    if (looksLikeHttp(connection->buffer.at(0))) {
        connection->role = Recipient;
        qInfo() << "Relay: recipient connected";
        while (!connection->busy && tryRequest(connection)) {
        }
        return;
    }

    connection->role = Tunnel;
    m_tunnels.insert(connection->tls, QList<QByteArray>());
    const QByteArray pending = connection->buffer;
    connection->buffer.clear();
    feedReader(connection, pending);
    qInfo() << "Relay: publisher tunnel connected";
}

void Relay::feedReader(Connection *connection, const QByteArray &data)
{
    connection->reader.push(data);
    quint8 type = 0;
    QByteArray payload;
    while (connection->reader.next(&type, &payload))
        onTunnelFrame(connection, type, payload);
    if (connection->reader.failed()) {
        qWarning() << "Relay: tunnel frame error:" << connection->reader.error();
        retire(connection);
    }
}

bool Relay::tryRequest(Connection *connection)
{
    if (connection->busy || connection->closeAfter)
        return false;

    const int end = connection->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (connection->buffer.size() > kMaxRequestBytes)
            respond(connection, 400, "Bad Request", "text/plain; charset=utf-8",
                    QByteArrayLiteral("request headers too large\n"));
        return false;
    }

    const QByteArray request = connection->buffer.left(end);
    connection->buffer.remove(0, end + 4);
    handleRequest(connection, request);
    return true;
}

void Relay::handleRequest(Connection *connection, const QByteArray &request)
{
    const QList<QByteArray> lines = request.split('\n');
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    if (requestLine.size() < 2
        || requestLine.at(0).trimmed() != QByteArrayLiteral("GET")) {
        respond(connection, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                QByteArrayLiteral("only GET is served\n"));
        return;
    }

    // Who wants the connection kept open. A browser speaking HTTP/1.1 does unless
    // it says otherwise; an HTTP/1.0 client does not unless it says so. Either
    // way the answer says which, and the connection ends when it said it would —
    // so a client that waits for the end of the response is not left waiting.
    const bool http11 = requestLine.value(2).trimmed() == QByteArrayLiteral("HTTP/1.1");
    bool keepAlive = http11;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        if (line.left(colon).trimmed().toLower() != QByteArrayLiteral("connection"))
            continue;
        keepAlive = line.mid(colon + 1).trimmed().toLower()
                        .contains(QByteArrayLiteral("keep-alive"));
    }
    connection->closeAfter = !keepAlive;

    QString path = QString::fromUtf8(requestLine.at(1).trimmed());
    const int query = path.indexOf(QLatin1Char('?'));
    if (query >= 0)
        path.truncate(query);

    // The gateway a browser opens (design §5.2): its own small path space under
    // /b/, and nothing else. Every bundle object still travels the token-gated
    // /v1/relay/ paths below, which the loader derives the token for itself.
    if (path == QLatin1String("/b") || path.startsWith(QLatin1String("/b/"))) {
        handleGatewayRequest(connection, path);
        return;
    }

    const QStringList segments = path.split(QLatin1Char('/'), QString::SkipEmptyParts);
    // /v1/relay/<token>/bundle/<bundle_id>/<info|object/<object_id>>
    if (segments.size() < 6 || segments.at(0) != QLatin1String("v1")
        || segments.at(1) != QLatin1String("relay")
        || segments.at(3) != QLatin1String("bundle")) {
        respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such endpoint\n"));
        return;
    }

    // The token in the path is compared to the one the publisher registered.
    // A wrong token, an unknown bundle and a bundle that is no longer published
    // all answer the same way, so the relay is not an enumeration oracle.
    QByteArray presented;
    QString error;
    if (!Kelp::Link::base64UrlDecode(segments.at(2), &presented, &error))
        presented.clear();
    QByteArray bundleId;
    if (!Kelp::Link::base32Decode(segments.at(4), &bundleId, &error))
        bundleId.clear();

    const bool known = m_bundles.contains(bundleId);
    const Bundle bundle = known ? m_bundles.value(bundleId) : Bundle();
    if (!known || !constantTimeEquals(presented, bundle.token)) {
        respond(connection, 401, "Unauthorized", "text/plain; charset=utf-8",
                QByteArrayLiteral("bad token\n"));
        return;
    }

    const QString subtree = segments.at(5);

    if (subtree == QLatin1String("info") && segments.size() == 6) {
        const QByteArray info =
            QStringLiteral("{\"v\":1,\"objects\":%1,\"files\":%2,\"bytes\":%3}\n")
                .arg(bundle.objects).arg(bundle.files)
                .arg(QString::number(bundle.bytes, 'f', 0)).toUtf8();
        respond(connection, 200, "OK", "application/json; charset=utf-8", info);
        return;
    }

    if (subtree == QLatin1String("object") && segments.size() == 7) {
        QByteArray objectId;
        if (!Kelp::Link::base32Decode(segments.at(6), &objectId, &error)) {
            respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("no such object\n"));
            return;
        }
        if (!bundle.tunnel) {
            respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("the origin is not connected\n"));
            return;
        }
        if (m_pending.contains(objectId)) {
            respond(connection, 503, "Service Unavailable", "text/plain; charset=utf-8",
                    QByteArrayLiteral("that object is already in flight\n"));
            return;
        }

        // Ask the origin; the answer arrives later and is written then.
        connection->busy = true;
        Pending pending;
        pending.recipient = connection;
        pending.bundleId = bundleId;
        pending.deadline = QDateTime::currentMSecsSinceEpoch() + kPendingTimeoutMs;
        m_pending.insert(objectId, pending);

        Kelp::Wire::WantMessage want;
        want.hash = objectId;
        bundle.tunnel->write(Kelp::Wire::encodeFrame(Kelp::Wire::Want,
                                                     Kelp::Wire::encodeWant(want)));
        return;
    }

    respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
            QByteArrayLiteral("no such endpoint\n"));
}

// The gateway a browser with no app opens: design §5.2. This is the loader page
// and the two scripts it needs, and nothing else. Nothing here is secret — the
// key is in the fragment, which no browser sends — and nothing here says whether
// a bundle exists: the page is public, its objects are not.
//
// The bundle's own files are never served here. The page's URL space is the
// bundle's path space (§5.3), and it is the *service worker in the reader's
// browser* that answers for it, decrypting what the token-gated paths below
// hand it.
void Relay::handleGatewayRequest(Connection *connection, const QString &path)
{
    const QString bundleId = path.section(QLatin1Char('/'), 2, 2);
    QByteArray id;
    QString error;
    if (bundleId.isEmpty()
        || !Kelp::Link::base32Decode(bundleId, &id, &error) || id.size() != 32) {
        respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such bundle\n"));
        return;
    }

    const QString prefix = QStringLiteral("/b/") + bundleId;
    const QString rest = path.mid(prefix.length());

    // One slash further, so the page's relative URLs and the worker's scope land
    // under the bundle's own prefix.
    if (rest.isEmpty()) {
        respond(connection, 301, "Moved Permanently", "text/plain; charset=utf-8",
                QByteArrayLiteral("the gateway page is one slash further\n"),
                QByteArrayLiteral("Location: ") + prefix.toUtf8() + "/\r\n");
        return;
    }

    if (rest == QLatin1String("/")) {
        if (m_loaderHtml.isEmpty()) {
            respond(connection, 500, "Internal Server Error", "text/plain; charset=utf-8",
                    QByteArrayLiteral("this relay was built without its gateway files\n"));
            return;
        }
        // The loader pulls nothing from outside itself: what it would fetch from
        // elsewhere is the sender's, and the sender is inside the bundle.
        respond(connection, 200, "OK", "text/html; charset=utf-8", m_loaderHtml,
                QByteArrayLiteral("Content-Security-Policy: default-src 'self'; "
                                  "img-src 'self' data: blob:; "
                                  "media-src 'self' blob:; "
                                  "style-src 'self' 'unsafe-inline'; "
                                  "connect-src 'self'"));
        return;
    }

    if (rest == QLatin1String("/.kelp/core.js")) {
        respond(connection, 200, "OK", "text/javascript; charset=utf-8", m_coreJs);
        return;
    }
    if (rest == QLatin1String("/.kelp/page.js")) {
        respond(connection, 200, "OK", "text/javascript; charset=utf-8", m_pageJs);
        return;
    }
    if (rest == QLatin1String("/.kelp/sw.js")) {
        // A worker script may only claim a scope above its own directory when its
        // response says so; that is what makes the bundle's own URL space — the
        // whole of /b/<id>/ — the space the worker answers in (§5.3).
        respond(connection, 200, "OK", "text/javascript; charset=utf-8", m_swJs,
                QByteArrayLiteral("Service-Worker-Allowed: ") + prefix.toUtf8() + "/\r\n");
        return;
    }

    respond(connection, 404, "Not Found", "text/plain; charset=utf-8",
            QByteArrayLiteral("only the gateway page is served here\n"));
}

// --- publisher tunnels --------------------------------------------------

void Relay::onTunnelFrame(Connection *connection, quint8 type, const QByteArray &payload)
{
    QString error;

    if (type == Kelp::Wire::Register) {
        Kelp::Wire::RegisterMessage registration;
        if (!Kelp::Wire::decodeRegister(payload, &registration, &error)) {
            qWarning() << "Relay: bad REGISTER:" << error;
            Kelp::Wire::ErrorMessage refusal;
            refusal.code = Kelp::Wire::BadToken;
            refusal.message = error;
            connection->tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Error,
                                                           Kelp::Wire::encodeError(refusal)));
            retire(connection);
            return;
        }

        Bundle bundle;
        bundle.token = registration.token;
        bundle.objects = registration.objects;
        bundle.files = registration.files;
        bundle.bytes = registration.bytes;
        bundle.tunnel = connection->tls;
        m_bundles.insert(registration.bundleId, bundle);
        if (!m_tunnels.value(connection->tls).contains(registration.bundleId))
            m_tunnels[connection->tls].append(registration.bundleId);

        qInfo() << "Relay: registered bundle" << registration.bundleId.toHex().left(16)
                << registration.objects << "objects";
        return;
    }

    if (type == Kelp::Wire::Data) {
        Kelp::Wire::DataMessage data;
        if (!Kelp::Wire::decodeData(payload, &data, &error)) {
            qWarning() << "Relay: bad DATA:" << error;
            return;
        }
        answerPending(data.hash, data.bytes, true);
        return;
    }

    if (type == Kelp::Wire::Reject) {
        Kelp::Wire::RejectMessage reject;
        if (!Kelp::Wire::decodeReject(payload, &reject, &error)) {
            qWarning() << "Relay: bad REJECT:" << error;
            return;
        }
        answerPending(reject.hash, QByteArray(), false);
        return;
    }

    if (type == Kelp::Wire::Ping) {
        quint64 t = 0;
        if (Kelp::Wire::decodePing(payload, &t, &error))
            connection->tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Pong,
                                                           Kelp::Wire::encodePing(t)));
        return;
    }

    if (type == Kelp::Wire::Error) {
        Kelp::Wire::ErrorMessage message;
        if (Kelp::Wire::decodeError(payload, &message, &error))
            qWarning() << "Relay: origin error" << message.code << message.message;
        return;
    }

    qWarning() << "Relay: unexpected tunnel frame type" << int(type);
}

void Relay::dropTunnel(Connection *connection)
{
    retire(connection);
}

void Relay::answerPending(const QByteArray &objectId, const QByteArray &bytes, bool found)
{
    if (!m_pending.contains(objectId))
        return;                                   // late or unsolicited
    const Pending pending = m_pending.take(objectId);
    if (!pending.recipient)
        return;

    pending.recipient->busy = false;
    if (found) {
        respond(pending.recipient, 200, "OK", "application/octet-stream", bytes);
    } else {
        respond(pending.recipient, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("the origin does not have that object\n"));
    }
    // A pipelined request may be waiting on this same connection.
    while (pending.recipient && !pending.recipient->busy && tryRequest(pending.recipient)) {
    }
}

void Relay::respond(Connection *connection, int status, const char *reason,
                    const char *contentType, const QByteArray &body,
                    const QByteArray &extraHeaders)
{
    if (!connection || !connection->tls)
        return;
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    head += "Content-Type: " + QByteArray(contentType) + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    if (connection->closeAfter)
        head += "Connection: close\r\n";
    // Extra headers carry their own CRLF endings; they are only ever ours.
    head += extraHeaders;
    head += "\r\n";
    connection->tls->write(head + body);
    if (connection->closeAfter) {
        // Written through OpenSSL and Qt's socket buffer, which disconnectFromHost
        // drains before the connection ends.
        connection->tls->close();
    }
}

void Relay::retire(Connection *connection)
{
    if (!m_connections.removeOne(connection))
        return;                                   // already retired

    // Whatever this connection was, it is gone now.
    if (connection->role == Tunnel) {
        const QList<QByteArray> bundles = m_tunnels.take(connection->tls);
        for (int i = 0; i < bundles.size(); ++i) {
            if (m_bundles.value(bundles.at(i)).tunnel == connection->tls)
                m_bundles.remove(bundles.at(i));
        }
    }

    // A recipient that is gone is not waiting for anything either: drop its
    // entries before anything can answer them. Browsers abandon requests
    // readily (a closed tab, a cancelled navigation), so this is the common
    // case, not the rare one.
    const QList<QByteArray> requests = m_pending.keys();
    for (int i = 0; i < requests.size(); ++i) {
        if (m_pending.value(requests.at(i)).recipient == connection)
            m_pending.remove(requests.at(i));
    }

    // Anything waiting on a bundle that no longer has a tunnel is never arriving.
    const QList<QByteArray> waiting = m_pending.keys();
    for (int i = 0; i < waiting.size(); ++i) {
        const Pending pending = m_pending.value(waiting.at(i));
        if (m_bundles.contains(pending.bundleId) && m_bundles.value(pending.bundleId).tunnel)
            continue;
        m_pending.remove(waiting.at(i));
        if (pending.recipient) {
            pending.recipient->busy = false;
            respond(pending.recipient, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("the origin is not connected\n"));
        }
    }

    connection->socket->deleteLater();
    connection->tls->deleteLater();
    delete connection;
}

void Relay::sweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<QByteArray> keys = m_pending.keys();
    for (int i = 0; i < keys.size(); ++i) {
        const Pending pending = m_pending.value(keys.at(i));
        if (now <= pending.deadline)
            continue;
        m_pending.remove(keys.at(i));
        if (pending.recipient) {
            pending.recipient->busy = false;
            respond(pending.recipient, 504, "Gateway Timeout", "text/plain; charset=utf-8",
                    QByteArrayLiteral("the origin did not answer in time\n"));
        }
    }
}
