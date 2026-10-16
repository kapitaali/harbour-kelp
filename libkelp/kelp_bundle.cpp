#include "kelp_bundle.h"

#include <algorithm>

#include <QSet>

#include "kelp_crypto.h"
#include "kelp_manifest.h"
#include "kelp_object.h"

namespace Kelp {

namespace {

const int kMaxTreeDepth = 128;

bool sourceLessThan(const SourceNode &a, const SourceNode &b)
{
    return a.name.toUtf8() < b.name.toUtf8();
}

class Builder
{
public:
    Builder(ObjectStore *store, const QByteArray &key, ContentSource *source)
        : m_store(store), m_key(key), m_source(source), m_count(0), m_files(0) {}

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    int count() const { return m_count; }
    int files() const { return m_files; }

    QByteArray build(const SourceNode &node, quint64 *size)
    {
        return node.isDir ? buildDirectory(node, size) : buildFile(node, size);
    }

private:
    void fail(const QString &message)
    {
        if (m_error.isEmpty())
            m_error = message;
    }

    QByteArray sealObject(const QByteArray &plaintext)
    {
        bool sealed = false;
        const QByteArray object = Object::sealRandom(m_key, plaintext, &sealed);
        if (!sealed) {
            fail(QStringLiteral("failed to seal an object"));
            return QByteArray();
        }
        QByteArray id;
        if (!m_store->put(object, &id)) {
            fail(QStringLiteral("failed to write an object to the store"));
            return QByteArray();
        }
        ++m_count;
        return id;
    }

    QByteArray buildFile(const SourceNode &node, quint64 *size)
    {
        if (node.size < 0) {
            fail(QStringLiteral("negative file size"));
            return QByteArray();
        }

        QVector<QByteArray> chunks;
        for (qint64 offset = 0; offset < node.size; offset += Manifest::ChunkSize) {
            const int length = int(qMin<qint64>(Manifest::ChunkSize, node.size - offset));
            if (!m_source) {
                fail(QStringLiteral("no content source for \"%1\"").arg(node.name));
                return QByteArray();
            }
            QByteArray plaintext;
            if (!m_source->read(node.path, offset, length, &plaintext, &m_error)) {
                if (m_error.isEmpty())
                    m_error = QStringLiteral("failed to read \"%1\"").arg(node.name);
                return QByteArray();
            }
            if (plaintext.size() != length) {
                fail(QStringLiteral("\"%1\" changed size while it was being sealed")
                     .arg(node.name));
                return QByteArray();
            }

            const QByteArray id = sealObject(plaintext);
            if (!ok())
                return QByteArray();
            chunks.append(id);
        }

        Manifest::File file;
        file.size = quint64(node.size);
        file.chunks = chunks;
        if (size)
            *size = quint64(node.size);
        ++m_files;
        return sealObject(Manifest::encodeFile(file));
    }

    QByteArray buildDirectory(const SourceNode &node, quint64 *size)
    {
        QVector<SourceNode> children = node.children;
        std::sort(children.begin(), children.end(), sourceLessThan);

        QVector<Manifest::Entry> entries;
        quint64 total = 0;
        for (int i = 0; i < children.size(); ++i) {
            const QString name = Manifest::normalizeName(children.at(i).name);
            if (!Manifest::isValidName(name)) {
                fail(QStringLiteral("invalid entry name \"%1\"").arg(children.at(i).name));
                return QByteArray();
            }
            if (i > 0 && name == Manifest::normalizeName(children.at(i - 1).name)) {
                fail(QStringLiteral("duplicate entry name \"%1\"").arg(name));
                return QByteArray();
            }

            quint64 childSize = 0;
            const QByteArray hash = build(children.at(i), &childSize);
            if (!ok())
                return QByteArray();

            Manifest::Entry entry;
            entry.name = name;
            entry.isDir = children.at(i).isDir;
            entry.hash = hash;
            entry.size = childSize;
            entries.append(entry);
            total += childSize;
        }

        if (size)
            *size = total;

        // A directory is either a leaf or an index over leaves, never both.
        Manifest::Directory leaf;
        leaf.entries = entries;
        const QByteArray manifest = Manifest::encodeDirectory(leaf);
        if (!manifest.isEmpty())
            return sealObject(manifest);

        QString splitError;
        const QVector<QByteArray> leaves = Manifest::encodeDirectoryLeaves(entries, &splitError);
        if (leaves.isEmpty()) {
            fail(splitError.isEmpty()
                 ? QStringLiteral("failed to encode a directory manifest")
                 : splitError);
            return QByteArray();
        }

        QVector<QByteArray> segments;
        for (int i = 0; i < leaves.size(); ++i) {
            const QByteArray id = sealObject(leaves.at(i));
            if (!ok())
                return QByteArray();
            segments.append(id);
        }

        Manifest::Directory index;
        index.segment = true;
        index.children = segments;
        return sealObject(Manifest::encodeDirectory(index));
    }

