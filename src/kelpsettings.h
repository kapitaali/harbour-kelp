/*
 * Persisted user preferences.
 *
 * Most of the Settings page is plain QML state that resets on restart; only the
 * handful of choices the app must remember live here. Both the Settings page
 * and the code that acts on a preference (KelpBundles, for the log setting)
 * read it from this one object, so the value cannot drift between what the user
 * sees and what the app does.
 *
 * Storage is a small INI file in the app's own data directory — the directory
 * Sailjail provisions, beside the log and the object store. QSettings' default
 * ~/.config location is deliberately avoided: the sandbox hides it, so a
 * preference written there would silently fail to persist.
 */

#ifndef KELPSETTINGS_H
#define KELPSETTINGS_H

#include <QObject>
#include <QSettings>
#include <QString>

class KelpSettings : public QObject
{
    Q_OBJECT

    // Whether a share's link names this device on the LAN (as `l=host:port`) and
    // the bundle is served there. On by default: that is the fast path for a
    // recipient on the same network. Off is for a share that must not disclose a
    // local address — the bundle is not served on the LAN and its link carries no
    // address, so it can only be fetched through a relay. A link is handed to
    // whoever the user sends it to, so this leaks the address to them.
    Q_PROPERTY(bool includeLanAddress READ includeLanAddress
                   WRITE setIncludeLanAddress NOTIFY includeLanAddressChanged)

    // Write each share's kelp:// link into kelp.log so a device with no usable
    // clipboard (the emulator, a headless box) can be handed one. Off by
    // default: a link carries the decryption key, and the log is a plain file.
    Q_PROPERTY(bool includeLinksInLog READ includeLinksInLog
                   WRITE setIncludeLinksInLog NOTIFY includeLinksInLogChanged)

    // Where a fetched bundle is written. Empty means the platform's Downloads
    // folder, which is also the default. The Open-a-link page sets it when the
    // user picks a folder, and remembers the choice across restarts.
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory
                   WRITE setDownloadDirectory NOTIFY downloadDirectoryChanged)

public:
    explicit KelpSettings(QObject *parent = nullptr);

    // The INI file the preferences are stored in. Static: the path is a
    // property of the app, not of any one instance.
    static QString filePath();

    bool includeLanAddress() const;
    void setIncludeLanAddress(bool on);

    bool includeLinksInLog() const;
    void setIncludeLinksInLog(bool on);

    QString downloadDirectory() const;
    void setDownloadDirectory(const QString &path);

signals:
    void includeLanAddressChanged();
    void includeLinksInLogChanged();
    void downloadDirectoryChanged();

private:
    QSettings m_settings;
};

#endif // KELPSETTINGS_H
