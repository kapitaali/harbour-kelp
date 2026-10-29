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
 * The address is reserved, so it survives a restart of the VM; it is a literal
 * rather than a name so a recipient needs no working DNS to share. Publishing
 * through a relay that cannot be reached is not fatal: the link is still minted,
 * without `r=`, and works on the LAN.
 */
inline QStringList kelpDefaultRelays()
{
    const QString override = QString::fromLocal8Bit(qgetenv("KELP_RELAY")).trimmed();
    if (!override.isEmpty())
        return QStringList() << override;

    return QStringList() << QStringLiteral("82.70.43.26:9443");
}

#endif // KELPRELAY_H