    ObjectStore *m_store;
    QByteArray m_key;
    ContentSource *m_source;
    int m_count;
    int m_files;
    QString m_error;
};

class Opener
{
public:
    Opener(ObjectStore *store, const QByteArray &key)
        : m_store(store), m_key(key) {}

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }

    bool readNode(const QByteArray &id, const QString &name, bool isDir,
                  TreeNode *out, quint64 *size, int depth)
    {
        if (depth > kMaxTreeDepth) {
            m_error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        out->name = name;
        out->isDir = isDir;

        if (isDir) {
            QVector<Manifest::Entry> entries;
            if (!readDirectoryEntries(id, &entries))
                return false;

            quint64 total = 0;
            for (int i = 0; i < entries.size(); ++i) {
                TreeNode child;
                quint64 childSize = 0;
                if (!readNode(entries.at(i).hash, entries.at(i).name, entries.at(i).isDir,
                              &child, &childSize, depth + 1))
                    return false;
                if (childSize != entries.at(i).size) {
                    m_error = QStringLiteral("\"%1\" does not match the size in its parent")
                        .arg(entries.at(i).name);
                    return false;
                }
                out->children.append(child);
                total += childSize;
            }
            *size = total;
            return true;
        }

        const QByteArray manifest = fetch(id);
        if (!ok())
            return false;

        Manifest::File file;
        if (!Manifest::decodeFile(manifest, &file, &m_error))
            return false;

        QByteArray data;
        data.reserve(int(file.size));
        for (int i = 0; i < file.chunks.size(); ++i) {
            const quint64 offset = quint64(i) * Manifest::ChunkSize;
            const quint64 remaining = file.size - offset;
            const quint64 expected = qMin<quint64>(Manifest::ChunkSize, remaining);
            const QByteArray chunk = fetch(file.chunks.at(i));
            if (!ok())
                return false;
            if (quint64(chunk.size()) != expected) {
                m_error = QStringLiteral("a chunk does not have the size the manifest promises");
                return false;
            }
            data.append(chunk);
        }
        if (quint64(data.size()) != file.size) {
            m_error = QStringLiteral("file data does not add up to the manifest size");
            return false;
        }

        out->data = data;
        *size = file.size;
        return true;
    }

private:
    bool readDirectoryEntries(const QByteArray &id, QVector<Manifest::Entry> *entries)
    {
        const QByteArray manifest = fetch(id);
        if (!ok())
            return false;

        Manifest::Directory directory;
        if (!Manifest::decodeDirectory(manifest, &directory, &m_error))
            return false;

        if (!directory.segment) {
            entries->append(directory.entries);
            return true;
        }

        for (int i = 0; i < directory.children.size(); ++i) {
            const QByteArray leafManifest = fetch(directory.children.at(i));
            if (!ok())
                return false;
            Manifest::Directory leaf;
            if (!Manifest::decodeDirectory(leafManifest, &leaf, &m_error))
                return false;
            if (leaf.segment) {
                m_error = QStringLiteral("segment index points at another segment index");
                return false;
            }
            entries->append(leaf.entries);
        }
        return true;
    }

    QByteArray fetch(const QByteArray &id)
    {
        QByteArray object;
        if (!m_store->get(id, &object)) {
            m_error = QStringLiteral("object %1 is missing or corrupt")
                .arg(QString::fromLatin1(id.toHex()));
            return QByteArray();
        }
        QByteArray plaintext;
        if (!Object::open(m_key, object, &plaintext, &m_error)) {
            if (m_error.isEmpty())
                m_error = QStringLiteral("an object failed to decrypt");
            return QByteArray();
        }
        return plaintext;
    }

    ObjectStore *m_store;
    QByteArray m_key;
    QString m_error;
};

/*
 * The fetch planner: the same walk as Opener, but it stops at the first object
 * it does not have instead of failing, and writes down what it would need. It
 * deliberately does not read data objects — only their file manifests, whose
 * chunk lists are what it is after.
 */
class Missing
{
public:
    Missing(ObjectStore *store, const QByteArray &key)
        : m_store(store), m_key(key) {}

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    QVector<QByteArray> ids() const { return m_ids; }

    bool walk(const QByteArray &rootId) { return visitDirectory(rootId, 0); }

private:
    enum ReadResult { ReadOk, ReadMissing, ReadFailed };

    void note(const QByteArray &id)
    {
        if (m_noted.contains(id))
            return;
        m_noted.insert(id);
        m_ids.append(id);
    }

