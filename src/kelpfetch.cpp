#include "kelpfetch.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelptls.h"

namespace {

// How long a first request may stay unanswered before the address is treated as
// unreachable. A reachable peer answers (or connects) well inside this, even on
// a slow network; an address that is dialled into but never heard from — the
// usual cause is a NAT with no way in — is otherwise indistinguishable from a
// peer that has gone quiet, and leaving it to KelpClient's silence deadline both
// takes too long and blames the wrong thing. Kept below that deadline on
// purpose, so "could not reach" wins the race for a peer that never answered.
const int kConnectMs = 8000;

// A relay is further away and does a TLS handshake, so it gets a little longer
// to show any sign of life.
const int kRelayConnectMs = 10000;

// What a status means to the person holding the link. The relay and the LAN
// answer the same codes for the same reasons (wire spec §7.2, §12.1).
QString statusText(int status)
{
    switch (status) {
    case 401:
        return QStringLiteral("the sender did not recognise this link — "
                              "it may have stopped sharing");
    case 404:
        return QStringLiteral("the sender no longer has part of this bundle");
    case 408:
        return QStringLiteral("the sender gave up waiting for our request");
    case 503:
        return QStringLiteral("the sender is busy with other peers");
    case 504:
        return QStringLiteral("the sender did not answer in time");
    default:
        return QStringLiteral("the sender answered %1").arg(status);
    }
}

// The address in a link goes straight into a URL, so it is checked hard (see
// KelpClient's LAN path for the same reasoning).
bool lanUrl(const QString &lan, QUrl *out)
{
    if (lan.isEmpty() || lan.size() > 255)
        return false;
    if (lan.contains(QLatin1Char('/')) || lan.contains(QLatin1Char('?'))
        || lan.contains(QLatin1Char('#')) || lan.contains(QLatin1Char('@'))
        || lan.contains(QLatin1Char(' ')) || lan.contains(QLatin1Char('\\')))
        return false;
    const QUrl url(QStringLiteral("http://") + lan);
    if (!url.isValid() || url.host().isEmpty() || !url.path().isEmpty()
        || !url.query().isEmpty() || !url.userInfo().isEmpty())
        return false;
    *out = url;
    return true;
}

} // namespace

// A relay is "host[:port]"; a port is required for a sane connection, so a bare
// host gets the default.
bool kelpRelayHostPort(const QString &relay, QString *host, quint16 *port)
{
    QString text = relay.trimmed();
    if (text.isEmpty() || text.size() > 255)
        return false;
    if (text.contains(QLatin1Char('/')) || text.contains(QLatin1Char('@'))
        || text.contains(QLatin1Char(' ')))
        return false;

    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        bool ok = false;
        const int value = text.mid(colon + 1).toInt(&ok);
        if (!ok || value <= 0 || value > 65535)
            return false;
        *host = text.left(colon);
        *port = quint16(value);
    } else {
        *host = text;
        *port = 9443;                       // the relay's default tunnel port
    }
    return !host->isEmpty();
}

// --- LAN-direct (wire spec §7.2) ----------------------------------------

LanSource::LanSource(const QUrl &base, const QByteArray &swarm, const QString &label,
                     const QString &bundleIdText, QObject *parent)
    : KelpSource(parent), m_base(base), m_label(label), m_swarm(swarm),
      m_bundleIdText(bundleIdText), m_reachability(new QTimer(this))
{
    m_network = new QNetworkAccessManager(this);
    m_reachability->setSingleShot(true);
    m_reachability->setInterval(kConnectMs);
    connect(m_reachability, &QTimer::timeout, this, &LanSource::onConnectTimeout);
}

void LanSource::setConnectTimeout(int ms)
{
    m_reachability->setInterval(ms);
}

// The one place that knows the header format: "Bearer " plus the base64url
// TEXT of the swarm token. The sender base64url-decodes the value and compares
// bytes (kelpserver.cpp), so this must never be given the raw token.
QNetworkRequest LanSource::infoRequest() const
{
    QNetworkRequest request(endpoint(QStringLiteral("info")));
    request.setRawHeader("Authorization",
                         "Bearer " + Kelp::Link::base64UrlEncode(m_swarm).toLatin1());
    return request;
}

QNetworkRequest LanSource::objectRequest(const QByteArray &id) const
{
    const QString idText = QString::fromLatin1(Kelp::Link::base32Encode(id));
    QNetworkRequest request(endpoint(QStringLiteral("object/") + idText));
    request.setRawHeader("Authorization",
                         "Bearer " + Kelp::Link::base64UrlEncode(m_swarm).toLatin1());
    return request;
}

void LanSource::begin()
{
    QNetworkReply *reply = m_network->get(infoRequest());
    m_replies.insert(reply);
    // The shape request doubles as a reachability probe: any sign of life —
    // response headers, or a finish carrying an error — means the address is
    // there. Nothing at all within the timeout means it is not.
    connect(reply, &QNetworkReply::metaDataChanged, this, &LanSource::noteReached);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onInfo(reply); });
    m_reachability->start();
}

