/*
 * File logger for Kelp.
 *
 * There is no backend yet, so the only way to see what the QML layer is doing
 * on a device is a log file. Everything written here — plus every Qt/QML
 * message, once install() has routed them in — lands in one file that can be
 * read over ssh (or from the About page) while the app runs.
 *
 * Usage from QML (exposed as the `kelpLog` context property):
 *
 *     kelpLog.info("added " + path)
 *     kelpLog.warn("model is undefined")
 *
 * console.log(...) and QML warnings are captured automatically.
 */

#ifndef KELPLOG_H
#define KELPLOG_H

#include <QObject>
#include <QString>

class KelpLog : public QObject
{
    Q_OBJECT

public:
    explicit KelpLog(QObject *parent = nullptr);

    // Routes every Qt/QML message (qDebug, qWarning, QML console.*, binding
    // errors) into the log file as well. Call once, as early as possible.
    static void install();

    // The app's data directory — the log and the object store live here. This is
    // the location Sailjail provisions for the app
    // (~/.local/share/<OrganizationName>/<ApplicationName>), so it is writable
    // both sandboxed and unsandboxed. A directory of our own naming under
    // ~/.local/share is NOT: a sandboxed app has the rest of that tree hidden and
    // cannot create anything in it, which is why a fresh install could not even
    // write its log. Requires the organization and application names to be set.
    static QString dataDirectory();

    // Absolute path of the log file. Shown on the About page and handy when
    // debugging over ssh.
    Q_INVOKABLE QString filePath() const;

    // Empties the log file.
    Q_INVOKABLE void clear();

    Q_INVOKABLE void debug(const QString &message);
    Q_INVOKABLE void info(const QString &message);
    Q_INVOKABLE void warn(const QString &message);
    Q_INVOKABLE void error(const QString &message);

private:
    void write(const QString &level, const QString &message);
};

#endif // KELPLOG_H
