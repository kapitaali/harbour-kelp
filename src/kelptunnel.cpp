#include "kelptunnel.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QTcpSocket>
#include <QTimer>

#include "kelpfetch.h"
#include "kelp_crypto.h"
#include "kelptls.h"
#include "kelpstore.h"

namespace {

// How often to nudge the relay when nothing else is happening, so an idle
// tunnel is not mistaken for a dead one by a middlebox.
const int kKeepaliveMs = 30000;

// Reconnect backoff, capped: 1s, 2s, 4s … 30s.
const int kRetryBaseMs = 1000;
const int kRetryMaxMs = 30000;

} // namespace

KelpTunnel::KelpTunnel(KelpObjectStore *store, QObject *parent)
    : QObject(parent),
      m_store(store),
      m_index(0),
      m_objects(0),
      m_files(0),
      m_bytes(0),
      m_wanted(false),
      m_live(false),
      m_attempt(0),
      m_socket(nullptr),
      m_tls(nullptr),
      m_keepalive(new QTimer(this)),
      m_retry(new QTimer(this))
{
    m_keepalive->setInterval(kKeepaliveMs);
    connect(m_keepalive, &QTimer::timeout, this, &KelpTunnel::onKeepalive);

    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &KelpTunnel::onRetry);
}

KelpTunnel::~KelpTunnel()
{
    stop();
}

bool KelpTunnel::start(const QStringList &relays, const QByteArray &bundleId,
                       const QByteArray &relayToken, int objects, int files, quint64 bytes,
                       QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    if (relays.isEmpty()) {
        *error = QStringLiteral("no relay to publish on");
        return false;
    }
    if (bundleId.size() != Kelp::Crypto::Sha256Size
        || relayToken.size() != Kelp::Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id and relay token must be 32 bytes");
        return false;
    }

    m_relays = relays;
    m_bundleId = bundleId;
    m_token = relayToken;
    m_objects = objects;
    m_files = files;
    m_bytes = bytes;
    m_index = 0;
    m_attempt = 0;
    m_wanted = true;

    connectToRelay(m_index);
    return true;
}

void KelpTunnel::stop()
{
    m_wanted = false;
    m_keepalive->stop();
    m_retry->stop();
    teardown();
    emit changed();
}

void KelpTunnel::teardown()
{
    m_keepalive->stop();
    m_live = false;
    m_current.clear();
    m_reader = Kelp::Wire::FrameReader();

    if (m_tls) {
        m_tls->disconnect(this);
        m_tls->close();
        m_tls->deleteLater();
        m_tls = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void KelpTunnel::connectToRelay(int index)
{
    if (!m_wanted || m_relays.isEmpty())
        return;

    QString host;
    quint16 port = 0;
    if (!kelpRelayHostPort(m_relays.at(index), &host, &port)) {
        qWarning() << "KelpTunnel: unusable relay address" << m_relays.at(index);
        m_index = (index + 1) % m_relays.size();
        scheduleRetry();
        return;
    }

    teardown();
    m_socket = new QTcpSocket(this);
    m_tls = new KelpTls(this);

    connect(m_socket, &QTcpSocket::connected, this, &KelpTunnel::onConnected);
    connect(m_tls, &KelpTls::encrypted, this, &KelpTunnel::onEncrypted);
    connect(m_tls, &KelpTls::readyRead, this, &KelpTunnel::onReadyRead);
    connect(m_tls, &KelpTls::error, this, [this](const QString &message) {
        qWarning() << "KelpTunnel: tunnel error:" << message;
        onClosed();
    });
    connect(m_tls, &KelpTls::closed, this, &KelpTunnel::onClosed);
    connect(m_socket,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                &QAbstractSocket::error),
            this, &KelpTunnel::onSocketError);

    qInfo() << "KelpTunnel: dialling relay" << host << port;
    m_socket->connectToHost(host, port);
}

void KelpTunnel::onConnected()
{
    QString error;
    if (!m_tls->start(KelpTls::Client, m_socket, &error)) {
        qWarning() << "KelpTunnel: TLS setup failed:" << error;
        onClosed();
    }
}

void KelpTunnel::onEncrypted()
{
    m_live = true;
    m_attempt = 0;
    m_current = m_relays.at(m_index);
    sendRegister();
    m_keepalive->start();
    qInfo() << "KelpTunnel: published on" << m_current;
    emit changed();
}

void KelpTunnel::sendRegister()
{
    Kelp::Wire::RegisterMessage registration;
    registration.bundleId = m_bundleId;
    registration.token = m_token;
    registration.objects = quint64(m_objects);
    registration.files = quint64(m_files);
    registration.bytes = m_bytes;
    m_tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Register,
                                         Kelp::Wire::encodeRegister(registration)));
}

