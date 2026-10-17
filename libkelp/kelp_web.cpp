#include "kelp_web.h"

#include <QFileInfo>

#include "kelp_object.h"

namespace Kelp {
namespace Web {

namespace {

// Reads one manifest object: the store gives back ciphertext, which is verified
// against its id by the store itself, and decrypted here.
bool readManifest(ObjectStore *store, const QByteArray &key, const QByteArray &id,
                  QByteArray *plaintext, QString *error)
{
    QByteArray object;
    if (!store->get(id, &object)) {
        *error = QStringLiteral("a manifest object is missing or corrupt");
        return false;
    }
    if (!Object::open(key, object, plaintext, error)) {
        if (error->isEmpty())
            *error = QStringLiteral("a manifest did not decrypt");
        return false;
    }
    return true;
}

// One directory's entries, whether the manifest is a leaf or a segment index
// pointing at leaves — the same two forms the bundle walk accepts, so a bundle
// that opens can also be previewed.
bool readEntries(ObjectStore *store, const QByteArray &key, const QByteArray &id,
                 QVector<Manifest::Entry> *entries, QString *error)
{
    QByteArray manifest;
    if (!readManifest(store, key, id, &manifest, error))
        return false;

    Manifest::Directory directory;
    if (!Manifest::decodeDirectory(manifest, &directory, error))
        return false;

    if (!directory.segment) {
        entries->append(directory.entries);
        return true;
    }

    for (int i = 0; i < directory.children.size(); ++i) {
        QByteArray leafManifest;
        if (!readManifest(store, key, directory.children.at(i), &leafManifest, error))
            return false;
        Manifest::Directory leaf;
        if (!Manifest::decodeDirectory(leafManifest, &leaf, error))
            return false;
        if (leaf.segment) {
            *error = QStringLiteral("a segment index points at another segment index");
            return false;
        }
        entries->append(leaf.entries);
    }
    return true;
}

// A page is HTML if the sender said so, or if it is named like one. Both are the
// sender's words, so this is not a check — it is which file to *offer*.
bool isHtml(const QString &name, const QString &mime)
{
    if (mime.startsWith(QLatin1String("text/html"), Qt::CaseInsensitive))
        return true;
    return name.endsWith(QLatin1String(".html"), Qt::CaseInsensitive)
        || name.endsWith(QLatin1String(".htm"), Qt::CaseInsensitive);
}

QString joined(const QString &prefix, const QString &name)
{
    return prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
}

bool collectHtml(ObjectStore *store, const QByteArray &key, const QByteArray &id,
                 const QString &prefix, QStringList *found, QString *error)
{
    QVector<Manifest::Entry> entries;
    if (!readEntries(store, key, id, &entries, error))
        return false;

    for (int i = 0; i < entries.size(); ++i) {
        const Manifest::Entry &entry = entries.at(i);
        const QString path = joined(prefix, entry.name);

        if (entry.isDir) {
            if (!collectHtml(store, key, entry.hash, path, found, error))
                return false;
            continue;
        }

        QByteArray manifest;
        if (!readManifest(store, key, entry.hash, &manifest, error))
            return false;
        Manifest::File file;
        if (!Manifest::decodeFile(manifest, &file, error))
            return false;

        if (isHtml(entry.name, file.mime))
            found->append(path);
    }
    return true;
}

// The shallowest of a list of paths.
QString shallowest(const QStringList &paths)
{
    QString best;
    for (int i = 0; i < paths.size(); ++i) {
        if (best.isEmpty()
            || paths.at(i).count(QLatin1Char('/')) < best.count(QLatin1Char('/')))
            best = paths.at(i);
    }
    return best;
}

QString directoryOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? QString() : path.left(slash);
}

} // namespace

bool page(ObjectStore *store, const QByteArray &key, const QByteArray &rootId,
          Page *page, QString *error)
{
    page->path.clear();
    page->root.clear();
    page->candidates.clear();

    QStringList html;
    if (!collectHtml(store, key, rootId, QString(), &html, error))
        return false;

    html.sort();
    page->candidates = html;

    // "index.html" at the bundle root, then any other index.html (the shallowest),
    // then the only HTML file there is. Anything else is a choice the recipient
    // makes, so it is left to them.
    QString best;
    if (html.contains(QLatin1String("index.html"), Qt::CaseInsensitive)) {
        best = QLatin1String("index.html");
    } else {
        QStringList indexes;
        for (int i = 0; i < html.size(); ++i) {
            if (QFileInfo(html.at(i)).fileName().compare(QLatin1String("index.html"),
                                                         Qt::CaseInsensitive) == 0)
                indexes.append(html.at(i));
        }
        if (!indexes.isEmpty())
            best = shallowest(indexes);
        else if (html.size() == 1)
            best = html.at(0);
    }

    page->path = best;
    page->root = best.isEmpty() ? QString() : directoryOf(best);
    return true;
}

QString mimeType(const Manifest::File &file, const QString &path)
{
    if (!file.mime.isEmpty())
        return file.mime;

    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const QString extension = dot < 0 ? QString()
                                      : path.mid(dot + 1).toLower();

    static const struct { const char *extension; const char *type; } table[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm",  "text/html; charset=utf-8" },
        { "css",  "text/css; charset=utf-8" },
        { "js",   "text/javascript; charset=utf-8" },
        { "mjs",  "text/javascript; charset=utf-8" },
        { "json", "application/json; charset=utf-8" },
        { "map",  "application/json; charset=utf-8" },
        { "webmanifest", "application/manifest+json; charset=utf-8" },
        { "txt",  "text/plain; charset=utf-8" },
        { "md",   "text/plain; charset=utf-8" },
        { "csv",  "text/csv; charset=utf-8" },
        { "xml",  "text/xml; charset=utf-8" },
        { "svg",  "image/svg+xml" },
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "webp", "image/webp" },
        { "ico",  "image/x-icon" },
        { "bmp",  "image/bmp" },
        { "pdf",  "application/pdf" },
        { "mp3",  "audio/mpeg" },
        { "wav",  "audio/wav" },
        { "ogg",  "audio/ogg" },
        { "oga",  "audio/ogg" },
        { "mp4",  "video/mp4" },
        { "m4v",  "video/mp4" },
        { "webm", "video/webm" },
        { "ogv",  "video/ogg" },
        { "woff", "font/woff" },
        { "woff2", "font/woff2" },
        { "ttf",  "font/ttf" },
        { "otf",  "font/otf" },
        { "wasm", "application/wasm" },
        { "zip",  "application/zip" },
    };

    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (extension == QLatin1String(table[i].extension))
            return QString::fromLatin1(table[i].type);
    }
    return QStringLiteral("application/octet-stream");
}

