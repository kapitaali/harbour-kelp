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
};

class MemoryStore : public ObjectStore
{
public:
    bool put(const QByteArray &objectBytes, QByteArray *id) override;
    bool get(const QByteArray &id, QByteArray *objectBytes) override;
    bool contains(const QByteArray &id) override;

    int count() const { return m_objects.size(); }
    qint64 byteCount() const;

    // A seam for tests that need a store to have *lost* an object.
    bool remove(const QByteArray &id) { return m_objects.remove(id) > 0; }

private:
    QHash<QByteArray, QByteArray> m_objects;
};

} // namespace Kelp

#endif // KELP_STORE_H
