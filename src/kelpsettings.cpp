#include "kelpsettings.h"

#include "kelpbuild.h"
#include "kelplog.h"

namespace {

// Namespaced so a future other section can share the file without collisions.
const char kIncludeLanAddressKey[] = "share/include_local_address";
const char kShareInBackgroundKey[] = "share/in_background";
const char kWifiOnlyKey[] = "share/wifi_only";
const char kPeerSwarmKey[] = "share/peer_swarm";
const char kIncludeLinksInLogKey[] = "log/include_links";
const char kDownloadDirectoryKey[] = "download/directory";

} // namespace

KelpSettings::KelpSettings(QObject *parent)
    : QObject(parent),
      m_settings(filePath(), QSettings::IniFormat)
{
}

QString KelpSettings::filePath()
{
    return KelpLog::dataDirectory() + QStringLiteral("/settings.conf");
}

bool KelpSettings::includeLanAddress() const
{
    // On by default: a link that names its device is what the same-network path
    // needs, and excluding the address costs the LAN fast path.
    return m_settings.value(QString::fromLatin1(kIncludeLanAddressKey), true).toBool();
}

void KelpSettings::setIncludeLanAddress(bool on)
{
    if (on == includeLanAddress())
        return;

    m_settings.setValue(QString::fromLatin1(kIncludeLanAddressKey), on);
    m_settings.sync();
    emit includeLanAddressChanged();
}

bool KelpSettings::includeLinksInLog() const
{
    // The default follows the dev-extras bundle (src/kelpbuild.h): on in a dev
    // build, off in a release. The bundle's guarantee is enforced where the link
    // would actually be written (KelpBundles), so even a preference carried over
    // from a dev build cannot make a release log a link; here the setting is
    // simply a setting, and the switch that changes it is only offered in a build
    // where it can work (SettingsPage).
    return m_settings.value(QString::fromLatin1(kIncludeLinksInLogKey),
                            KelpBuild::devExtras()).toBool();
}

void KelpSettings::setIncludeLinksInLog(bool on)
{
    if (on == includeLinksInLog())
        return;

    m_settings.setValue(QString::fromLatin1(kIncludeLinksInLogKey), on);
    m_settings.sync();
    emit includeLinksInLogChanged();
}

QString KelpSettings::downloadDirectory() const
{
    return m_settings.value(QString::fromLatin1(kDownloadDirectoryKey)).toString();
}

void KelpSettings::setDownloadDirectory(const QString &path)
{
    if (path == downloadDirectory())
        return;

    m_settings.setValue(QString::fromLatin1(kDownloadDirectoryKey), path);
    m_settings.sync();
    emit downloadDirectoryChanged();
}

bool KelpSettings::shareInBackground() const
{
    // On by default: serving in the background is what "share" means to a
    // recipient who opens the link later, and the platform ends us anyway when
    // the app is closed.
    return m_settings.value(QString::fromLatin1(kShareInBackgroundKey), true).toBool();
}

void KelpSettings::setShareInBackground(bool on)
{
    if (on == shareInBackground())
        return;

    m_settings.setValue(QString::fromLatin1(kShareInBackgroundKey), on);
    m_settings.sync();
    emit shareInBackgroundChanged();
}

bool KelpSettings::wifiOnly() const
{
    // On by default: burning a data plan to serve a file is the surprise this
    // switch exists to prevent.
    return m_settings.value(QString::fromLatin1(kWifiOnlyKey), true).toBool();
}

void KelpSettings::setWifiOnly(bool on)
{
    if (on == wifiOnly())
        return;

    m_settings.setValue(QString::fromLatin1(kWifiOnlyKey), on);
    m_settings.sync();
    emit wifiOnlyChanged();
}

bool KelpSettings::peerSwarm() const
{
    // Off by default, and it is a permission rather than a behaviour: nothing
    // serves peers yet, so this only decides what the per-share option, when it
    // exists, is allowed to offer.
    return m_settings.value(QString::fromLatin1(kPeerSwarmKey), false).toBool();
}

void KelpSettings::setPeerSwarm(bool on)
{
    if (on == peerSwarm())
        return;

    m_settings.setValue(QString::fromLatin1(kPeerSwarmKey), on);
    m_settings.sync();
    emit peerSwarmChanged();
}
