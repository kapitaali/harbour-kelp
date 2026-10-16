// Throwaway-but-reusable verification harness for the relay (not part of the app
// or the relay). One process plays both roles so there is nothing to background:
//
//   publisher ──TLS tunnel──▶ relay ──TLS HTTP──▶ fetcher
//
// The publisher half is the app's own KelpTunnel, not a re-implementation, so
// this checks the wire code the device really runs: it registers a bundle from a
// real object store, serves it, and the harness then checks the relay's answers
// and the tunnel's own account of what it served. Run it against a relay started
// with a self-signed cert.
#include <cstdio>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>

#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_wire.h"
#include "kelpstore.h"
#include "kelptls.h"
#include "kelptunnel.h"

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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // One port serves both roles (relay.h); the harness uses it for both.
    const quint16 port = quint16(QByteArray(argv[1]).toUShort());

    // A throwaway store holding the one object the tunnel is to serve, so it reads
    // the object back from disk exactly as it would on a device.
    const QString base = QDir::tempPath() + QStringLiteral("/kelp-relay-test");
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    KelpObjectStore store(base + QStringLiteral("/objects"));
    QString error;
    if (!store.initialise(&error)) {
        printf("cannot open the object store: %s\n", qPrintable(error));
        return 1;
    }

    const QByteArray objectId = Crypto::sha256(payload());
    QByteArray storedId;
    if (!store.put(payload(), &storedId) || storedId != objectId) {
        printf("cannot store the payload\n");
        return 1;
    }

    const QByteArray bundleId = Crypto::sha256(QByteArrayLiteral("bundle"));
    const QByteArray token = Crypto::sha256(QByteArrayLiteral("token"));

    const QByteArray path = QByteArrayLiteral("/v1/relay/")
        + Link::base64UrlEncode(token).toLatin1() + QByteArrayLiteral("/bundle/")
        + Link::base32Encode(bundleId) + QByteArrayLiteral("/");

    // The app's own tunnel, doing the app's own registering and serving.
    KelpTunnel tunnel(&store);
    const QStringList relays =
        QStringList() << QStringLiteral("127.0.0.1:%1").arg(port);
    if (!tunnel.start(relays, bundleId, token, QStringLiteral("relay test"), 22, 6,
                      1687667, &error)) {
        printf("cannot start the tunnel: %s\n", qPrintable(error));
        return 1;
    }

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

                        // What the tunnel says it did: two objects were wanted (one
                        // held here, one not) and one left the device. The wrong
                        // token never became a WANT, so it is not counted.
                        const KelpTunnel::Traffic tally = tunnel.traffic();
                        report("tunnel tallied the transfer",
                               tally.wanted == 2 && tally.objects == 1
                                   && tally.bytes == payload().size() && tally.rejected == 1,
                               QStringLiteral("wanted=%1 served=%2 bytes=%3 notHeld=%4")
                                   .arg(tally.wanted).arg(tally.objects).arg(tally.bytes)
                                   .arg(tally.rejected));

                        // And it reports itself once the asking stops — that line is
                        // what lands in the publisher's log (§3.15). Reporting resets
                        // the tally, so an empty tally is the proof that it ran.
                        QTimer::singleShot(7000, &app, [&]() {
                            const KelpTunnel::Traffic after = tunnel.traffic();
                            report("tunnel reported itself and reset",
                                   after.wanted == 0 && after.objects == 0,
                                   QStringLiteral("wanted=%1 served=%2")
                                       .arg(after.wanted).arg(after.objects));
                            printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
                            tunnel.stop();
                            app.exit(g_failures == 0 ? 0 : 1);
                        });
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
