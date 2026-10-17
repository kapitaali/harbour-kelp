#ifndef KELPFILES_H
#define KELPFILES_H

#include <QObject>
#include <QStringList>
#include <QVariantMap>

/*
 * Minimal filesystem helper for the UI.
 *
 * This is deliberately not part of libkelp: it answers questions about paths on
 * this device — which file in a folder is the page, and how big something is in
 * words — that QML cannot answer on its own. Walking a tree needs a recursion,
 * so it lives in C++.
 */
class KelpFiles : public QObject
{
    Q_OBJECT

public:
    explicit KelpFiles(QObject *parent = nullptr);

    // Which file in a folder would be offered as its web page, before the folder
    // is sealed as a webpage (design §5.3, §5.5). Returns a map:
    //   ok         -> the folder could be walked
    //   error      -> why not, when `ok` is false
    //   hasPage    -> whether the convention finds a page at all
    //   entry      -> the page, folder-relative ("site/index.html"), else ""
    //   candidates -> every HTML file in it, sorted
    Q_INVOKABLE QVariantMap pageEntry(const QString &path) const;

    // Human-readable size, e.g. "4.2 MB".
    Q_INVOKABLE QString formatSize(qint64 bytes) const;

private:
    static void collectHtmlNames(const QString &dir, const QString &prefix,
                                 QStringList *found);
};

#endif // KELPFILES_H
