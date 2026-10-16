#include "kelpsettings.h"

#include "kelplog.h"

namespace {

// Namespaced so a future other section can share the file without collisions.
const char kIncludeLanAddressKey[] = "share/include_local_address";
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
    return m_settings.value(QString::fromLatin1(kIncludeLinksInLogKey), false).toBool();
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
