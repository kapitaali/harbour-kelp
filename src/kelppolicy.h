/*
 * May this device serve right now? (design §12.8)
 *
 * The rule itself is Kelp::Policy::servingBlock() in libkelp — pure, and tested
 * with the whole truth table. This class is the QML-facing wrapper: it holds
 * the two inputs that change (the settings switch and what the device looks
 * like) and re-answers whenever either moves.
 *
 * QML asks two things: `block` (a token: "" / "metered" / "battery") and
 * `mayServe`. The wording is not built here — a qsTr() string belongs in QML —
 * but the *reason* is a token rather than a sentence, so the log line and the
 * message on the row cannot drift apart.
 *
 * The root QML is the only thing that acts on it: when the block appears it
 * stops the shares that are being served and remembers which; when it clears it
 * serves them again, which re-mints the same link because the LAN listener
 * never moved. Nothing is written to shares.json that the user did not choose.
 */

#ifndef KELPPOLICY_H
#define KELPPOLICY_H

#include <QObject>
#include <QString>

#include "kelp_policy.h"

class KelpDeviceState;
class KelpSettings;

class KelpPolicy : public QObject
{
    Q_OBJECT

    // Whether serving may continue. False while a block is reported below.
    Q_PROPERTY(bool mayServe READ mayServe NOTIFY changed)

    // "" when serving may continue, otherwise "metered" or "battery". A token,
    // not a sentence: QML turns it into qsTr() wording.
    Q_PROPERTY(QString block READ block NOTIFY changed)

    // The battery threshold, read-only, so the sentence the Settings page shows
    // names the number the rule actually uses (libkelp's kelp_policy.h).
    Q_PROPERTY(int lowBatteryPercent READ lowBatteryPercent CONSTANT)

public:
    KelpPolicy(KelpSettings *settings, KelpDeviceState *state, QObject *parent = nullptr);

    bool mayServe() const { return m_block == Kelp::Policy::ServingBlock::None; }
    QString block() const;

    int lowBatteryPercent() const { return Kelp::Policy::kLowBatteryPercent; }

    Kelp::Policy::ServingBlock blockValue() const { return m_block; }

signals:
    void changed();

private:
    void reevaluate();

    KelpSettings *m_settings;
    KelpDeviceState *m_state;
    Kelp::Policy::ServingBlock m_block;
};

#endif // KELPPOLICY_H
