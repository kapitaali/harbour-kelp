#include "kelplog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

namespace {

// Keep one previous file so a crash-heavy session does not grow without bound.
const qint64 MaxLogBytes = 512 * 1024;

QMutex g_mutex;
QtMessageHandler g_previousHandler = nullptr;

QString logDirectory()
{
    return KelpLog::dataDirectory();
}

QString logFilePath()
{
    return logDirectory() + QStringLiteral("/kelp.log");
}

void rotateIfNeeded()
{
    if (QFileInfo(logFilePath()).size() < MaxLogBytes)
        return;

    const QString previous = logFilePath() + QStringLiteral(".1");
    QFile::remove(previous);
    QFile::rename(logFilePath(), previous);
}

// Appends one line to the log file. Caller must not already hold g_mutex.
void logLine(const QString &level, const QString &message)
{
    QMutexLocker locker(&g_mutex);

    QDir().mkpath(logDirectory());
    rotateIfNeeded();

    QFile file(logFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        fprintf(stderr, "%s %s\n", qPrintable(level), qPrintable(message));
        return;
    }

    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    QTextStream out(&file);
    out << stamp << ' ' << level << ' ' << message << '\n';
    out.flush();
}

QString levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return QStringLiteral("DEBUG");
    case QtInfoMsg:     return QStringLiteral("INFO ");
    case QtWarningMsg:  return QStringLiteral("WARN ");
    case QtCriticalMsg: return QStringLiteral("ERROR");
    case QtFatalMsg:    return QStringLiteral("FATAL");
    }
    return QStringLiteral("?????");
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString line = message;
    if (context.file) {
        line += QStringLiteral("  (") + QString::fromUtf8(context.file);
        if (context.line > 0)
            line += QStringLiteral(":") + QString::number(context.line);
        line += QLatin1Char(')');
    }

    logLine(levelName(type), line);

    // Still hand the message to the default handler so stderr/journal keeps
    // working for direct launches.
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    else
        fprintf(stderr, "%s %s\n", qPrintable(levelName(type)), qPrintable(line));
}

} // namespace

KelpLog::KelpLog(QObject *parent)
    : QObject(parent)
{
}

QString KelpLog::dataDirectory()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (path.isEmpty())
        path = QDir::homePath() + QStringLiteral("/.local/share/harbour.kelp/harbour-kelp");

    // One-off move from the pre-Sailjail-aware location, so objects and links
    // sealed by an earlier build keep working. Best-effort: inside the sandbox
    // the old directory is not even visible, so this quietly does nothing there.
    const QString legacy = QDir::homePath() + QStringLiteral("/.local/share/harbour-kelp");
    if (path != legacy && !QDir(path).exists() && QDir(legacy).exists())
        QDir().rename(legacy, path);

    return path;
}

void KelpLog::install()
{
    rotateIfNeeded();
    g_previousHandler = qInstallMessageHandler(messageHandler);

    logLine(QStringLiteral("INFO "), QStringLiteral("---- log opened ----"));
}

QString KelpLog::filePath() const
{
    return logFilePath();
}

void KelpLog::clear()
{
    QMutexLocker locker(&g_mutex);
    QFile::remove(logFilePath());
}

void KelpLog::debug(const QString &message) { write(QStringLiteral("DEBUG"), message); }
void KelpLog::info(const QString &message)  { write(QStringLiteral("INFO "), message); }
void KelpLog::warn(const QString &message)  { write(QStringLiteral("WARN "), message); }
void KelpLog::error(const QString &message) { write(QStringLiteral("ERROR"), message); }

void KelpLog::write(const QString &level, const QString &message)
{
    logLine(level, message);
    fprintf(stderr, "%s %s\n", qPrintable(level), qPrintable(message));
}
