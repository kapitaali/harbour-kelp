#ifndef KELPSTORE_H
#define KELPSTORE_H

#include <QByteArray>
#include <QFile>
#include <QScopedPointer>
#include <QString>

#include "kelp_bundle.h"
#include "kelp_store.h"

/*
 * The object store on disk.
 *
 * Objects live under <root>/<first byte of the id, as hex>/<id as hex>, sharded
 * so that no single directory grows without bound. Writes go through a
 * temporary file and a rename, so a killed process cannot leave a half-written
 * object behind, and an object that is not whole is never visible under its id.
 *
 * Objects are addressed by their content, so the store is self-verifying: `get`
 * re-hashes every object it returns.
 */
class KelpObjectStore : public Kelp::ObjectStore
{
public:
    explicit KelpObjectStore(const QString &rootPath);

    bool initialise(QString *error);

    bool put(const QByteArray &objectBytes, QByteArray *id) override;
    bool get(const QByteArray &id, QByteArray *objectBytes) override;
    bool contains(const QByteArray &id) override;

    QString rootPath() const { return m_root; }
    QString pathForId(const QByteArray &id) const;

    // Both walk the whole sharded tree, so only use them for the About page.
    int objectCount() const;
    qint64 diskUsage() const;

private:
    QString m_root;
};

// Reads file content off the filesystem for the sealer, keeping the last file
// open because a file is read one 256 KiB chunk at a time.
class KelpFileSource : public Kelp::ContentSource
{
public:
    KelpFileSource() {}

    bool read(const QString &path, qint64 offset, qint64 length,
              QByteArray *out, QString *error) override;

private:
    QString m_openPath;
    QScopedPointer<QFile> m_file;
};

#endif // KELPSTORE_H
