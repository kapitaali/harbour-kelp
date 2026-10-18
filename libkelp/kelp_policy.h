#ifndef KELP_POLICY_H
#define KELP_POLICY_H

#include <QString>
#include <QVector>

namespace Kelp {
namespace Policy {

/*
 * When this device may serve (design §12.8).
 *
 * Serving costs battery and, on a mobile connection, data. Two rules say when
 * it must stop, and both are answered here rather than in the UI, so the rule
 * the user reads about and the rule the app acts on are the same code:
 *
 *   - "Wi-Fi only" (the switch's own label is *never seed over mobile data*):
 *     stop when the device's only usable connection is cellular.
 *   - low battery: stop below the threshold while not charging, and come back
 *     by itself when it is charging again.
 *
 * Everything in this file is pure: text in, answers out. The reading of the
 * kernel's files and of the network interfaces is the app's business
 * (src/kelpdevicestate.cpp), which is what makes the rules testable without a
 * device in any particular state.
 *
 * One honest limit, written down here because it is a property of the rule and
 * not of any caller: with Wi-Fi and mobile data both up, the device is *not*
 * metered, and a relay tunnel may still ride the mobile connection. The
 * question answered here is about the device's connection, not about each
 * socket. Relays are off by default.
 */

// Below this, and not charging, serving stops. A constant rather than a
// setting: it is a courtesy limit, and the two numbers that matter (stopped,
// resumed) are the transitions a user notices.
const int kLowBatteryPercent = 15;

// What a network interface is, as far as this rule cares.
enum class InterfaceKind {
    Other,      // wired, a tunnel, anything we cannot name — unmetered
    Wifi,
    Cellular
};

// The battery, as the kernel reports it. `known` is false on a machine with no
// battery at all (the emulator) or when the files could not be read; an unknown
// battery never blocks serving.
struct Power {
    bool known;
    bool charging;
    int percent;    // 0..100, or -1 when unreadable

    Power() : known(false), charging(false), percent(-1) {}
    Power(bool known_, bool charging_, int percent_)
        : known(known_), charging(charging_), percent(percent_) {}
};

// Why serving is stopped, or that it is not.
enum class ServingBlock {
    None,
    Metered,    // the only usable connection is mobile data, and Wi-Fi-only is on
    Battery     // the battery is low and the device is not charging
};

// A network interface's kind, from its name and whether the kernel marks it
// wireless (`/sys/class/net/<if>/wireless` exists — what Wi-Fi actually means,
// and true for wlan0, ap0 and p2p0 on a C2). The cellular names are a
// heuristic, and the reason the app logs its classification every time it
// changes: ccmni* (MediaTek, the C2), rmnet*, wwan*, pdp_ip*, ppp*.
//
// A name that is neither wireless nor a known cellular name — eth0, usb0, tun0
// — is Other, which is *unmetered* for this rule. Guessing "metered" for an
// unknown interface would stop serving on a wired network or a VPN.
InterfaceKind classifyInterface(const QString &name, bool wireless);

// Whether the kinds of the interfaces that are up and carry an address (the
// caller filters loopback and link-local away) add up to a metered connection:
// at least one usable interface, and every one of them cellular. No usable
// interface at all is *not* metered — there is nothing to serve over, and the
// row should say what is really wrong instead of blaming the data plan.
bool metered(const QVector<InterfaceKind> &usable);

// The battery, from the kernel's `status` word ("Charging", "Discharging",
// "Full", "Not charging", …) and its `capacity` percentage (-1 if unreadable).
// "Full" counts as charging: it is plugged in and there is nothing to save.
// Anything else, or a missing capacity, is a Power that is not known.
Power powerFromStatus(const QString &status, int capacity);

// The whole rule: the two inputs and the facts in, one reason out. When both
// rules would stop serving, the data plan is the answer reported — the explicit
// user preference is the more surprising of the two to violate, and either
// message is true.
ServingBlock servingBlock(bool wifiOnly, bool meteredConnection, const Power &power,
                          int lowBatteryPercent = kLowBatteryPercent);

} // namespace Policy
} // namespace Kelp

#endif // KELP_POLICY_H
