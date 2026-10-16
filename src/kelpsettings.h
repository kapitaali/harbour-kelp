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

    // Write each share's kelp:// link into kelp.log so a device with no usable
    // clipboard (the emulator, a headless box) can be handed one. Off by
    // default: a link carries the decryption key, and the log is a plain file.
    Q_PROPERTY(bool includeLinksInLog READ includeLinksInLog
                   WRITE setIncludeLinksInLog NOTIFY includeLinksInLogChanged)

public:
    explicit KelpSettings(QObject *parent = nullptr);

    // The INI file the preferences are stored in. Static: the path is a
    // property of the app, not of any one instance.
    static QString filePath();

    bool includeLinksInLog() const;
    void setIncludeLinksInLog(bool on);

signals:
    void includeLinksInLogChanged();

private:
    QSettings m_settings;
};

#endif // KELPSETTINGS_H
