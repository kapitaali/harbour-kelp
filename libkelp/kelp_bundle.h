#ifndef KELP_BUNDLE_H
#define KELP_BUNDLE_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include "kelp_store.h"

namespace Kelp {

/*
 * A bundle is a directory manifest (the root) plus everything it reaches.
 * Every bundle root is a directory, even for a single file, so the root is
 * stable and always carries a name.
 */

// What `open()` gives back: the whole tree, in memory.
struct TreeNode {
    QString name;                 // entry name; the root's name is not stored
    bool isDir;
    QByteArray data;              // file content
    QVector<TreeNode> children;   // directories only

    TreeNode() : isDir(false) {}
};

// What `seal()` is given: names, sizes and where the content lives. File
// content is deliberately *not* held here — a shared file can be a gigabyte,
// and the sealer only ever needs one chunk of it at a time.
struct SourceNode {
    QString name;
    bool isDir;
    QString path;                 // files: the key handed to the ContentSource
    qint64 size;                  // files: the exact size in bytes
    QVector<SourceNode> children; // directories only

    SourceNode() : isDir(false), size(0) {}
};

// Supplies file content on demand. Implementations read `length` bytes at
// `offset` from the file identified by `path`, and must return exactly that
// many bytes or fail.
class ContentSource
{
public:
    virtual ~ContentSource() {}
    virtual bool read(const QString &path, qint64 offset, qint64 length,
                      QByteArray *out, QString *error) = 0;
};

struct SealedBundle {
    QByteArray rootId;            // bundle_id: object id of the root manifest
    int objectCount;
    int fileCount;
    quint64 totalBytes;           // total plaintext size of the tree

    SealedBundle() : objectCount(0), fileCount(0), totalBytes(0) {}
};

// Derived from the link's master secret:
//   content_key = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/content")
//   swarm_token = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/swarm")
//   relay_token = HKDF-SHA256(secret, salt=32 zero bytes, info="kelp/v1/relay")
//
// They are independent: a relay that is given relay_token learns nothing about
// the content key or the swarm token. That is the point of the third one — the
// relay gates on a credential of its own, and never holds the swarm token.
QByteArray contentKey(const QByteArray &secret);
QByteArray swarmToken(const QByteArray &secret);
QByteArray relayToken(const QByteArray &secret);

// Seals `root` — which must be a directory — into objects and writes them to
// `store`. Object ids are content addresses, so the same tree seals to
// different ids every time (fresh nonces) but always opens to the same bytes.
bool seal(ObjectStore *store, const QByteArray &key, const SourceNode &root,
          ContentSource *source, SealedBundle *result, QString *error);

// Fetches, verifies (id), decrypts and reassembles the tree rooted at `rootId`.
bool open(ObjectStore *store, const QByteArray &key,
          const QByteArray &rootId, TreeNode *root, QString *error);

// Walks as much of the bundle as `store` can already answer and lists the object
// ids that are not there yet — the fetch list, in traversal order, without
// duplicates. A receiver loops: ask a peer for these, store them, walk again,
// until nothing is missing, then `open()`.
//
// A missing object is the normal case, so this returns true with the ids in
// `missing`. An object that is *here* but cannot be used is not: it does not
// decrypt with `key` (this link's key is not the sender's) or it is not a
// manifest at all, and asking for it again would change nothing. Stores that
// verify what they hand back — as the on-disk store does — turn a damaged
// object into a missing one, so a re-fetch heals it.
bool missingObjects(ObjectStore *store, const QByteArray &key,
                    const QByteArray &rootId, QVector<QByteArray> *missing,
                    QString *error);

// Every object the bundle rooted at `rootId` is made of: the root manifest, the
// manifests below it, and the data objects they name — unique, in traversal order.
//
// Strict about manifests: each one must be in the store and must open with `key`,
// because what lies under an unreadable manifest is unknown, and a caller using
// this list to decide which objects are *not* referenced would otherwise throw
// live objects away. Data objects are only named by their manifests, never read,
// so a bundle whose data has holes still enumerates completely.
bool bundleObjects(ObjectStore *store, const QByteArray &key,
                   const QByteArray &rootId, QVector<QByteArray> *ids, QString *error);

// The receiver's rule for everything that arrives over the network: the id is
// the promise the sender made, so the bytes are stored only if they hash to the
// id they were asked for. On a mismatch nothing is written and this is false —
// a peer cannot plant an object under someone else's name, and a garbled
// download leaves no trace in the store.
bool storeReceived(ObjectStore *store, const QByteArray &expectedId,
                   const QByteArray &objectBytes);

} // namespace Kelp

#endif // KELP_BUNDLE_H
