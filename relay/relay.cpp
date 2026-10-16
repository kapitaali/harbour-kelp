#include "relay.h"

#include <QDateTime>
#include <QDebug>
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
    tryRequest(connection);
}

void Relay::decide(Connection *connection)
{
    if (connection->buffer.isEmpty())
        return;

    if (looksLikeHttp(connection->buffer.at(0))) {
        connection->role = Recipient;
        qInfo() << "Relay: recipient connected";
        tryRequest(connection);
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

void Relay::tryRequest(Connection *connection)
{
    if (connection->busy)
        return;

    const int end = connection->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (connection->buffer.size() > kMaxRequestBytes)
            respond(connection, 400, "Bad Request", "text/plain; charset=utf-8",
                    QByteArrayLiteral("request headers too large\n"));
        return;
    }

    const QByteArray request = connection->buffer.left(end);
    connection->buffer.remove(0, end + 4);
    handleRequest(connection, request);
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

    const QString path = QString::fromUtf8(requestLine.at(1).trimmed());
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
    tryRequest(pending.recipient);                // a pipelined request may be waiting
}

void Relay::respond(Connection *connection, int status, const char *reason,
                    const char *contentType, const QByteArray &body)
{
    if (!connection || !connection->tls)
        return;
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    head += "Content-Type: " + QByteArray(contentType) + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "\r\n";
    connection->tls->write(head + body);
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
