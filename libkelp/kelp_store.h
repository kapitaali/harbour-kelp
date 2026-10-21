#ifndef KELP_STORE_H
#define KELP_STORE_H

#include <QByteArray>
#include <QHash>

namespace Kelp {

/*
 * Object storage, keyed by object_id.
 *
 * The content layer never touches the filesystem itself: it asks a store for
 * objects. On the device this will be a content-addressed directory; in the
 * tests it is memory. Implementations must key objects by object_id and must
 * verify that an object still hashes to the id it was asked for.
 */
class ObjectStore
{
public:
    virtual ~ObjectStore() {}

    virtual bool put(const QByteArray &objectBytes, QByteArray *id) = 0;
    virtual bool get(const QByteArray &id, QByteArray *objectBytes) = 0;
    virtual bool contains(const QByteArray &id) = 0;

    // Bytes this store currently holds, or -1 when it cannot say cheaply. A memory
    // store knows; the on-disk app store would have to walk its shards, and the
    // only caller that needs the number is a received page's memory ceiling.
    virtual qint64 byteCount() const { return -1; }

    // Housekeeping for a store that keeps objects in directories: drop any
    // directory that no longer holds one and answer how many went. A store
    // with no directories (memory, say) answers 0.
    virtual int removeEmptyDirectories() { return 0; }
};

class MemoryStore : public ObjectStore
{
public:
    bool put(const QByteArray &objectBytes, QByteArray *id) override;
    bool get(const QByteArray &id, QByteArray *objectBytes) override;
    bool contains(const QByteArray &id) override;
    qint64 byteCount() const override;

    int count() const { return m_objects.size(); }

    // A seam for tests that need a store to have *lost* an object.
    bool remove(const QByteArray &id) { return m_objects.remove(id) > 0; }

private:
    QHash<QByteArray, QByteArray> m_objects;
};

} // namespace Kelp

#endif // KELP_STORE_H
