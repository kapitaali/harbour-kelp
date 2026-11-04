#ifndef KELPRELAY_H
#define KELPRELAY_H

#include <QString>
#include <QStringList>

/*
 * The relay every published share goes through.
 *
 * One relay, named once. The publisher dials out to it and every link it mints
 * carries it as `r=`, so a recipient on another network has a path to the
 * bundle without the sender configuring anything. A link also carries the
 * sender's LAN address; recipients prefer that and retry on the relay when it
 * cannot answer (KelpClient::retryOnRelay).
 *
 * It is a build-time constant rather than a Settings field on purpose: the
 * relay is a service the app provides, not a knob, and a mistyped address in a
 * field would silently break every share a user sends. KELP_RELAY overrides it,
 * which is how a test — or a second deployment — points at a different relay;
 * a release never needs it.
 *
 * The address is a name rather than the reserved literal: the browser gateway
 * is reached over HTTPS, and a certificate can only match a name. Recipients
 * fetching from the app also resolve it, which every internet path can do; the
 * LAN address in the same link needs no DNS. Publishing through a relay that
 * cannot be reached is not fatal: the link is still minted, without `r=`, and
 * works on the LAN.
 */
inline QStringList kelpDefaultRelays()
{
    const QString override = QString::fromLocal8Bit(qgetenv("KELP_RELAY")).trimmed();
    if (!override.isEmpty())
        return QStringList() << override;

    return QStringList() << QStringLiteral("kelp.kapitaali.com:9443");
}

#endif // KELPRELAY_H
