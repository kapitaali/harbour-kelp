#include "kelpserver.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "kelp_link.h"
#include "kelpstore.h"

namespace {

// A request line plus headers; anything longer is not a request we will read.
const int kMaxRequestBytes = 8 * 1024;

// Connections that have not asked for anything yet, and how long one may take to
// send its request. Without these a peer could hold sockets open until the app is
// restarted; the wire spec's DoS section (§13) is the full story, this is the
// floor.
const int kMaxPendingConnections = 32;
const int kRequestTimeoutMs = 10000;

// Comparing a token must not leak how much of it was right, so compare every
// byte rather than returning at the first difference.
bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return difference == 0;
}

// Qt 5.6 has no QHostAddress::isLinkLocal(), and an address that only reaches
// the local segment (169.254.0.0/16) is useless in a link.
bool isLinkLocal(const QHostAddress &address)
{
    const quint32 v4 = address.toIPv4Address();
    return (v4 & 0xffff0000u) == 0xa9fe0000u;
}

bool usable(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol
        && !address.isLoopback() && !isLinkLocal(address);
}

} // namespace

KelpServer::KelpServer(KelpObjectStore *store, QObject *parent)
    : QObject(parent), m_store(store), m_server(new QTcpServer(this)),
      m_sweeper(new QTimer(this)), m_port(0)
{
    connect(m_server, SIGNAL(newConnection()), this, SLOT(onConnection()));

    m_sweeper->setInterval(kRequestTimeoutMs / 2);
    connect(m_sweeper, &QTimer::timeout, this, &KelpServer::sweepPending);
}

QHostAddress KelpServer::lanHostAddress()
{
    // "Which interface is the LAN" has no exact answer in Qt, so this is two
    // passes: prefer an ordinary interface (the Wi-Fi one), and only fall back to
    // a point-to-point one — a cellular bearer — rather than refusing to serve at
    // all. The address is a hint anyway: a recipient that cannot reach it says so,
    // and the link stays valid.
    for (int pass = 0; pass < 2; ++pass) {
        const bool allowPointToPoint = (pass == 1);
        const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        for (int i = 0; i < interfaces.size(); ++i) {
            const QNetworkInterface &interface = interfaces.at(i);
            const QNetworkInterface::InterfaceFlags flags = interface.flags();
            if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
                || (flags & QNetworkInterface::IsLoopBack))
                continue;
            if (!allowPointToPoint && (flags & QNetworkInterface::IsPointToPoint))
                continue;

            const QList<QNetworkAddressEntry> entries = interface.addressEntries();
            for (int j = 0; j < entries.size(); ++j) {
                if (usable(entries.at(j).ip()))
                    return entries.at(j).ip();
            }
        }
    }
    return QHostAddress();
}

bool KelpServer::start(QString *error)
{
    if (m_server->isListening())
        return true;

    const QHostAddress address = lanHostAddress();
    if (address.isNull()) {
        if (error)
            *error = QStringLiteral("this device has no usable network address");
        return false;
    }

    if (!m_server->listen(address, 0)) {
        if (error)
            *error = QStringLiteral("cannot listen on %1: %2")
                .arg(address.toString(), m_server->errorString());
        return false;
    }

    m_host = address.toString();
    m_port = m_server->serverPort();
    m_sweeper->start();

    // A test affordance: a device behind NAT — the Sailfish emulator behind
    // QEMU's user-mode NAT — is reachable only through its host, so the host
    // names itself in the link instead (src/tests/run-emu-share.sh). Only the
    // address written into the link changes; the socket still binds the real
    // interface, and the link is still sealed by the app, which is the only
    // party that can seal it.
    const QByteArray advertised = qgetenv("KELP_ADVERTISE_HOST");
    if (!advertised.isEmpty())
        m_host = QString::fromUtf8(advertised);

    if (m_host != address.toString())
        qInfo() << "KelpServer: bound to"
                << (address.toString() + QLatin1Char(':') + QString::number(m_port));
    qInfo() << "KelpServer: serving on" << lanAddress();
    return true;
}

bool KelpServer::isListening() const
{
    return m_server->isListening();
}

QString KelpServer::lanAddress() const
{
    if (!m_server->isListening())
        return QString();
    return m_host + QLatin1Char(':') + QString::number(m_port);
}

void KelpServer::share(const QByteArray &bundleId, const QByteArray &swarmToken,
                       int objects, int files, quint64 bytes)
{
    Shared entry;
    entry.token = swarmToken;
    entry.objects = objects;
    entry.files = files;
    entry.bytes = bytes;
    m_shared.insert(bundleId, entry);
}

void KelpServer::unshare(const QByteArray &bundleId)
{
    m_shared.remove(bundleId);
}

