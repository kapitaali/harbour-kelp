#include "kelphandoff.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLocalSocket>
#include <QTimer>

#include "kelplog.h"

namespace {

// A real link is a few hundred characters; this is a ceiling, not a target. It
// bounds the read buffer, so a client that never sends a newline cannot make the
// instance grow.
const int kMaxLinkBytes = 4096;

// The forwarder lives in a world-readable argv (see the header), so it should not
// linger: these are the longest waits before it gives up or hands over.
const int kConnectMs = 500;
const int kWriteMs = 500;
const int kAckMs = 1500;

// A client that connects and says nothing must not hold the instance's attention.
const int kIdleMs = 2000;

} // namespace

KelpHandoff::KelpHandoff(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QLocalServer::newConnection, this, &KelpHandoff::onConnection);
}

QString KelpHandoff::socketPath()
{
    return KelpLog::dataDirectory() + QStringLiteral("/link.sock");
}

QString KelpHandoff::linkFromArguments(const QStringList &arguments)
{
    // Index 0 is the program. A scheme activation puts the URL in the next one;
    // scanning the rest costs nothing and survives a launcher that adds a flag.
    for (int i = 1; i < arguments.size(); ++i) {
        if (acceptable(arguments.at(i)))
            return arguments.at(i);
    }
    return QString();
}

bool KelpHandoff::acceptable(const QString &link)
{
    if (link.isEmpty() || link.size() > kMaxLinkBytes)
        return false;
    if (!link.startsWith(QStringLiteral("kelp://"), Qt::CaseInsensitive))
        return false;

    // One line, and no whitespace: the wire is newline-terminated, so a control
    // character could smuggle a second message, and a real link never has either.
    for (int i = 0; i < link.size(); ++i) {
        const QChar c = link.at(i);
        if (c.unicode() < 0x20 || c.unicode() == 0x7f || c.isSpace())
            return false;
    }
    return true;
}

KelpHandoff::Result KelpHandoff::forward(const QString &link, QString *error)
{
    if (!acceptable(link)) {
        if (error)
            *error = QStringLiteral("not a kelp link");
        return Refused;
    }

    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(kConnectMs)) {
        if (error)
            *error = socket.errorString();
        // A timeout is the one failure that says "something is there": a live but
        // stuck instance must not be replaced. Everything else — no file, a stale
        // file, a refused connection — means nobody is serving.
        return socket.error() == QLocalSocket::SocketTimeoutError ? Unresponsive : NoServer;
    }

    socket.write(link.toUtf8() + '\n');
    if (!socket.waitForBytesWritten(kWriteMs)) {
        if (error)
            *error = socket.errorString();
        return Unresponsive;
    }

    // Wait for "ok"/"no" without blocking the event loop: in a test the server is
    // in this same thread, and in the app a nested loop before app.exec() is
    // harmless.
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&socket, &QLocalSocket::readyRead, &loop, &QEventLoop::quit);
    connect(&socket, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);

    QByteArray answer;
    deadline.start(kAckMs);
    while (deadline.isActive()) {
        answer += socket.readAll();
        if (answer.contains('\n') || socket.state() != QLocalSocket::ConnectedState)
            break;
        loop.exec();
    }
    answer += socket.readAll();

    const QByteArray text = answer.trimmed();
    if (text == QByteArrayLiteral("ok"))
        return Sent;
    if (text == QByteArrayLiteral("no"))
        return Refused;
    if (error)
        *error = QStringLiteral("the running instance did not answer");
    return Unresponsive;
}

bool KelpHandoff::instanceRunning() const
{
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (socket.waitForConnected(kConnectMs))
        return true;
    return socket.error() == QLocalSocket::SocketTimeoutError;
}

bool KelpHandoff::listen(bool replaceStale, QString *error)
{
    QDir().mkpath(KelpLog::dataDirectory());

    if (!m_server.listen(socketPath())) {
        // Only a caller that has established nobody is listening may clear the
        // path; a live instance's socket is never stolen. `replaceStale` is the
        // caller's earlier finding, so it is checked again here: between that
        // check and now another process may have become the instance (two cold
        // launches racing), and its fresh socket must not be removed.
        if (!replaceStale || instanceRunning()) {
            if (error) {
                *error = replaceStale
                        ? QStringLiteral("another instance took the socket first")
                        : m_server.errorString();
            }
            return false;
        }
        QLocalServer::removeServer(socketPath());
        if (!m_server.listen(socketPath())) {
            if (error)
                *error = m_server.errorString();
            return false;
        }
    }

    // The socket is a door into this app and the link it carries is a key, so the
    // file is owner-only. The data directory is already the app's own; the mode
    // is what a connect checks.
    QFile::setPermissions(socketPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void KelpHandoff::onConnection()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { readFrom(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(kIdleMs, socket, [socket] { socket->abort(); });
        if (socket->bytesAvailable() > 0)
            readFrom(socket);
    }
}

void KelpHandoff::readFrom(QLocalSocket *socket)
{
    if (socket->property("kelpFinished").toBool())
        return;

    QByteArray buffer = socket->property("kelpBuffer").toByteArray();
    buffer += socket->readAll();
    socket->setProperty("kelpBuffer", buffer);

    const int newline = buffer.indexOf('\n');
    if (newline < 0) {
        // No terminator yet. Past the cap it is not a link, whatever it is.
        if (buffer.size() > kMaxLinkBytes + 2)
            finish(socket, QByteArrayLiteral("no"));
        return;
    }

    QByteArray line = buffer.left(newline);
    if (line.endsWith('\r'))
        line.chop(1);

    const QString text = QString::fromUtf8(line);
    if (!acceptable(text)) {
        finish(socket, QByteArrayLiteral("no"));
        return;
    }

    finish(socket, QByteArrayLiteral("ok"));
    // After the ack, so the forwarder's process can exit while this page is still
    // being put together.
    emit linkArrived(text);
}

void KelpHandoff::finish(QLocalSocket *socket, const QByteArray &answer)
{
    socket->setProperty("kelpFinished", true);
    socket->write(answer + '\n');
    socket->disconnectFromServer();     // flushes what was written, then closes
}
