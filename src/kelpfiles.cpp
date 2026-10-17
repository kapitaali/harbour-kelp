#include "kelpfiles.h"

#include <QDir>
#include <QFileInfo>

#include "kelp_web.h"

KelpFiles::KelpFiles(QObject *parent)
    : QObject(parent)
{
}

// Which file in a folder a preview would offer as the page (design §5.3), asked
// before the folder is sealed as a webpage (design §5.5). Only names are read —
// no content, no mime, since Kelp records no mime when sealing — and the rule
// itself is Kelp::Web::chooseEntry, the one the preview uses, so the sender is
// never told a folder has a page that the recipient's preview cannot find.
//
// The walk mirrors what sealing will do: the same entries in the same order,
// symlinks skipped rather than followed, and paths relative to the folder. The
// folder's own name prefixes every path once sealed, but that shifts every
// candidate equally and so cannot change which one is chosen.
QVariantMap KelpFiles::pageEntry(const QString &path) const
{
    QVariantMap result;
    result.insert("ok", false);
    result.insert("hasPage", false);
    result.insert("entry", QString());
    result.insert("candidates", QStringList());

    const QFileInfo info(path);
    if (!info.exists()) {
        result.insert("error", QStringLiteral("%1 no longer exists").arg(path));
        return result;
    }
    if (!info.isDir()) {
        result.insert("error", QStringLiteral("pick the folder that holds the page"));
        return result;
    }

    QStringList html;
    collectHtmlNames(path, QString(), &html);

    QString root;
    const QString entry = Kelp::Web::chooseEntry(html, &root);

    result["ok"] = true;
    result["hasPage"] = !entry.isEmpty();
    result["entry"] = entry;
    result["candidates"] = html;
    return result;
}

void KelpFiles::collectHtmlNames(const QString &dir, const QString &prefix,
                                 QStringList *found)
{
    const QFileInfoList entries =
            QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);

    for (const QFileInfo &entry : entries) {
        // A link back up the tree would recurse forever, so links are skipped
        // here exactly as sealing skips them.
        if (entry.isSymLink())
            continue;

        const QString name = entry.fileName();
        const QString relative =
                prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;

        if (entry.isDir()) {
            collectHtmlNames(entry.absoluteFilePath(), relative, found);
            continue;
        }
        if (Kelp::Web::isHtmlName(name))
            found->append(relative);
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
