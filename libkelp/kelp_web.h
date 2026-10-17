#ifndef KELP_WEB_H
#define KELP_WEB_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "kelp_manifest.h"
#include "kelp_store.h"

namespace Kelp {
namespace Web {

/*
 * A bundle's web page (design §5.1, §5.3).
 *
 * A bundle can contain a page, and a recipient's browser can be shown one. Which
 * file that is, and how a URL path maps onto the bundle, is a convention over the
 * ordinary manifests — no new wire field, nothing the sender has to declare, and
 * a link that carries a page is an ordinary link.
 *
 * Everything here decodes manifests only: a data object is never read, so asking
 * a question about a bundle's page costs its manifests and nothing more, however
 * large the bundle is. Nothing here touches I/O beyond the store it is given.
 */

// The page a bundle offers.
struct Page {
    QString path;             // entry document, bundle-relative ("site/index.html")
    QString root;             // directory that becomes "/" ("" = the bundle root)
    QStringList candidates;   // every HTML file, sorted; what a picker would show

    Page() {}
};

// Whether a *name* is HTML by the convention's rule (design §5.3): it ends
// ".html" or ".htm". The manifest side also counts a sender-recorded `mime`,
// which a folder on disk has no equivalent of, so this is only the name half —
// shared so that a folder checked before sealing and the preview of the bundle
// it becomes answer the same question the same way.
bool isHtmlName(const QString &name);

// The entry document chosen out of a bundle's HTML files (design §5.3):
// "index.html" at the bundle root, else the shallowest "index.html", else the
// only HTML file there is, else nothing. `htmlPaths` are bundle-relative and may
// arrive in any order; `rootOut` is the chosen file's directory — what should
// become "/" — and is empty when there is no page.
//
// This is the one copy of the rule. page() feeds it the names a bundle's
// manifests hold; the app feeds it the names in a folder before sealing that
// folder as a webpage (design §5.5), so what a sender is told about their folder
// and what a recipient's preview later finds cannot drift apart.
QString chooseEntry(const QStringList &htmlPaths, QString *rootOut);

// Finds the page (design §5.3): "index.html" at the bundle root; else an
// "index.html" anywhere, the shallowest one; else the only HTML file there is;
// else nothing, with every HTML file in `candidates` for a picker.
//
// False only when the bundle's manifests cannot be read — the bundle is not all
// here yet. A bundle with no page is a *true* answer with `page->path` empty,
// because that is a fact about the bundle, not a failure.
bool page(ObjectStore *store, const QByteArray &key, const QByteArray &rootId,
          Page *page, QString *error);

// What a browser should be told a path is: the manifest's own `mime` when the
// sender recorded one, else the extension table, else application/octet-stream.
//
// The extension table is the common path, not the fallback: nothing in the app
// sets `mime` when sealing, so a bundle made by Kelp has no MIME types in it at
// all, and a link must not depend on `mime` being there.
QString mimeType(const Manifest::File &file, const QString &path);

// The chunk range covering `length` bytes at `offset`, with the fixed-size
// chunking of §1.2: byte n lives in chunk n / ChunkSize at offset n % ChunkSize,
// so a range is a contiguous range of objects and a seek costs only the objects
// it lands in. False when the range does not fit the file.
//
// This is the property content-defined chunking would break (§12.1): CDC would
// need per-chunk lengths in the manifest to answer the same question.
bool chunkRange(const Manifest::File &file, quint64 offset, quint64 length,
                int *first, int *count);

// Resolves a bundle-relative path to a file or a directory, decoding manifests
// only. The path has no leading or trailing slash and its segments are already
// percent-decoded; an empty path is the bundle root, which is a directory.
//
//   * `found` false means the path names nothing in the manifests — a 404, and
//     also what "..", "." and a NUL produce, so a hostile path cannot climb out.
//   * A non-empty `error` means the question could not be answered at all: a
//     manifest is missing or corrupt, so the bundle is not all here yet — a 503,
//     which a caller must not confuse with "no such path".
bool resolve(ObjectStore *store, const QByteArray &key, const QByteArray &rootId,
             const QString &path, bool *found, bool *isDir, QByteArray *hash,
             Manifest::File *file, QString *error);

} // namespace Web
} // namespace Kelp

#endif // KELP_WEB_H
