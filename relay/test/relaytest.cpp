// Throwaway-but-reusable verification harness for the relay (not part of the app
// or the relay). One process plays both roles so there is nothing to background:
//
//   publisher ──TLS tunnel──▶ relay ──TLS HTTP──▶ fetcher
//
// It registers a bundle, serves one object from memory, and then runs a handful
// of recipient requests, printing PASS/FAIL for each. Run it against a relay
// started with a self-signed cert.
#include <cstdio>

#include <QByteArray>
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>

#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_wire.h"
#include "kelptls.h"

using namespace Kelp;

namespace {

int g_failures = 0;

void report(const char *name, bool ok, const QString &detail = QString())
{
    printf("%s %s%s\n", ok ? "PASS" : "FAIL", name,
           detail.isEmpty() ? "" : qPrintable(QStringLiteral(" — ") + detail));
    fflush(stdout);
    if (!ok)
        ++g_failures;
}

// The one object the publisher will serve. Its id is its address.
QByteArray payload() { return QByteArrayLiteral("kelp relay test payload v1\n"); }

// A single HTTP request over TLS, reporting the status and body when complete.
class Fetch : public QObject
{
    Q_OBJECT
public:
    Fetch(quint16 port, const QString &path, QObject *parent = nullptr)
        : QObject(parent), m_port(port), m_path(path), m_status(0)
    {
        connect(&m_socket, &QTcpSocket::connected, this, [this]() {
            QString error;
            if (!m_tls.start(KelpTls::Client, &m_socket, &error)) {
                done(0, QByteArray());
            }
        });
        connect(&m_tls, &KelpTls::encrypted, this, [this]() {
            m_tls.write(QStringLiteral("GET %1 HTTP/1.0\r\nHost: relay\r\n\r\n")
                            .arg(m_path).toUtf8());
        });
        connect(&m_tls, &KelpTls::readyRead, this, [this]() {
            m_buffer += m_tls.readAll();
            parse();
        });
        connect(&m_tls, &KelpTls::error, this, [this](const QString &) {
            done(0, QByteArray());
        });
        connect(&m_tls, &KelpTls::closed, this, [this]() { parse(); });

        QTimer::singleShot(8000, this, [this]() { if (!m_done) done(0, QByteArray()); });
    }

    void go() { m_socket.connectToHost(QStringLiteral("127.0.0.1"), m_port); }

signals:
    void finished(int status, const QByteArray &body);

private:
    void done(int status, const QByteArray &body)
    {
        if (m_done)
            return;
        m_done = true;
        emit finished(status, body);
    }

    void parse()
    {
        if (m_done)
            return;
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0)
            return;
        const QList<QByteArray> lines = m_buffer.left(end).split('\n');
        const QList<QByteArray> statusLine = lines.value(0).trimmed().split(' ');
        m_status = statusLine.size() >= 2 ? statusLine.at(1).toInt() : 0;

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
            return;                                  // body still in flight
        done(m_status, body);
    }

    quint16 m_port;
    QString m_path;
    QTcpSocket m_socket;
    KelpTls m_tls;
    QByteArray m_buffer;
    int m_status;
    bool m_done = false;
};

class Publisher : public QObject
{
    Q_OBJECT
public:
    Publisher(quint16 port, const QByteArray &bundleId, const QByteArray &token,
              QObject *parent = nullptr)
        : QObject(parent), m_port(port), m_bundleId(bundleId), m_token(token)
    {
        connect(&m_socket, &QTcpSocket::connected, this, [this]() {
            QString error;
            if (!m_tls.start(KelpTls::Client, &m_socket, &error))
                printf("publisher: start failed: %s\n", qPrintable(error));
        });
        connect(&m_tls, &KelpTls::encrypted, this, [this]() {
            Wire::RegisterMessage registration;
            registration.bundleId = m_bundleId;
            registration.token = m_token;
            registration.objects = 22;
            registration.files = 6;
            registration.bytes = 1687667;
            m_tls.write(Wire::encodeFrame(Wire::Register,
                                          Wire::encodeRegister(registration)));
            printf("publisher: registered\n");
            fflush(stdout);
        });
        connect(&m_tls, &KelpTls::readyRead, this, [this]() {
            m_reader.push(m_tls.readAll());
            quint8 type = 0;
            QByteArray frame;
            while (m_reader.next(&type, &frame))
                onFrame(type, frame);
        });
        connect(&m_tls, &KelpTls::error, this, [](const QString &message) {
            printf("publisher: tls error: %s\n", qPrintable(message));
        });
    }

