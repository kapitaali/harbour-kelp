#include "kelpchannelfetch.h"

#include <QTcpSocket>
#include <QTimer>

#include "kelp_link.h"
#include "kelpfetch.h"
#include "kelptls.h"

namespace {

// One relay gets this long to answer before it is passed over.
const int kRelayTimeoutMs = 10000;

// A sealed pointer is a few hundred bytes. Anything larger is not one.
const int kMaxResponseBytes = 1024 * 1024;

QVariantMap failure(const QString &error)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), error);
    return result;
}

} // namespace

KelpChannelFetch::KelpChannelFetch(QObject *parent)
    : QObject(parent), m_index(0), m_active(false), m_have(false),
      m_responseDone(false), m_socket(nullptr), m_tls(nullptr)
{
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        relayFailed(QStringLiteral("no answer from the relay"));
    });
}

QVariantMap KelpChannelFetch::resolveAsync(const QString &linkText)
{
    if (m_active)
        return failure(QStringLiteral("a channel is already being looked up"));

    QString error;
    if (!Kelp::Channel::decodeLink(linkText, &m_link, &error))
        return failure(error);
    if (m_link.relays.isEmpty())
        return failure(QStringLiteral("this channel link names no relay"));

    m_index = 0;
    m_have = false;
    m_responseDone = false;
    m_best = Kelp::Channel::Pointer();
    m_lastError.clear();
    m_active = true;
    nextRelay();

    QVariantMap started;
    started.insert(QStringLiteral("ok"), true);
    started.insert(QStringLiteral("error"), QString());
    return started;
}

void KelpChannelFetch::cancel()
{
    if (!m_active)
        return;
    dropConnection();
    m_active = false;
    emit resolved(failure(QStringLiteral("cancelled")));
}

void KelpChannelFetch::nextRelay()
{
    if (m_index >= m_link.relays.size()) {
        finish();
        return;
    }

    QString host;
    quint16 port = 0;
    if (!kelpRelayHostPort(m_link.relays.at(m_index), &host, &port)) {
        relayFailed(QStringLiteral("a relay in this link is not an address"));
        return;
    }

    m_responseDone = false;
    m_buffer.clear();
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &KelpChannelFetch::onConnected);
    connect(m_socket,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                &QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
                relayFailed(QStringLiteral("could not reach the relay"));
            });

    m_timeout->start(kRelayTimeoutMs);
    m_socket->connectToHost(host, port);
}

void KelpChannelFetch::onConnected()
{
    m_tls = new KelpTls(this);
    connect(m_tls, &KelpTls::encrypted, this, &KelpChannelFetch::onEncrypted);
    connect(m_tls, &KelpTls::readyRead, this, &KelpChannelFetch::onReadyRead);
    connect(m_tls, &KelpTls::error, this, [this](const QString &) { onTlsDone(); });
    connect(m_tls, &KelpTls::closed, this, &KelpChannelFetch::onTlsDone);

    QString error;
    if (!m_tls->start(KelpTls::Client, m_socket, &error))
        relayFailed(QStringLiteral("could not start TLS to the relay"));
}

void KelpChannelFetch::onEncrypted()
{
    // The pointer is addressed by the channel id: the path names nothing secret.
    const QString path = QStringLiteral("/v1/channel/%1")
        .arg(QString::fromLatin1(Kelp::Link::base32Encode(m_link.id)));
    m_tls->write(QStringLiteral("GET %1 HTTP/1.0\r\nHost: relay\r\n\r\n")
                     .arg(path).toUtf8());
}

void KelpChannelFetch::onReadyRead()
{
    m_buffer += m_tls->readAll();
    parseResponse();
}

void KelpChannelFetch::onTlsDone()
{
    // HTTP/1.0 ends with the close, so a complete answer may arrive here.
    if (m_responseDone)
        return;
    parseResponse();
    if (!m_responseDone)
        relayFailed(QStringLiteral("the relay closed the connection before answering"));
}

void KelpChannelFetch::parseResponse()
{
    if (m_responseDone)
        return;
    if (m_buffer.size() > kMaxResponseBytes) {
        relayFailed(QStringLiteral("the relay sent more than a pointer's worth"));
        return;
    }

    const int end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0)
        return;

    const QList<QByteArray> lines = m_buffer.left(end).split('\n');
    const QList<QByteArray> statusLine = lines.value(0).trimmed().split(' ');
    const int status = statusLine.size() >= 2 ? statusLine.at(1).toInt() : 0;

    int length = -1;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        if (line.left(colon).trimmed().toLower() == QByteArrayLiteral("content-length"))
            length = line.mid(colon + 1).trimmed().toInt();
    }

    const QByteArray body = m_buffer.mid(end + 4);
    if (length >= 0 && body.size() < length)
        return;                                   // body still in flight

    m_responseDone = true;
    m_timeout->stop();
    handleResponse(status, length >= 0 ? body.left(length) : body);
}

void KelpChannelFetch::handleResponse(int status, const QByteArray &body)
{
    if (status == 200) {
        Kelp::Channel::Pointer pointer;
        QString error;
        if (!Kelp::Channel::open(body, m_link.readSecret, m_link.id, &pointer, &error)) {
            // A pointer that does not open or verify is not this channel's, so
            // the relay is passed over rather than trusted.
            m_lastError = QStringLiteral("the relay's pointer did not verify: %1").arg(error);
        } else if (!m_have || Kelp::Channel::isNewer(pointer, m_best)) {
            m_best = pointer;
            m_have = true;
        }
    } else if (status == 404) {
        m_lastError = QStringLiteral("the relay has no such channel");
    } else {
        m_lastError = QStringLiteral("the relay answered %1").arg(status);
    }

    ++m_index;
    dropConnection();
    nextRelay();
}

void KelpChannelFetch::relayFailed(const QString &why)
{
    if (m_responseDone)
        return;
    m_lastError = why;
    ++m_index;
    dropConnection();
    nextRelay();
}

void KelpChannelFetch::dropConnection()
{
    m_timeout->stop();
    if (m_tls) {
        m_tls->disconnect(this);
        m_tls->close();
        m_tls->deleteLater();
        m_tls = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void KelpChannelFetch::finish()
{
    m_active = false;

    if (!m_have) {
        emit resolved(failure(m_lastError.isEmpty()
                                  ? QStringLiteral("no relay has this channel")
                                  : m_lastError));
        return;
    }

    Kelp::Link::Bundle bundle;
    bundle.bundleId = m_best.bundleId;
    bundle.secret = m_best.bundleSecret;
    bundle.relays = m_link.relays;
    bundle.name = m_best.name;

    QVariantMap result;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    result.insert(QStringLiteral("name"), m_best.name);
    result.insert(QStringLiteral("sequence"), m_best.sequence);
    result.insert(QStringLiteral("timestamp"), m_best.timestamp);
    result.insert(QStringLiteral("bundleId"),
                  QString::fromLatin1(Kelp::Link::base32Encode(m_best.bundleId)));
    result.insert(QStringLiteral("link"), Kelp::Link::encode(bundle));
    m_best = Kelp::Channel::Pointer();
    emit resolved(result);
}