void KelpTunnel::onReadyRead()
{
    m_reader.push(m_tls->readAll());
    quint8 type = 0;
    QByteArray payload;
    while (m_reader.next(&type, &payload))
        handleFrame(type, payload);
    if (m_reader.failed()) {
        qWarning() << "KelpTunnel: frame error:" << m_reader.error();
        onClosed();
    }
}

void KelpTunnel::handleFrame(quint8 type, const QByteArray &payload)
{
    QString error;
    if (type == Kelp::Wire::Want) {
        Kelp::Wire::WantMessage want;
        if (!Kelp::Wire::decodeWant(payload, &want, &error)) {
            qWarning() << "KelpTunnel: bad WANT:" << error;
            return;
        }
        serve(want.hash);
        return;
    }
    if (type == Kelp::Wire::Ping) {
        quint64 t = 0;
        if (Kelp::Wire::decodePing(payload, &t, &error))
            m_tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Pong,
                                                 Kelp::Wire::encodePing(t)));
        return;
    }
    if (type == Kelp::Wire::Pong) {
        return;                       // our keepalive came back: the tunnel is alive
    }
    if (type == Kelp::Wire::Error) {
        Kelp::Wire::ErrorMessage message;
        if (Kelp::Wire::decodeError(payload, &message, &error))
            qWarning() << "KelpTunnel: relay error" << message.code << message.message;
        return;
    }
    qWarning() << "KelpTunnel: unexpected frame type" << int(type);
}

void KelpTunnel::serve(const QByteArray &objectId)
{
    QByteArray object;
    if (m_store && m_store->get(objectId, &object)) {
        Kelp::Wire::DataMessage data;
        // The canonical index is only used for swarm availability reporting,
        // which does not exist yet; the id is what identifies the object.
        data.index = 0;
        data.hash = objectId;
        data.bytes = object;
        m_tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Data,
                                             Kelp::Wire::encodeData(data)));
        return;
    }

    Kelp::Wire::RejectMessage reject;
    reject.hash = objectId;
    reject.why = Kelp::Wire::NotFound;
    m_tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Reject,
                                         Kelp::Wire::encodeReject(reject)));
}

void KelpTunnel::onSocketError()
{
    if (!m_socket)
        return;
    qWarning() << "KelpTunnel: connection failed:" << m_socket->errorString();
    onClosed();
}

void KelpTunnel::onClosed()
{
    if (!m_wanted)
        return;
    teardown();
    // Rotate to the next relay: if one is down, another may not be.
    if (!m_relays.isEmpty())
        m_index = (m_index + 1) % m_relays.size();
    emit changed();
    scheduleRetry();
}

void KelpTunnel::scheduleRetry()
{
    if (!m_wanted)
        return;
    ++m_attempt;
    int delay = kRetryBaseMs;
    for (int i = 1; i < m_attempt && delay < kRetryMaxMs; ++i)
        delay *= 2;
    if (delay > kRetryMaxMs)
        delay = kRetryMaxMs;
    qInfo() << "KelpTunnel: retrying in" << delay << "ms";
    m_retry->start(delay);
}

void KelpTunnel::onRetry()
{
    connectToRelay(m_index);
}

void KelpTunnel::onKeepalive()
{
    if (!m_live || !m_tls)
        return;
    m_tls->write(Kelp::Wire::encodeFrame(Kelp::Wire::Ping,
                                         Kelp::Wire::encodePing(quint64(0))));
}
