#include "kelpstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include "kelp_crypto.h"

KelpObjectStore::KelpObjectStore(const QString &rootPath)
    : m_root(rootPath)
{
}

bool KelpObjectStore::initialise(QString *error)
{
    if (QDir().mkpath(m_root))
        return true;
    if (error)
        *error = QStringLiteral("cannot create %1").arg(m_root);
    return false;
}

QString KelpObjectStore::pathForId(const QByteArray &id) const
{
    if (id.size() != Kelp::Crypto::Sha256Size)
        return QString();
    const QString hex = QString::fromLatin1(id.toHex());
    return m_root + QLatin1Char('/') + hex.left(2) + QLatin1Char('/') + hex;
}

bool KelpObjectStore::put(const QByteArray &objectBytes, QByteArray *id)
{
    const QByteArray objectId = Kelp::Crypto::sha256(objectBytes);
    if (objectId.size() != Kelp::Crypto::Sha256Size)
        return false;
    if (id)
        *id = objectId;

    const QString path = pathForId(objectId);
    if (path.isEmpty())
        return false;
    if (QFile::exists(path))
        return true;                       // same address, so the same bytes

    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    const QString temporary = path + QStringLiteral(".tmp");
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const qint64 written = file.write(objectBytes);
    file.close();
    if (written != qint64(objectBytes.size()) || !QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        return false;
    }
    return true;
}

bool KelpObjectStore::get(const QByteArray &id, QByteArray *objectBytes)
{
    const QString path = pathForId(id);
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray object = file.readAll();

    // Never hand back an object that does not hash to the id it was filed under.
    if (Kelp::Crypto::sha256(object) != id)
        return false;

    if (objectBytes)
        *objectBytes = object;
    return true;
}

bool KelpObjectStore::contains(const QByteArray &id)
{
    const QString path = pathForId(id);
    return !path.isEmpty() && QFile::exists(path);
}

int KelpObjectStore::objectCount() const
{
    int count = 0;
    const QDir root(m_root);
    const QStringList shards = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (int i = 0; i < shards.size(); ++i) {
        const QDir shard(root.filePath(shards.at(i)));
        count += shard.entryList(QDir::Files).size();
    }
    return count;
}

qint64 KelpObjectStore::diskUsage() const
{
    qint64 total = 0;
    const QDir root(m_root);
    const QStringList shards = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (int i = 0; i < shards.size(); ++i) {
        const QDir shard(root.filePath(shards.at(i)));
        const QStringList files = shard.entryList(QDir::Files);
        for (int j = 0; j < files.size(); ++j)
            total += QFileInfo(shard.filePath(files.at(j))).size();
    }
    return total;
}

bool KelpFileSource::read(const QString &path, qint64 offset, qint64 length,
                          QByteArray *out, QString *error)
{
    if (offset < 0 || length < 0) {
        if (error)
            *error = QStringLiteral("refusing to read a negative range");
        return false;
    }

    if (m_file.isNull() || m_openPath != path) {
        m_file.reset(new QFile(path));
        m_openPath.clear();
        if (!m_file->open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("cannot read %1: %2").arg(path, m_file->errorString());
            m_file.reset();
            return false;
        }
        m_openPath = path;
    }

    if (!m_file->seek(offset)) {
        if (error)
            *error = QStringLiteral("cannot seek in %1").arg(path);
        return false;
    }

    *out = m_file->read(length);
    if (out->size() != length) {
        if (error)
            *error = QStringLiteral("%1 changed size while it was being sealed").arg(path);
        return false;
    }
    return true;
}
