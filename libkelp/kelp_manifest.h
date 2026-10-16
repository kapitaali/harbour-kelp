#ifndef KELP_MANIFEST_H
#define KELP_MANIFEST_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace Kelp {
namespace Manifest {

/*
 * Canonical CBOR manifests (kelp-swarm-wire-spec.md §3).
 *
 *   file      { "c": [hash, ...], "mime": text?, "s": uint, "t": "f", "v": 1 }
 *   dir leaf  { "e": [entry, ...], "t": "d", "v": 1 }
 *   dir index { "c": [hash, ...], "t": "d", "v": 1 }
 *   entry     { "h": hash, "n": text, "s": uint, "t": "f" | "d" }
 *
 * Keys are ordered as shown in each schema's byte form, not as written here.
 */

enum {
    Version = 1,
    ChunkSize = 262144,           // 256 KiB, fixed-size chunking in v1
    MaxManifestBytes = 262144
};

struct Entry {
    QString name;                 // NFC-normalised on the wire
    bool isDir;
    QByteArray hash;              // object id of the child manifest
    quint64 size;                 // total subtree size

    Entry() : isDir(false), size(0) {}
};

struct File {
    quint64 size;
    QVector<QByteArray> chunks;
    QString mime;                 // optional

    File() : size(0) {}
};

struct Directory {
    bool segment;                 // true => the 'c' (index) form
    QVector<Entry> entries;       // leaf form
    QVector<QByteArray> children; // index form

    Directory() : segment(false) {}
};

// Returns an empty QByteArray when the value cannot be represented: an invalid
// or duplicate entry name, or a leaf that exceeds MaxManifestBytes.
QByteArray encodeFile(const File &file);
QByteArray encodeDirectory(const Directory &directory);

// Splits already-sorted entries into leaf-form directory manifests, each within
// MaxManifestBytes. Used when a directory is too large for one leaf.
QVector<QByteArray> encodeDirectoryLeaves(const QVector<Entry> &entries, QString *error);

bool decodeFile(const QByteArray &cbor, File *out, QString *error);
bool decodeDirectory(const QByteArray &cbor, Directory *out, QString *error);

// Rejects "", ".", ".." and anything containing '/' or a NUL.
bool isValidName(const QString &name);

QString normalizeName(const QString &name);

} // namespace Manifest
} // namespace Kelp

#endif // KELP_MANIFEST_H
