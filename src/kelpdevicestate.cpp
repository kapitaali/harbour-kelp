#include "kelpdevicestate.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QVector>
#include <QDebug>

using Kelp::Policy::InterfaceKind;

namespace {

QString readText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(file.readAll()).trimmed();
}

// A test affordance (META-INF/BUILD.md, and what src/tests/run-emu-share.sh
// uses): the emulator's only connection is ethernet and it has no battery at
// all, so the two situations this policy exists for cannot be reached there.
// These two variables make them reachable on a device that has neither, and
// they can only ever make the app more conservative — neither can make it serve
// something it would not otherwise have served, which is why they are not
// behind the dev-extras bundle.
//
//   KELP_FORCE_METERED=1        the connection counts as mobile data
//   KELP_FORCE_BATTERY=<0..100> the battery reads that, discharging
bool forcedMetered()
{
    return qgetenv("KELP_FORCE_METERED") == QByteArrayLiteral("1");
}

Kelp::Policy::Power forcedBattery()
{
    const QByteArray text = qgetenv("KELP_FORCE_BATTERY");
    if (text.isEmpty())
        return Kelp::Policy::Power();

    bool ok = false;
    const int percent = text.toInt(&ok);
    if (!ok)
        return Kelp::Policy::Power();
    return Kelp::Policy::Power(true, false, percent);
}

// An interface is usable when it really carries an address a packet could be
// sent to: no loopback and no link-local (169.254/16, fe80::/10). A Wi-Fi
// interface with the radio off is up in the kernel but has neither.
bool hasUsableAddress(const QNetworkInterface &interface)
{
    const QList<QNetworkAddressEntry> entries = interface.addressEntries();
    for (const QNetworkAddressEntry &entry : entries) {
        const QHostAddress address = entry.ip();
        if (address.isNull() || address.isLoopback())
            continue;
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            if (address.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16))
                continue;
            return true;
        }
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            if (address.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10))
                continue;
            return true;
        }
    }
    return false;
}

QString kindName(InterfaceKind kind)
{
    switch (kind) {
    case InterfaceKind::Wifi: return QStringLiteral("wifi");
    case InterfaceKind::Cellular: return QStringLiteral("cellular");
    case InterfaceKind::Other: break;
    }
    return QStringLiteral("other");
}

// The kernel names one supply of several "Battery" (a C2 lists a dozen charger
// nodes and one battery); its status word and capacity are the two facts.
Kelp::Policy::Power readBattery()
{
    const QString root = QStringLiteral("/sys/class/power_supply");
    const QStringList supplies = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &supply : supplies) {
        const QString directory = root + QLatin1Char('/') + supply;
        if (readText(directory + QStringLiteral("/type")) != QLatin1String("Battery"))
            continue;

        const QString capacity = readText(directory + QStringLiteral("/capacity"));
        bool ok = false;
        const int percent = capacity.toInt(&ok);
        const Kelp::Policy::Power power =
            Kelp::Policy::powerFromStatus(readText(directory + QStringLiteral("/status")),
                                          ok ? percent : -1);
        if (power.known)
            return power;

        // A battery node that cannot be read is a machine that cannot tell us;
        // say so rather than reporting zero percent.
        return Kelp::Policy::Power();
    }

    return Kelp::Policy::Power();
}

} // namespace

KelpDeviceState::KelpDeviceState(QObject *parent)
    : QObject(parent),
      m_online(false),
      m_metered(false),
      m_batteryKnown(false),
      m_charging(false),
      m_batteryPercent(-1)
{
    readNetwork();
    readPower();

    m_timer.setInterval(kPollMs);
    connect(&m_timer, &QTimer::timeout, this, &KelpDeviceState::refresh);
    m_timer.start();
}

void KelpDeviceState::refresh()
{
    readNetwork();
    readPower();
}

void KelpDeviceState::readNetwork()
{
    QVector<InterfaceKind> usable;
    QString firstName;
    InterfaceKind firstKind = InterfaceKind::Other;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = interface.flags();
        if (flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        if (!flags.testFlag(QNetworkInterface::IsUp))
            continue;
        if (!hasUsableAddress(interface))
            continue;

        const bool wireless = QFile::exists(QStringLiteral("/sys/class/net/%1/wireless")
                                                .arg(interface.name()));
        const InterfaceKind kind = Kelp::Policy::classifyInterface(interface.name(), wireless);
        usable.append(kind);
        if (firstName.isEmpty()) {
            firstName = interface.name();
            firstKind = kind;
        }
    }

    const bool online = !usable.isEmpty();
    bool metered = Kelp::Policy::metered(usable);
    QString name = firstName;
    QString kind = online ? kindName(firstKind) : QString();

    const bool forced = forcedMetered();
    if (forced) {
        metered = true;
        name = QStringLiteral("forced0");
        kind = QStringLiteral("cellular");
    }

    if (online == m_online && metered == m_metered
            && name == m_interfaceName && kind == m_interfaceKind) {
        return;
    }

    // Logged at every change: a metered verdict is the one thing here that can
    // be wrong (the cellular names are a heuristic), and the interface name is
    // what makes it checkable from a device read over ssh.
    QString line = QStringLiteral("connection %1 - serving interface %2")
                       .arg(online ? QStringLiteral("up") : QStringLiteral("none"),
                            name.isEmpty() ? QStringLiteral("-") : name);
    if (!name.isEmpty())
        line += QStringLiteral(" (%1)").arg(kind);
    line += QStringLiteral(" - metered %1").arg(metered ? QStringLiteral("yes")
                                                        : QStringLiteral("no"));
    if (forced)
        line += QStringLiteral(" (forced by KELP_FORCE_METERED)");
    qInfo() << "KelpDeviceState:" << line;

    m_online = online;
    m_metered = metered;
    m_interfaceName = name;
    m_interfaceKind = kind;
    emit changed();
}

void KelpDeviceState::readPower()
{
    const Kelp::Policy::Power forced = forcedBattery();
    const Kelp::Policy::Power power = forced.known ? forced : readBattery();

    if (power.known == m_batteryKnown && power.charging == m_charging
            && power.percent == m_batteryPercent) {
        return;
    }

    QString line = QStringLiteral("battery ");
    if (power.known) {
        line += QStringLiteral("%1% %2").arg(power.percent)
                    .arg(power.charging ? QStringLiteral("charging")
                                        : QStringLiteral("discharging"));
    } else {
        line += QStringLiteral("unknown");
    }
    if (forced.known)
        line += QStringLiteral(" (forced by KELP_FORCE_BATTERY)");
    qInfo() << "KelpDeviceState:" << line;

    m_batteryKnown = power.known;
    m_charging = power.charging;
    m_batteryPercent = power.percent;
    emit changed();
}