void KelpServer::onConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();

        // Registered before the cap check so a refused socket is cleaned up too.
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_pending.remove(socket);
            socket->deleteLater();
        });

        if (m_pending.size() >= kMaxPendingConnections) {
            respond(socket, 503, "Service Unavailable", "text/plain; charset=utf-8",
                    QByteArrayLiteral("too many unfinished requests\n"));
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

void KelpServer::sweepPending()
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
                QByteArrayLiteral("waited too long for a request\n"));
    }
}

void KelpServer::readRequest(QTcpSocket *socket)
{
    QHash<QTcpSocket *, Pending>::iterator it = m_pending.find(socket);
    if (it == m_pending.end())     // swept or already answered
        return;
    Pending &pending = it.value();
    pending.buffer += socket->readAll();

    const int end = pending.buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (pending.buffer.size() > kMaxRequestBytes) {
            m_pending.remove(socket);
            respond(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                    QByteArrayLiteral("request headers too large\n"));
        }
        return;
    }

    const QByteArray request = pending.buffer.left(end);
    m_pending.remove(socket);
    handle(socket, request);
}

void KelpServer::handle(QTcpSocket *socket, const QByteArray &request)
{
    const QList<QByteArray> lines = request.split('\n');
    const QList<QByteArray> requestLine = lines.at(0).trimmed().split(' ');
    if (requestLine.size() < 2) {
        respond(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                QByteArrayLiteral("malformed request line\n"));
        return;
    }

    const QByteArray method = requestLine.at(0).trimmed();
    const QString path = QString::fromUtf8(requestLine.at(1).trimmed());

    QByteArray token;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name == QByteArrayLiteral("authorization")
            && value.toLower().startsWith(QByteArrayLiteral("bearer ")))
            token = value.mid(7).trimmed();
    }

    if (method != QByteArrayLiteral("GET")) {
        respond(socket, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                QByteArrayLiteral("only GET is served\n"));
        return;
    }

    const QStringList segments = path.split(QLatin1Char('/'), QString::SkipEmptyParts);
    if (segments.size() < 3 || segments.at(0) != QLatin1String("v1")
        || segments.at(1) != QLatin1String("bundle")) {
        respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                QByteArrayLiteral("no such endpoint\n"));
        return;
    }

    QByteArray bundleId;
    QString error;
    if (!Kelp::Link::base32Decode(segments.at(2), &bundleId, &error)) {
        respond(socket, 401, "Unauthorized", "text/plain; charset=utf-8",
                QByteArrayLiteral("bad token\n"));
        return;
    }

    // The header carries the base64url text of the token, so decode it and
    // compare bytes. Decoding also tolerates standard base64 and padding, which
    // is what a hand-typed curl command tends to produce.
    QByteArray presented;
    QString tokenError;
    if (!token.isEmpty()
        && !Kelp::Link::base64UrlDecode(QString::fromLatin1(token), &presented, &tokenError))
        presented.clear();

    // "No such bundle", "not shared any more" and "wrong token" all answer the
    // same way, so this cannot be used to enumerate what is being served.
    const bool known = m_shared.contains(bundleId);
    const QByteArray expected = known ? m_shared.value(bundleId).token : QByteArray();
    if (!known || !constantTimeEquals(presented, expected)) {
        respond(socket, 401, "Unauthorized", "text/plain; charset=utf-8",
                QByteArrayLiteral("bad token\n"));
        return;
    }

    const Shared shared = m_shared.value(bundleId);

    if (segments.size() == 4 && segments.at(3) == QLatin1String("info")) {
        const QString info = QStringLiteral("{\"v\":1,\"objects\":%1,\"files\":%2,\"bytes\":%3}\n")
            .arg(shared.objects).arg(shared.files)
            .arg(QString::number(shared.bytes, 'f', 0));
        respond(socket, 200, "OK", "application/json; charset=utf-8", info.toUtf8());
        return;
    }

    if (segments.size() == 5 && segments.at(3) == QLatin1String("object")) {
        QByteArray objectId;
        if (!Kelp::Link::base32Decode(segments.at(4), &objectId, &error)) {
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("no such object\n"));
            return;
        }
        QByteArray object;
        if (!m_store || !m_store->get(objectId, &object)) {
            respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
                    QByteArrayLiteral("no such object\n"));
            return;
        }
        respond(socket, 200, "OK", "application/octet-stream", object);
        return;
    }

    respond(socket, 404, "Not Found", "text/plain; charset=utf-8",
            QByteArrayLiteral("no such endpoint\n"));
}

void KelpServer::respond(QTcpSocket *socket, int status, const char *reason,
                         const char *contentType, const QByteArray &body)
{
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    head += "Content-Type: " + QByteArray(contentType) + "\r\n";
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "\r\n";

    socket->write(head);
    socket->write(body);
    socket->disconnectFromHost();
}