void LanSource::noteReached()
{
    m_reached = true;
    m_reachability->stop();
}

void LanSource::onConnectTimeout()
{
    if (m_reached)
        return;
    m_unreachable = true;

    // Drop what is in flight without reporting it: the closure of an aborted
    // request is not a second failure.
    const QSet<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QSet<QNetworkReply *>::const_iterator it = replies.constBegin();
         it != replies.constEnd(); ++it) {
        (*it)->disconnect(this);
        (*it)->abort();
        (*it)->deleteLater();
    }
    emit failed(QStringLiteral("could not reach the sender — no answer from %1").arg(m_label));
}

void LanSource::requestObject(const QByteArray &id)
{
    QNetworkReply *reply = m_network->get(objectRequest(id));
    reply->setProperty("kelpObjectId", id);
    m_replies.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onObject(reply); });
}

void LanSource::stop()
{
    m_reachability->stop();
    const QSet<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QSet<QNetworkReply *>::const_iterator it = replies.constBegin();
         it != replies.constEnd(); ++it) {
        (*it)->disconnect(this);
        (*it)->abort();
        (*it)->deleteLater();
    }
}

QString LanSource::describe() const { return m_label; }

QUrl LanSource::endpoint(const QString &suffix) const
{
    QUrl url = m_base;
    url.setPath(QStringLiteral("/v1/bundle/%1/%2").arg(m_bundleIdText, suffix));
    return url;
}

bool LanSource::usable(QNetworkReply *reply, QString *error) const
{
    const QVariant code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = code.isValid() ? code.toInt() : 0;
    if (status == 0) {
        *error = QStringLiteral("could not reach the sender — %1").arg(reply->errorString());
        return false;
    }
    if (status != 200) {
        *error = statusText(status);
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        *error = QStringLiteral("the sender's answer was cut short — %1")
            .arg(reply->errorString());
        return false;
    }
    return true;
}

void LanSource::onInfo(QNetworkReply *reply)
{
    m_replies.remove(reply);
    reply->deleteLater();
    if (m_unreachable)
        return;                     // the probe already reported; this is its echo
    m_reached = true;
    m_reachability->stop();
    QString reason;
    if (!usable(reply, &reason)) {
        emit failed(reason);
        return;
    }
    const QJsonObject info = QJsonDocument::fromJson(reply->readAll()).object();
    emit infoReady(qMax(0, info.value(QStringLiteral("objects")).toInt()),
                   qMax(0, info.value(QStringLiteral("files")).toInt()),
                   quint64(info.value(QStringLiteral("bytes")).toDouble()));
}

void LanSource::onObject(QNetworkReply *reply)
{
    m_replies.remove(reply);
    reply->deleteLater();
    if (m_unreachable)
        return;
    m_reached = true;
    m_reachability->stop();
    QString reason;
    if (!usable(reply, &reason)) {
        emit failed(reason);
        return;
    }
    emit objectReady(reply->property("kelpObjectId").toByteArray(), reply->readAll());
}

// --- Relay (wire spec §12.1) --------------------------------------------

// HTTP inside a TLS 1.3 tunnel to the relay. The relay token rides in the path,
// never in a header, so the relay never sees the swarm token.
class RelaySource : public KelpSource
{
    Q_OBJECT
public:
    RelaySource(const QString &host, quint16 port, const QByteArray &token,
                const QString &bundleIdText, QObject *parent = nullptr)
        : KelpSource(parent), m_host(host), m_port(port),
          m_bundleIdText(bundleIdText), m_connected(false), m_failed(false),
          m_connect(new QTimer(this))
    {
        m_tokenText = Kelp::Link::base64UrlEncode(token);

        // A relay that accepts nothing, or accepts and stalls before TLS is up,
        // is unreachable, not "answering slowly": say so rather than waiting for
        // the fetch loop's silence deadline to call it a stopped sender.
        m_connect->setSingleShot(true);
        connect(m_connect, &QTimer::timeout, this, [this]() {
            if (!m_connected && !m_failed)
                fail(QStringLiteral("could not reach the relay — no answer from %1:%2")
                         .arg(m_host).arg(m_port));
        });
    }

    void begin() override
    {
        Request request;
        request.path = path(QStringLiteral("info"));
        request.info = true;
        m_queue.enqueue(request);
        open();
    }

    void requestObject(const QByteArray &id) override
    {
        Request request;
        request.path = path(QStringLiteral("object/")
                            + QString::fromLatin1(Kelp::Link::base32Encode(id)));
        request.info = false;
        request.id = id;
        m_queue.enqueue(request);
        if (m_connected)
            sendNext();
        else
            open();
    }

    void stop() override
    {
        m_connect->stop();
        m_queue.clear();
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
        m_connected = false;
    }

    QString describe() const override
    {
        return QStringLiteral("relay %1:%2").arg(m_host).arg(m_port);
    }

private:
    struct Request {
        QString path;
        QByteArray id;
        bool info;

        Request() : info(false) {}
    };

    void fail(const QString &message)
    {
        if (m_failed)
            return;
        m_failed = true;
        stop();
        emit failed(message);
    }

