#ifndef KELPFILES_H
#define KELPFILES_H

#include <QObject>
#include <QVariantMap>

/*
 * Minimal filesystem helper for the UI.
 *
 * This is deliberately not part of libkelp: it only answers "how big is this
 * thing and how many files are in it" for the Add page, which QML cannot do
 * on its own. Reading sizes needs to recurse and stat, so it lives in C++.
 */
class KelpFiles : public QObject
{
    Q_OBJECT

public:
    explicit KelpFiles(QObject *parent = nullptr);

    // Recursively describe a path. Returns a map:
    //   kind       -> "file" | "folder" | "other"
    //   fileCount  -> number of regular files (1 for a file)
    //   totalBytes -> summed size
    Q_INVOKABLE QVariantMap describe(const QString &path) const;

    // Human-readable size, e.g. "4.2 MB".
    Q_INVOKABLE QString formatSize(qint64 bytes) const;

private:
    static void accumulate(const QString &dir, int &fileCount, qint64 &totalBytes);
};

#endif // KELPFILES_H
