/*
 * What the device's connection and battery look like right now (design §12.8).
 *
 * This class reads; it does not decide. The decision — may this device serve?
 * — is Kelp::Policy::servingBlock() in libkelp, wrapped for QML by
 * src/kelppolicy.h, so the rule is testable without a device in any particular
 * state and there is one copy of it.
 *
 * Where the facts come from, and why not the obvious place: connman and MCE
 * would be the daemons to ask, but a sandboxed app may not talk to either. The
 * device's own permission files (/etc/sailjail/permissions/Internet.permission)
 * grant an app with the Internet permission exactly org.ofono, lipstick's
 * connection selector and com.jolla.PinQuery — net.connman and com.nokia.mce
 * are not among them. So this reads the same kernel the daemons read:
 *
 *   - /sys/class/power_supply/<supply>/type, capacity, status   (the battery)
 *   - /sys/class/net/<if>/wireless + the interface's name       (what it is)
 *   - QNetworkInterface                                        (is it usable)
 *
 * The app's firejail profile adds only --private-bin, no private /sys or /proc,
 * so both are readable from inside the sandbox (verified on a C2: the battery
 * reads "Battery 56% Discharging", and the cellular interface is ccmni1).
 *
 * Polled, not evented: a connection that arrives and leaves inside one interval
 * can be missed, and that is the honest description of the mechanism rather
 * than a claim that it cannot. Reading a handful of files is free, so the
 * interval is short enough that a user will not notice either way.
 */

#ifndef KELPDEVICESTATE_H
#define KELPDEVICESTATE_H

#include <QObject>
#include <QString>
#include <QTimer>

#include "kelp_policy.h"

class KelpDeviceState : public QObject
{
    Q_OBJECT

    // Any usable connection at all: an interface that is up and carries an
    // address. (Whether it is the connection serving would use is `metered`.)
    Q_PROPERTY(bool online READ online NOTIFY changed)

    // The device's only usable connection is cellular — Kelp::Policy::metered().
    // No connection at all is *not* metered: there is nothing to serve over, and
    // the row should say so rather than blame a data plan.
    Q_PROPERTY(bool metered READ metered NOTIFY changed)

    // The interface serving would use, and what kind it is: "wifi", "cellular",
    // "other", or "" when there is none. For the log and the Settings page —
    // naming the interface is what makes a metered verdict checkable.
    Q_PROPERTY(QString interfaceName READ interfaceName NOTIFY changed)
    Q_PROPERTY(QString interfaceKind READ interfaceKind NOTIFY changed)

    // The battery. false `batteryKnown` is a machine with no battery at all (the
    // emulator) or a supply whose files could not be read; an unknown battery
    // never stops anything.
    Q_PROPERTY(bool batteryKnown READ batteryKnown NOTIFY changed)
    Q_PROPERTY(bool charging READ charging NOTIFY changed)
    Q_PROPERTY(int batteryPercent READ batteryPercent NOTIFY changed)

public:
    explicit KelpDeviceState(QObject *parent = nullptr);

    bool online() const { return m_online; }
    bool metered() const { return m_metered; }
    QString interfaceName() const { return m_interfaceName; }
    QString interfaceKind() const { return m_interfaceKind; }
    bool batteryKnown() const { return m_batteryKnown; }
    bool charging() const { return m_charging; }
    int batteryPercent() const { return m_batteryPercent; }

    // How often both sources are read. Short enough that a user who plugs the
    // phone in sees serving come back while they are still looking at it.
    static const int kPollMs = 20000;

    // Reads both sources now. Called by the poll, and once at construction so a
    // launch into a blocked state is blocked from the start rather than one
    // interval later.
    Q_INVOKABLE void refresh();

signals:
    // Emitted only when a value really changed, so a binding does not re-evaluate
    // (and the policy does not re-log) every twenty seconds.
    void changed();

private:
    void readNetwork();
    void readPower();

    QTimer m_timer;
    bool m_online;
    bool m_metered;
    QString m_interfaceName;
    QString m_interfaceKind;
    bool m_batteryKnown;
    bool m_charging;
    int m_batteryPercent;
};

#endif // KELPDEVICESTATE_H
