#include "kelp_policy.h"

#include <QStringList>

namespace Kelp {
namespace Policy {

namespace {

// The interface-name prefixes that mean "this is the mobile connection".
// MediaTek (ccmni — the Jolla C2), Qualcomm (rmnet), Intel/others (wwan),
// Apple/older Android (pdp_ip), and classic PPP. Prefixes, because the number
// or a suffix varies: ccmni1, rmnet_data0, wwan0.
const char *const kCellularPrefixes[] = {
    "ccmni", "rmnet", "wwan", "pdp", "ppp"
};

} // namespace

InterfaceKind classifyInterface(const QString &name, bool wireless)
{
    // The kernel's own answer first: a Wi-Fi interface (or a SoftAP, or a P2P
    // one) carries a `wireless` file and nothing else on the device does. A
    // cellular interface never has one, whatever it is called.
    if (wireless)
        return InterfaceKind::Wifi;

    for (const char *prefix : kCellularPrefixes) {
        if (name.startsWith(QLatin1String(prefix)))
            return InterfaceKind::Cellular;
    }

    return InterfaceKind::Other;
}

bool metered(const QVector<InterfaceKind> &usable)
{
    if (usable.isEmpty())
        return false;

    for (InterfaceKind kind : usable) {
        if (kind != InterfaceKind::Cellular)
            return false;
    }

    return true;
}

Power powerFromStatus(const QString &status, int capacity)
{
    const QString word = status.trimmed();

    if (word.isEmpty() || capacity < 0 || capacity > 100)
        return Power();

    const bool charging = word.compare(QLatin1String("Charging"), Qt::CaseInsensitive) == 0
            || word.compare(QLatin1String("Full"), Qt::CaseInsensitive) == 0;

    return Power(true, charging, capacity);
}

ServingBlock servingBlock(bool wifiOnly, bool meteredConnection, const Power &power,
                          int lowBatteryPercent)
{
    if (wifiOnly && meteredConnection)
        return ServingBlock::Metered;

    if (power.known && !power.charging && power.percent >= 0
            && power.percent < lowBatteryPercent) {
        return ServingBlock::Battery;
    }

    return ServingBlock::None;
}

} // namespace Policy
} // namespace Kelp
