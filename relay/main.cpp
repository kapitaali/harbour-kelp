/*
 * kelp-relay — the publisher's outbound-tunnel endpoint (wire spec §12.1).
 *
 * Not part of the phone app: it is the server side of C3, the thing a publisher
 * dials out to so a recipient behind another network can fetch. Run it anywhere
 * reachable — a VPS, a home box, another machine on the LAN.
 *
 *   kelp-relay --port 9443 --cert cert.pem --key key.pem
 *
 * One port, TLS 1.3 with ALPN "kelp/1", serving both publisher tunnels and
 * recipient fetches (§12.1). The certificate is self-signed and unverified by
 * peers (content is hash-verified); it exists so the tunnel is encrypted, not to
 * establish identity.
 */

#include <cstdio>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QStringList>

#include "relay.h"

namespace {

bool readFile(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *out = file.readAll();
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    quint16 port = 9443;
    QString certificatePath;
    QString keyPath;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        const QString value = i + 1 < args.size() ? args.at(i + 1) : QString();
        if (arg == QLatin1String("--port") && !value.isEmpty()) {
            port = quint16(value.toUShort());
            ++i;
        } else if (arg == QLatin1String("--cert") && !value.isEmpty()) {
            certificatePath = value;
            ++i;
        } else if (arg == QLatin1String("--key") && !value.isEmpty()) {
            keyPath = value;
            ++i;
        }
    }

    QByteArray certificate;
    QByteArray key;
    if (certificatePath.isEmpty() || keyPath.isEmpty()
        || !readFile(certificatePath, &certificate) || !readFile(keyPath, &key)) {
        fprintf(stderr, "usage: %s --port <port> --cert <pem> --key <pem>\n",
                qPrintable(app.arguments().value(0)));
        return 2;
    }

    Relay relay;
    QString error;
    if (!relay.start(port, certificate, key, &error)) {
        fprintf(stderr, "kelp-relay: %s\n", qPrintable(error));
        return 1;
    }

    // Printed so a test harness (or a human) can read the port even when 0 was
    // asked for and the OS chose one.
    printf("KELP_RELAY_READY port=%u\n", unsigned(relay.port()));
    fflush(stdout);

    return app.exec();
}