    ReadResult read(const QByteArray &id, QByteArray *plaintext)
    {
        QByteArray object;
        if (!m_store->get(id, &object)) {
            note(id);
            return ReadMissing;
        }
        if (!Object::open(m_key, object, plaintext, &m_error)) {
            if (m_error.isEmpty())
                m_error = QStringLiteral("object %1 does not open with this key")
                    .arg(QString::fromLatin1(id.toHex()));
            return ReadFailed;
        }
        return ReadOk;
    }

    bool visitDirectory(const QByteArray &id, int depth)
    {
        if (depth > kMaxTreeDepth) {
            m_error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        QByteArray manifest;
        const ReadResult result = read(id, &manifest);
        if (result == ReadMissing)
            return true;                // its entries are unknown until it arrives
        if (result == ReadFailed)
            return false;

        Manifest::Directory directory;
        if (!Manifest::decodeDirectory(manifest, &directory, &m_error))
            return false;
        if (!directory.segment)
            return visitEntries(directory.entries, depth);

        for (int i = 0; i < directory.children.size(); ++i) {
            QByteArray leafManifest;
            const ReadResult leafResult = read(directory.children.at(i), &leafManifest);
            if (leafResult == ReadMissing)
                continue;
            if (leafResult == ReadFailed)
                return false;
            Manifest::Directory leaf;
            if (!Manifest::decodeDirectory(leafManifest, &leaf, &m_error))
                return false;
            if (leaf.segment) {
                m_error = QStringLiteral("segment index points at another segment index");
                return false;
            }
            if (!visitEntries(leaf.entries, depth))
                return false;
        }
        return true;
    }

    bool visitEntries(const QVector<Manifest::Entry> &entries, int depth)
    {
        for (int i = 0; i < entries.size(); ++i) {
            const bool descended = entries.at(i).isDir
                ? visitDirectory(entries.at(i).hash, depth + 1)
                : visitFile(entries.at(i).hash);
            if (!descended)
                return false;
        }
        return true;
    }

    bool visitFile(const QByteArray &id)
    {
        QByteArray manifest;
        const ReadResult result = read(id, &manifest);
        if (result == ReadMissing)
            return true;
        if (result == ReadFailed)
            return false;

        Manifest::File file;
        if (!Manifest::decodeFile(manifest, &file, &m_error))
            return false;
        for (int i = 0; i < file.chunks.size(); ++i) {
            if (!m_store->contains(file.chunks.at(i)))
                note(file.chunks.at(i));
        }
        return true;
    }

    ObjectStore *m_store;
    QByteArray m_key;
    QVector<QByteArray> m_ids;
    QSet<QByteArray> m_noted;
    QString m_error;
};

/*
 * The object-listing walk: every object the bundle rooted at rootId reaches,
 * whether or not the store holds it. Same shape as Missing, two differences: it
 * notes an id as it *visits* it rather than only when it is absent, and it refuses
 * to continue past a manifest it cannot read — the entries under a manifest are
 * unknown without it, and a caller using this list to decide what is unreferenced
 * would otherwise throw away live objects.
 */
class Reach
{
public:
    Reach(ObjectStore *store, const QByteArray &key)
        : m_store(store), m_key(key) {}

    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    QVector<QByteArray> ids() const { return m_ids; }

    bool walk(const QByteArray &rootId) { return visitManifest(rootId, 0); }

private:
    void note(const QByteArray &id)
    {
        if (m_noted.contains(id))
            return;
        m_noted.insert(id);
        m_ids.append(id);
    }

    QByteArray read(const QByteArray &id)
    {
        QByteArray object;
        if (!m_store->get(id, &object)) {
            m_error = QStringLiteral("object %1 is missing from the store")
                .arg(QString::fromLatin1(id.toHex()));
            return QByteArray();
        }
        QByteArray plaintext;
        if (!Object::open(m_key, object, &plaintext, &m_error)) {
            if (m_error.isEmpty())
                m_error = QStringLiteral("object %1 does not open with this key")
                    .arg(QString::fromLatin1(id.toHex()));
            return QByteArray();
        }
        return plaintext;
    }

