#include "kelp_store.h"

#include "kelp_crypto.h"

namespace Kelp {

bool MemoryStore::put(const QByteArray &objectBytes, QByteArray *id)
{
    const QByteArray objectId = Crypto::sha256(objectBytes);
    if (objectId.size() != Crypto::Sha256Size)
        return false;
    if (id)
        *id = objectId;
    m_objects.insert(objectId, objectBytes);
    return true;
}

bool MemoryStore::get(const QByteArray &id, QByteArray *objectBytes)
{
    const QHash<QByteArray, QByteArray>::const_iterator it = m_objects.constFind(id);
    if (it == m_objects.constEnd())
        return false;
    // An object that does not hash to the id it is filed under is corrupt; the
    // caller must never receive it.
    if (Crypto::sha256(it.value()) != id)
        return false;
    if (objectBytes)
        *objectBytes = it.value();
    return true;
}

bool MemoryStore::contains(const QByteArray &id)
{
    const QHash<QByteArray, QByteArray>::const_iterator it = m_objects.constFind(id);
    return it != m_objects.constEnd() && Crypto::sha256(it.value()) == id;
}

qint64 MemoryStore::byteCount() const
{
    qint64 total = 0;
    for (QHash<QByteArray, QByteArray>::const_iterator it = m_objects.constBegin();
         it != m_objects.constEnd(); ++it)
        total += it.value().size();
    return total;
}

} // namespace Kelp