    QString path(const QString &suffix) const
    {
        return QStringLiteral("/v1/relay/%1/bundle/%2/%3")
            .arg(m_tokenText, m_bundleIdText, suffix);
    }

    void open()
    {
        if (m_failed || m_socket)
            return;
        m_socket = new QTcpSocket(this);
        m_tls = new KelpTls(this);
        connect(m_socket, &QTcpSocket::connected, this, [this]() {
            QString error;
            if (!m_tls->start(KelpTls::Client, m_socket, &error)) {
                fail(error);
            }
        });
        connect(m_tls, &KelpTls::encrypted, this, [this]() {
            m_connect->stop();              // talking to it: it is reachable
            m_connected = true;
            sendNext();
        });
        connect(m_tls, &KelpTls::readyRead, this, [this]() {
            m_buffer += m_tls->readAll();
            drain();
        });
        connect(m_tls, &KelpTls::error, this, [this](const QString &message) {
            fail(QStringLiteral("the relay connection failed — %1").arg(message));
        });
        connect(m_tls, &KelpTls::closed, this, [this]() {
            if (!m_failed && !m_queue.isEmpty())
                fail(QStringLiteral("the relay closed the connection"));
        });
        connect(m_socket,
                static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
                    &QAbstractSocket::error),
                this, [this]() {
            fail(QStringLiteral("could not reach the relay — %1").arg(m_socket->errorString()));
        });
        m_socket->connectToHost(m_host, m_port);
        m_connect->start(kRelayConnectMs);
    }

    void sendNext()
    {
        if (!m_connected || m_inFlight || m_queue.isEmpty())
            return;
        const Request request = m_queue.head();
        m_inFlight = true;
        m_tls->write(QStringLiteral("GET %1 HTTP/1.1\r\nHost: %2\r\n\r\n")
                         .arg(request.path, m_host).toUtf8());
    }

    // One response at a time; HTTP/1.1 + Content-Length lets the tunnel stay up.
    void drain()
    {
        for (;;) {
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
            if (length < 0) {
                fail(QStringLiteral("the relay sent a response without a length"));
                return;
            }
            if (m_buffer.size() < end + 4 + length)
                return;

            const QByteArray body = m_buffer.mid(end + 4, length);
            m_buffer.remove(0, end + 4 + length);

            if (m_queue.isEmpty()) {
                fail(QStringLiteral("the relay answered a request we did not make"));
                return;
            }
            const Request request = m_queue.dequeue();
            m_inFlight = false;

            if (status != 200) {
                fail(statusText(status));
                return;
            }
            if (request.info) {
                const QJsonObject info = QJsonDocument::fromJson(body).object();
                emit infoReady(qMax(0, info.value(QStringLiteral("objects")).toInt()),
                               qMax(0, info.value(QStringLiteral("files")).toInt()),
                               quint64(info.value(QStringLiteral("bytes")).toDouble()));
            } else {
                emit objectReady(request.id, body);
            }
            sendNext();
        }
    }

    QString m_host;
    quint16 m_port;
    QString m_bundleIdText;
    QString m_tokenText;
    QTcpSocket *m_socket = nullptr;
    KelpTls *m_tls = nullptr;
    QQueue<Request> m_queue;
    QByteArray m_buffer;
    bool m_connected;
    bool m_inFlight = false;
    bool m_failed;
    QTimer *m_connect = nullptr;
};

// --- construction --------------------------------------------------------

KelpSource *KelpSource::create(const Kelp::Link::Bundle &link, QObject *parent, QString *error)
{
    const QString bundleIdText = QString::fromLatin1(Kelp::Link::base32Encode(link.bundleId));

    QUrl base;
    if (lanUrl(link.lan, &base)) {
        const QByteArray swarm = Kelp::swarmToken(link.secret);
        if (swarm.size() != Kelp::Crypto::KeySize) {
            *error = QStringLiteral("could not derive a key from this link");
            return nullptr;
        }
        // Raw bytes: LanSource encodes them into the header itself, in the one
        // place that knows the format.
        return new LanSource(base, swarm, link.lan, bundleIdText, parent);
    }

    if (link.relays.isEmpty()) {
        *error = link.lan.isEmpty()
            ? QStringLiteral("this link names no address and no relay")
            : QStringLiteral("this link's address is not one to fetch from");
        return nullptr;
    }

    const QByteArray token = Kelp::relayToken(link.secret);
    if (token.size() != Kelp::Crypto::KeySize) {
        *error = QStringLiteral("could not derive a key from this link");
        return nullptr;
    }

    // Try the relays in the order the link lists them.
    for (int i = 0; i < link.relays.size(); ++i) {
        QString host;
        quint16 port = 0;
        if (!kelpRelayHostPort(link.relays.at(i), &host, &port)) {
            qWarning() << "KelpSource: unusable relay address" << link.relays.at(i);
            continue;
        }
        return new RelaySource(host, port, token, bundleIdText, parent);
    }

    *error = QStringLiteral("this link's relays are not addresses to fetch from");
    return nullptr;
}

#include "kelpfetch.moc"