    bool visitManifest(const QByteArray &id, int depth)
    {
        if (depth > kMaxTreeDepth) {
            m_error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        note(id);
        const QByteArray manifest = read(id);
        if (!ok())
            return false;

        Manifest::Directory directory;
        if (!Manifest::decodeDirectory(manifest, &directory, &m_error))
            return false;
        if (!directory.segment)
            return visitEntries(directory.entries, depth);

        for (int i = 0; i < directory.children.size(); ++i) {
            note(directory.children.at(i));
            const QByteArray leafManifest = read(directory.children.at(i));
            if (!ok())
                return false;
            Manifest::Directory leaf;
            if (!Manifest::decodeDirectory(leafManifest, &leaf, &m_error))
                return false;
            if (leaf.segment) {
                m_error = QStringLiteral("segment index points at another segment index");
                return false;
            }
            if (!visitEntries(leaf.entries, depth))
                return false;
        }
        return true;
    }

    bool visitEntries(const QVector<Manifest::Entry> &entries, int depth)
    {
        for (int i = 0; i < entries.size(); ++i) {
            const bool descended = entries.at(i).isDir
                ? visitManifest(entries.at(i).hash, depth + 1)
                : visitFile(entries.at(i).hash);
            if (!descended)
                return false;
        }
        return true;
    }

    bool visitFile(const QByteArray &id)
    {
        note(id);
        const QByteArray manifest = read(id);
        if (!ok())
            return false;

        // The chunks are named, not read: a bundle whose data has holes still
        // lists every object it is made of.
        Manifest::File file;
        if (!Manifest::decodeFile(manifest, &file, &m_error))
            return false;
        for (int i = 0; i < file.chunks.size(); ++i)
            note(file.chunks.at(i));
        return true;
    }

    ObjectStore *m_store;
    QByteArray m_key;
    QVector<QByteArray> m_ids;
    QSet<QByteArray> m_noted;
    QString m_error;
};

} // namespace

QByteArray contentKey(const QByteArray &secret)
{
    QByteArray out;
    if (!Crypto::hkdfSha256(secret, QByteArray(Crypto::Sha256Size, '\0'),
                            QByteArrayLiteral("kelp/v1/content"), Crypto::KeySize, &out))
        return QByteArray();
    return out;
}

QByteArray swarmToken(const QByteArray &secret)
{
    QByteArray out;
    if (!Crypto::hkdfSha256(secret, QByteArray(Crypto::Sha256Size, '\0'),
                            QByteArrayLiteral("kelp/v1/swarm"), Crypto::KeySize, &out))
        return QByteArray();
    return out;
}

QByteArray relayToken(const QByteArray &secret)
{
    QByteArray out;
    if (!Crypto::hkdfSha256(secret, QByteArray(Crypto::Sha256Size, '\0'),
                            QByteArrayLiteral("kelp/v1/relay"), Crypto::KeySize, &out))
        return QByteArray();
    return out;
}

bool seal(ObjectStore *store, const QByteArray &key, const SourceNode &root,
          ContentSource *source, SealedBundle *result, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!store) {
        *error = QStringLiteral("no object store");
        return false;
    }
    if (!root.isDir) {
        *error = QStringLiteral("a bundle root must be a directory");
        return false;
    }

    Builder builder(store, key, source);
    quint64 size = 0;
    const QByteArray rootId = builder.build(root, &size);
    if (!builder.ok()) {
        *error = builder.error();
        return false;
    }

    if (result) {
        result->rootId = rootId;
        result->objectCount = builder.count();
        result->fileCount = builder.files();
        result->totalBytes = size;
    }
    return true;
}

bool open(ObjectStore *store, const QByteArray &key,
          const QByteArray &rootId, TreeNode *root, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!store || !root) {
        *error = QStringLiteral("no object store or destination");
        return false;
    }
    if (rootId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id is not a 32-byte object id");
        return false;
    }

    Opener opener(store, key);
    TreeNode node;
    quint64 size = 0;
    if (!opener.readNode(rootId, QString(), true, &node, &size, 0)) {
        *error = opener.error();
        return false;
    }

    *root = node;
    return true;
}

bool missingObjects(ObjectStore *store, const QByteArray &key,
                    const QByteArray &rootId, QVector<QByteArray> *missing,
                    QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (missing)
        missing->clear();
    if (!store || !missing) {
        *error = QStringLiteral("no object store or destination");
        return false;
    }
    if (rootId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id is not a 32-byte object id");
        return false;
    }

    Missing walker(store, key);
    if (!walker.walk(rootId)) {
        *error = walker.error();
        return false;
    }

    *missing = walker.ids();
    return true;
}

bool bundleObjects(ObjectStore *store, const QByteArray &key,
                   const QByteArray &rootId, QVector<QByteArray> *ids, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (ids)
        ids->clear();
    if (!store || !ids) {
        *error = QStringLiteral("no object store or destination");
        return false;
    }
    if (rootId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id is not a 32-byte object id");
        return false;
    }

    Reach walker(store, key);
    if (!walker.walk(rootId)) {
        *error = walker.error();
        return false;
    }

    *ids = walker.ids();
    return true;
}

bool storeReceived(ObjectStore *store, const QByteArray &expectedId,
                   const QByteArray &objectBytes)
{
    if (!store || expectedId.size() != Crypto::Sha256Size)
        return false;
    if (Object::id(objectBytes) != expectedId)
        return false;

    QByteArray id;
    return store->put(objectBytes, &id) && id == expectedId;
}

} // namespace Kelp