    void go() { m_socket.connectToHost(QStringLiteral("127.0.0.1"), m_port); }

private:
    void onFrame(quint8 type, const QByteArray &frame)
    {
        if (type == Wire::Want) {
            Wire::WantMessage want;
            QString error;
            if (!Wire::decodeWant(frame, &want, &error))
                return;
            const QByteArray expected = Crypto::sha256(payload());
            if (want.hash == expected) {
                Wire::DataMessage data;
                data.index = 3;
                data.hash = want.hash;
                data.bytes = payload();
                m_tls.write(Wire::encodeFrame(Wire::Data, Wire::encodeData(data)));
                printf("publisher: served %s\n", want.hash.toHex().left(12).constData());
            } else {
                Wire::RejectMessage reject;
                reject.hash = want.hash;
                reject.why = Wire::NotFound;
                m_tls.write(Wire::encodeFrame(Wire::Reject, Wire::encodeReject(reject)));
                printf("publisher: rejected %s\n", want.hash.toHex().left(12).constData());
            }
            fflush(stdout);
        } else if (type == Wire::Ping) {
            quint64 t = 0;
            QString error;
            if (Wire::decodePing(frame, &t, &error))
                m_tls.write(Wire::encodeFrame(Wire::Pong, Wire::encodePing(t)));
        }
    }

    quint16 m_port;
    QByteArray m_bundleId;
    QByteArray m_token;
    QTcpSocket m_socket;
    KelpTls m_tls;
    Wire::FrameReader m_reader;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // One port serves both roles (relay.h); the harness uses it for both.
    const quint16 port = quint16(QByteArray(argv[1]).toUShort());

    const QByteArray bundleId = Crypto::sha256(QByteArrayLiteral("bundle"));
    const QByteArray token = Crypto::sha256(QByteArrayLiteral("token"));
    const QByteArray objectId = Crypto::sha256(payload());

    const QByteArray path = QByteArrayLiteral("/v1/relay/")
        + Link::base64UrlEncode(token).toLatin1() + QByteArrayLiteral("/bundle/")
        + Link::base32Encode(bundleId) + QByteArrayLiteral("/");

    Publisher publisher(port, bundleId, token);
    publisher.go();

    // Let the tunnel register before the first recipient request.
    QTimer::singleShot(500, &app, [&]() {
        Fetch *info = new Fetch(port, QString::fromLatin1(path + "info"), &app);
        QObject::connect(info, &Fetch::finished, &app, [&](int status, const QByteArray &body) {
            const bool ok = status == 200 && body.contains("\"objects\":22")
                && body.contains("\"files\":6") && body.contains("\"bytes\":1687667");
            report("/info via relay", ok, QString::fromLatin1(body).trimmed());

            // The object the publisher has.
            Fetch *good = new Fetch(port,
                                    QString::fromLatin1(path + "object/")
                                        + QString::fromLatin1(Link::base32Encode(objectId)), &app);
            QObject::connect(good, &Fetch::finished, &app, [&](int status, const QByteArray &body) {
                report("fetch object via relay", status == 200 && body == payload(),
                       QStringLiteral("status=%1 bytes=%2").arg(status).arg(body.size()));

                // An object the publisher does not have -> REJECT -> 404.
                Fetch *missing = new Fetch(port,
                    QString::fromLatin1(path + "object/")
                        + QString::fromLatin1(Link::base32Encode(Crypto::sha256("nope"))), &app);
                QObject::connect(missing, &Fetch::finished, &app, [&](int s, const QByteArray &) {
                    report("missing object -> 404", s == 404, QStringLiteral("status=%1").arg(s));

                    // A wrong relay token -> 401 (same as an unknown bundle).
                    const QByteArray badPath = QByteArrayLiteral("/v1/relay/")
                        + Link::base64UrlEncode(Crypto::sha256("wrong")).toLatin1()
                        + QByteArrayLiteral("/bundle/") + Link::base32Encode(bundleId)
                        + QByteArrayLiteral("/info");
                    Fetch *bad = new Fetch(port, QString::fromLatin1(badPath), &app);
                    QObject::connect(bad, &Fetch::finished, &app, [&](int s2, const QByteArray &) {
                        report("wrong token -> 401", s2 == 401,
                               QStringLiteral("status=%1").arg(s2));
                        printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
                        app.exit(g_failures == 0 ? 0 : 1);
                    });
                    bad->go();
                });
                missing->go();
            });
            good->go();
        });
        info->go();
    });

    return app.exec();
}

#include "relaytest.moc"
