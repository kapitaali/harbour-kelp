#include "kelppolicy.h"

#include <QDebug>

#include "kelpdevicestate.h"
#include "kelpsettings.h"

using Kelp::Policy::Power;
using Kelp::Policy::ServingBlock;

namespace {

// What the device looks like, for the log line beside a verdict. Only ever read
// by a human looking at kelp.log.
QString describe(const KelpDeviceState *state)
{
    QString text;
    if (state->online()) {
        text = QStringLiteral("on %1 (%2)").arg(
            state->interfaceName().isEmpty() ? QStringLiteral("?") : state->interfaceName(),
            state->interfaceKind());
    } else {
        text = QStringLiteral("no connection");
    }

    if (state->batteryKnown()) {
        text += QStringLiteral(", battery %1% %2")
                    .arg(state->batteryPercent())
                    .arg(state->charging() ? QStringLiteral("charging")
                                           : QStringLiteral("discharging"));
    } else {
        text += QStringLiteral(", battery unknown");
    }
    return text;
}

} // namespace

KelpPolicy::KelpPolicy(KelpSettings *settings, KelpDeviceState *state, QObject *parent)
    : QObject(parent),
      m_settings(settings),
      m_state(state),
      m_block(ServingBlock::None)
{
    // Either input moving is a reason to ask again.
    connect(m_state, &KelpDeviceState::changed, this, &KelpPolicy::reevaluate);
    connect(m_settings, &KelpSettings::wifiOnlyChanged, this, &KelpPolicy::reevaluate);

    m_block = Kelp::Policy::servingBlock(m_settings->wifiOnly(), m_state->metered(),
                                         Power(m_state->batteryKnown(), m_state->charging(),
                                               m_state->batteryPercent()));

    // Logged once at startup even when nothing blocks: "is serving allowed, and
    // why" is the first question when a link does not answer.
    qInfo() << "KelpPolicy: serving" << (mayServe() ? "allowed" : "stopped")
            << "-" << describe(m_state)
            << "- Wi-Fi only" << (m_settings->wifiOnly() ? "on" : "off");
}

QString KelpPolicy::block() const
{
    switch (m_block) {
    case ServingBlock::None: return QString();
    case ServingBlock::Metered: return QStringLiteral("metered");
    case ServingBlock::Battery: return QStringLiteral("battery");
    }
    return QString();
}

void KelpPolicy::reevaluate()
{
    const ServingBlock block =
        Kelp::Policy::servingBlock(m_settings->wifiOnly(), m_state->metered(),
                                   Power(m_state->batteryKnown(), m_state->charging(),
                                         m_state->batteryPercent()));
    if (block == m_block)
        return;

    m_block = block;

    switch (m_block) {
    case ServingBlock::Metered:
        qInfo() << "KelpPolicy: serving stopped — this is the only connection and it"
                   " is mobile data, with Wi-Fi only on -" << describe(m_state);
        break;
    case ServingBlock::Battery:
        qInfo() << "KelpPolicy: serving stopped — battery low and not charging -"
                << describe(m_state);
        break;
    case ServingBlock::None:
        qInfo() << "KelpPolicy: serving allowed again -" << describe(m_state);
        break;
    }

    emit changed();
}
