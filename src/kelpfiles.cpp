#include "kelpfiles.h"

#include <QDir>
#include <QFileInfo>

KelpFiles::KelpFiles(QObject *parent)
    : QObject(parent)
{
}

QVariantMap KelpFiles::describe(const QString &path) const
{
    QVariantMap result;
    result.insert("kind", "other");
    result.insert("fileCount", 0);
    result.insert("totalBytes", qint64(0));

    const QFileInfo info(path);
    if (!info.exists())
        return result;

    if (info.isDir()) {
        int files = 0;
        qint64 bytes = 0;
        accumulate(path, files, bytes);
        result["kind"] = "folder";
        result["fileCount"] = files;
        result["totalBytes"] = bytes;
    } else if (info.isFile()) {
        result["kind"] = "file";
        result["fileCount"] = 1;
        result["totalBytes"] = info.size();
    }
    return result;
}

void KelpFiles::accumulate(const QString &dir, int &fileCount, qint64 &totalBytes)
{
    // Hidden entries count toward the size, but symlinks do not: a link back
    // up the tree would recurse forever.
    const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                        | QDir::Hidden,
                                    QDir::Name);

    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink())
            continue;
        if (entry.isDir())
            accumulate(entry.absoluteFilePath(), fileCount, totalBytes);
        else if (entry.isFile()) {
            ++fileCount;
            totalBytes += entry.size();
        }
    }
}

QString KelpFiles::formatSize(qint64 bytes) const
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };

    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);

    double value = double(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}