bool chunkRange(const Manifest::File &file, quint64 offset, quint64 length,
                int *first, int *count)
{
    if (offset > file.size || length > file.size - offset)
        return false;

    const quint64 chunk = Manifest::ChunkSize;
    *first = int(offset / chunk);
    if (length == 0) {
        *count = 0;
        return true;
    }

    const quint64 last = (offset + length - 1) / chunk;
    *count = int(last - quint64(*first) + 1);
    return true;
}

bool resolve(ObjectStore *store, const QByteArray &key, const QByteArray &rootId,
             const QString &path, bool *found, bool *isDir, QByteArray *hash,
             Manifest::File *file, QString *error)
{
    *found = false;
    *isDir = true;
    *hash = rootId;
    *file = Manifest::File();

    QStringList segments;
    if (!path.isEmpty())
        segments = path.split(QLatin1Char('/'), QString::SkipEmptyParts);

    QByteArray current = rootId;
    bool currentIsDir = true;

    for (int i = 0; i < segments.size(); ++i) {
        const QString name = Manifest::normalizeName(segments.at(i));

        // "..", "." or a NUL is not a name in any bundle, and a file cannot have
        // children: both are simply paths that name nothing.
        if (!Manifest::isValidName(name) || !currentIsDir)
            return true;

        QVector<Manifest::Entry> entries;
        if (!readEntries(store, key, current, &entries, error))
            return false;

        int match = -1;
        for (int j = 0; j < entries.size(); ++j) {
            if (entries.at(j).name == name) {
                match = j;
                break;
            }
        }
        if (match < 0)
            return true;

        current = entries.at(match).hash;
        currentIsDir = entries.at(match).isDir;
    }

    *hash = current;
    *isDir = currentIsDir;

    if (!currentIsDir) {
        QByteArray manifest;
        if (!readManifest(store, key, current, &manifest, error))
            return false;
        if (!Manifest::decodeFile(manifest, file, error))
            return false;
    }

    *found = true;
    return true;
}

} // namespace Web
} // namespace Kelp
