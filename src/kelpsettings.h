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
    // clipboard (the emulator, a phone read over ssh) can be handed one. The
    // default follows the dev-extras bundle (src/kelpbuild.h): on in a dev build,
    // off in a release, where the switch is not offered either — a link carries
    // its bundle's key, so the log is then a secret.
    Q_PROPERTY(bool includeLinksInLog READ includeLinksInLog
                   WRITE setIncludeLinksInLog NOTIFY includeLinksInLogChanged)

    // Where a fetched bundle is written. Empty means the platform's Downloads
    // folder, which is also the default. The Open-a-link page sets it when the
    // user picks a folder, and remembers the choice across restarts.
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory
                   WRITE setDownloadDirectory NOTIFY downloadDirectoryChanged)

    // Settings → Share in background. On by default. What it asks the platform
    // for is KeepAlive (the root QML holds it while something is really being
    // served); closing Kelp still ends serving — there is no always-on service.
    Q_PROPERTY(bool shareInBackground READ shareInBackground
                   WRITE setShareInBackground NOTIFY shareInBackgroundChanged)

    // Settings → Wi-Fi only. On by default. The label says the real rule: never
    // seed over mobile data. The serving policy (src/kelppolicy.h and libkelp's
    // kelp_policy.h) is what acts on it; this is only where the choice lives.
    Q_PROPERTY(bool wifiOnly READ wifiOnly
                   WRITE setWifiOnly NOTIFY wifiOnlyChanged)

    // Settings → Allow peer swarm. Off by default. Persisted so the switch is
    // honest about being a remembered choice, but nothing serves peers yet —
    // the per-share option it gates is still not implemented (design §12.6).
    Q_PROPERTY(bool peerSwarm READ peerSwarm
                   WRITE setPeerSwarm NOTIFY peerSwarmChanged)

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

    bool shareInBackground() const;
    void setShareInBackground(bool on);

    bool wifiOnly() const;
    void setWifiOnly(bool on);

    bool peerSwarm() const;
    void setPeerSwarm(bool on);

signals:
    void includeLanAddressChanged();
    void includeLinksInLogChanged();
    void downloadDirectoryChanged();
    void shareInBackgroundChanged();
    void wifiOnlyChanged();
    void peerSwarmChanged();

private:
    QSettings m_settings;
};

#endif // KELPSETTINGS_H
