#include "kelpchannels.h"

#include <stdio.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "kelp_channel.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelplog.h"
#include "kelpstore.h"

namespace {

// A missing file is an empty list; an existing but unreadable one is an error.
// The callers that write (create, publish) must not treat "could not read" as
// "there was nothing there", or they would overwrite a channel list that a
// transient problem hid.
QJsonArray readArray(const QString &path, QString *error)
{
    if (!QFile::exists(path))
        return QJsonArray();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(path, file.errorString());
        return QJsonArray();
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        *error = QStringLiteral("%1 is not a readable channel file: %2")
                     .arg(path, parseError.errorString());
        return QJsonArray();
    }
    return document.array();
}

// Owner-only, and atomic: the data goes to a temporary file that is tightened
// before anything is written, then renamed over the old one. A reader therefore
// never sees a half-written file, and a crash never leaves one in place.
bool writeArray(const QString &path, const QJsonArray &array, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }

    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Indented);
    const QString temporary = path + QStringLiteral(".tmp");
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("cannot write %1: %2").arg(temporary, file.errorString());
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const qint64 written = file.write(data);
    file.close();

    if (written != qint64(data.size())) {
        QFile::remove(temporary);
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }

    // POSIX rename replaces the destination atomically; QFile::rename refuses
    // to overwrite, which would make every update after the first one fail.
    const QByteArray from = QFile::encodeName(temporary);
    const QByteArray to = QFile::encodeName(path);
    if (::rename(from.constData(), to.constData()) != 0) {
        QFile::remove(temporary);
        *error = QStringLiteral("cannot replace %1").arg(path);
        return false;
    }
    return true;
}

bool decodeBytes(const QString &text, int size, QByteArray *out)
{
    const QByteArray bytes = QByteArray::fromHex(text.toLatin1());
    if (bytes.size() != size)
        return false;
    *out = bytes;
    return true;
}

// Bundle and channel ids travel through QML as base32, the same text a link
// names. Canonicalising here means an upper-case paste still matches.
bool decodeBase32Id(const QString &text, int size, QByteArray *out, QString *error)
{
    if (!Kelp::Link::base32Decode(text.trimmed(), out, error))
        return false;
    if (out->size() != size) {
        *error = QStringLiteral("id is not %1 bytes").arg(size);
        return false;
    }
    return true;
}

bool decodeBundleId(const QString &text, QByteArray *out, QString *error)
{
    return decodeBase32Id(text, Kelp::Crypto::Sha256Size, out, error);
}

QString canonicalChannelId(const QString &text, QByteArray *id, QString *error)
{
    if (!decodeBase32Id(text, Kelp::Channel::IdSize, id, error))
        return QString();
    return QString::fromLatin1(Kelp::Link::base32Encode(*id));
}

bool decodeChannelId(const QString &text, QByteArray *id, QString *error)
{
    QByteArray raw;
    if (!decodeBase32Id(text, Kelp::Channel::IdSize, &raw, error))
        return false;
    if (id)
        *id = raw;
    return true;
}

QVariantMap failure(const QString &error)
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("error"), error);
    return result;
}

QVariantMap success(const QVariantMap &fields)
{
    QVariantMap result = fields;
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    return result;
}

} // namespace

KelpChannels::KelpChannels(KelpObjectStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
}

QString KelpChannels::filePath()
{
    return KelpLog::dataDirectory() + QStringLiteral("/channels.json");
}

QString KelpChannels::keysFilePath()
{
    return KelpLog::dataDirectory() + QStringLiteral("/channel-keys.json");
}

QVector<KelpChannels::Entry> KelpChannels::loadEntries(QString *error) const
{
    QString scratch;
    if (!error)
        error = &scratch;

    QVector<Entry> entries;
    const QJsonArray array = readArray(filePath(), error);
    if (!error->isEmpty())
        return entries;

    // One malformed row is treated like an unreadable file, not skipped: a
    // writer that ignored it would erase that channel the next time it saved.
    const auto malformed = [&](int index) {
        *error = QStringLiteral("%1 has a malformed row at index %2")
                     .arg(filePath()).arg(index);
        return QVector<Entry>();
    };

    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject())
            return malformed(i);
        const QJsonObject object = array.at(i).toObject();

        QByteArray rawId;
        if (!decodeChannelId(object.value(QStringLiteral("id")).toString(), &rawId, 0))
            return malformed(i);

        Entry entry;
        entry.id = QString::fromLatin1(Kelp::Link::base32Encode(rawId));
        entry.name = object.value(QStringLiteral("name")).toString();
        if (entry.name.isEmpty())
            return malformed(i);
        if (!decodeBytes(object.value(QStringLiteral("publicKey")).toString(),
                         Kelp::Channel::PublicKeySize, &entry.publicKey))
            return malformed(i);
        if (!decodeBytes(object.value(QStringLiteral("secret")).toString(),
                         Kelp::Channel::ReadSecretSize, &entry.secret))
            return malformed(i);

        QByteArray rawBundle;
        QString why;
        if (!decodeBundleId(object.value(QStringLiteral("bundleId")).toString(),
                            &rawBundle, &why))
            return malformed(i);
        entry.bundleId = QString::fromLatin1(Kelp::Link::base32Encode(rawBundle));

        entry.sequence = quint64(object.value(QStringLiteral("sequence")).toDouble());
        entry.timestamp = qint64(object.value(QStringLiteral("timestamp")).toDouble());
        if (entry.sequence < 1 || entry.timestamp < 0)
            return malformed(i);

        const QJsonArray pointers = object.value(QStringLiteral("pointers")).toArray();
        for (int j = 0; j < pointers.size() && entry.pointers.size() < 5; ++j) {
            QByteArray pointer;
            if (!decodeBytes(pointers.at(j).toString(), Kelp::Crypto::Sha256Size, &pointer))
                return malformed(i);
            entry.pointers.append(QString::fromLatin1(pointer.toHex()));
        }

        entries.append(entry);
    }
    return entries;
}

bool KelpChannels::saveEntries(const QVector<Entry> &entries, QString *error) const
{
    QJsonArray array;
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries.at(i);
        QJsonArray pointers;
        for (int j = 0; j < entry.pointers.size(); ++j)
            pointers.append(entry.pointers.at(j));

        QJsonObject object;
        object.insert(QStringLiteral("id"), entry.id);
        object.insert(QStringLiteral("name"), entry.name);
        object.insert(QStringLiteral("publicKey"),
                      QString::fromLatin1(entry.publicKey.toHex()));
        object.insert(QStringLiteral("secret"), QString::fromLatin1(entry.secret.toHex()));
        object.insert(QStringLiteral("bundleId"), entry.bundleId);
        object.insert(QStringLiteral("sequence"), double(entry.sequence));
        object.insert(QStringLiteral("timestamp"), double(entry.timestamp));
        object.insert(QStringLiteral("pointers"), pointers);
        array.append(object);
    }
    return writeArray(filePath(), array, error);
}

QHash<QString, QByteArray> KelpChannels::loadKeys(QString *error) const
{
    QString scratch;
    if (!error)
        error = &scratch;

    QHash<QString, QByteArray> keys;
    const QJsonArray array = readArray(keysFilePath(), error);
    if (!error->isEmpty())
        return keys;

    const auto malformed = [&](int index) {
        *error = QStringLiteral("%1 has a malformed row at index %2")
                     .arg(keysFilePath()).arg(index);
        return QHash<QString, QByteArray>();
    };

    for (int i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject())
            return malformed(i);
        const QJsonObject object = array.at(i).toObject();

        QByteArray rawId;
        if (!decodeChannelId(object.value(QStringLiteral("id")).toString(), &rawId, 0))
            return malformed(i);
        QByteArray privateKey;
        if (!decodeBytes(object.value(QStringLiteral("privateKey")).toString(),
                         Kelp::Channel::PrivateKeySize, &privateKey))
            return malformed(i);

        keys.insert(QString::fromLatin1(Kelp::Link::base32Encode(rawId)), privateKey);
    }
    return keys;
}

bool KelpChannels::saveKeys(const QHash<QString, QByteArray> &keys, QString *error) const
{
    QJsonArray array;
    for (QHash<QString, QByteArray>::const_iterator it = keys.constBegin();
         it != keys.constEnd(); ++it) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), it.key());
        object.insert(QStringLiteral("privateKey"), QString::fromLatin1(it.value().toHex()));
        array.append(object);
    }
    return writeArray(keysFilePath(), array, error);
}

QVariantList KelpChannels::load() const
{
    QString error;
    const QVector<Entry> entries = loadEntries(&error);
    if (!error.isEmpty()) {
        qWarning() << "KelpChannels:" << error;
        return QVariantList();
    }

    QString keyError;
    const QHash<QString, QByteArray> keys = loadKeys(&keyError);
    if (!keyError.isEmpty())
        qWarning() << "KelpChannels:" << keyError;

    QVariantList rows;
    for (int i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries.at(i);
        QVariantMap row;
        row.insert(QStringLiteral("id"), entry.id);
        row.insert(QStringLiteral("name"), entry.name);
        row.insert(QStringLiteral("bundleId"), entry.bundleId);
        row.insert(QStringLiteral("sequence"), entry.sequence);
        row.insert(QStringLiteral("timestamp"), entry.timestamp);
        row.insert(QStringLiteral("pointer"), entry.pointers.value(0));
        row.insert(QStringLiteral("hasKey"), keys.contains(entry.id));
        rows.append(row);
    }
    return rows;
}

QVariantMap KelpChannels::create(const QString &name, const QString &bundleId)
{
    if (!m_store)
        return failure(QStringLiteral("the object store is unavailable"));

    const QString channelName = name.trimmed();
    if (channelName.isEmpty())
        return failure(QStringLiteral("a channel needs a name"));

    QByteArray rawBundle;
    QString error;
    if (!decodeBundleId(bundleId, &rawBundle, &error))
        return failure(QStringLiteral("bundle id: %1").arg(error));
    if (!m_store->contains(rawBundle))
        return failure(QStringLiteral("that bundle is not in this device's store"));

    QVector<Entry> entries = loadEntries(&error);
    if (!error.isEmpty())
        return failure(error);

    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).name.compare(channelName, Qt::CaseInsensitive) == 0)
            return failure(QStringLiteral("a channel named \"%1\" already exists")
                               .arg(channelName));
    }

    Kelp::Channel::KeyPair keys;
    if (!Kelp::Channel::generateKeyPair(&keys, &error))
        return failure(error);
    const QByteArray readSecret = Kelp::Crypto::randomBytes(Kelp::Channel::ReadSecretSize);
    if (readSecret.size() != Kelp::Channel::ReadSecretSize)
        return failure(QStringLiteral("could not generate a read secret"));

    Kelp::Channel::Pointer pointer;
    pointer.name = channelName;
    pointer.bundleId = rawBundle;
    pointer.sequence = 1;
    pointer.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000;
    if (!Kelp::Channel::sign(&pointer, keys.privateKey, &error))
        return failure(error);

    bool sealed = false;
    const QByteArray objectBytes = Kelp::Channel::seal(pointer, readSecret, &sealed);
    if (!sealed)
        return failure(QStringLiteral("could not seal the channel pointer"));

    QByteArray objectId;
    if (!m_store->put(objectBytes, &objectId))
        return failure(QStringLiteral("could not store the channel pointer"));

    QHash<QString, QByteArray> keyStore = loadKeys(&error);
    if (!error.isEmpty())
        return failure(error);
    keyStore.insert(QString::fromLatin1(Kelp::Link::base32Encode(Kelp::Channel::id(keys.publicKey))),
                    keys.privateKey);

    Entry entry;
    entry.id = QString::fromLatin1(
        Kelp::Link::base32Encode(Kelp::Channel::id(keys.publicKey)));
    entry.name = channelName;
    entry.publicKey = keys.publicKey;
    entry.secret = readSecret;
    entry.bundleId = QString::fromLatin1(Kelp::Link::base32Encode(rawBundle));
    entry.sequence = pointer.sequence;
    entry.timestamp = pointer.timestamp;
    entry.pointers.append(QString::fromLatin1(objectId.toHex()));

    if (!saveKeys(keyStore, &error))
        return failure(error);

    entries.append(entry);
    if (!saveEntries(entries, &error)) {
        // The key has no channel to belong to now; leave no orphan that looks
        // like a working identity.
        keyStore.remove(entry.id);
        QString ignored;
        saveKeys(keyStore, &ignored);
        return failure(error);
    }

    QVariantMap fields;
    fields.insert(QStringLiteral("id"), entry.id);
    fields.insert(QStringLiteral("name"), entry.name);
    fields.insert(QStringLiteral("bundleId"), entry.bundleId);
    fields.insert(QStringLiteral("sequence"), entry.sequence);
    fields.insert(QStringLiteral("timestamp"), entry.timestamp);
    fields.insert(QStringLiteral("pointer"), entry.pointers.value(0));
    fields.insert(QStringLiteral("link"), link(entry.id));
    return success(fields);
}

QVariantMap KelpChannels::publish(const QString &channelId, const QString &bundleId)
{
    if (!m_store)
        return failure(QStringLiteral("the object store is unavailable"));

    QByteArray rawChannel;
    QString error;
    const QString canonical = canonicalChannelId(channelId, &rawChannel, &error);
    if (canonical.isEmpty())
        return failure(QStringLiteral("channel id: %1").arg(error));

    QByteArray rawBundle;
    if (!decodeBundleId(bundleId, &rawBundle, &error))
        return failure(QStringLiteral("bundle id: %1").arg(error));
    if (!m_store->contains(rawBundle))
        return failure(QStringLiteral("that bundle is not in this device's store"));

    QVector<Entry> entries = loadEntries(&error);
    if (!error.isEmpty())
        return failure(error);

    int index = -1;
    for (int i = 0; i < entries.size() && index < 0; ++i) {
        if (entries.at(i).id == canonical)
            index = i;
    }
    if (index < 0)
        return failure(QStringLiteral("no such channel on this device"));

    const QHash<QString, QByteArray> keyStore = loadKeys(&error);
    if (!error.isEmpty())
        return failure(error);
    const QByteArray privateKey = keyStore.value(canonical);
    if (privateKey.size() != Kelp::Channel::PrivateKeySize) {
        return failure(QStringLiteral("the signing key for this channel is missing; "
                                      "it can no longer be updated"));
    }

    Entry &entry = entries[index];
    Kelp::Channel::Pointer pointer;
    pointer.name = entry.name;
    pointer.bundleId = rawBundle;
    pointer.sequence = entry.sequence + 1;
    pointer.timestamp = QDateTime::currentMSecsSinceEpoch() / 1000;
    if (!Kelp::Channel::sign(&pointer, privateKey, &error))
        return failure(error);

    bool sealed = false;
    const QByteArray objectBytes = Kelp::Channel::seal(pointer, entry.secret, &sealed);
    if (!sealed)
        return failure(QStringLiteral("could not seal the channel pointer"));

    QByteArray objectId;
    if (!m_store->put(objectBytes, &objectId))
        return failure(QStringLiteral("could not store the channel pointer"));

    entry.bundleId = QString::fromLatin1(Kelp::Link::base32Encode(rawBundle));
    entry.sequence = pointer.sequence;
    entry.timestamp = pointer.timestamp;
    entry.pointers.prepend(QString::fromLatin1(objectId.toHex()));
    while (entry.pointers.size() > 5)
        entry.pointers.removeLast();

    if (!saveEntries(entries, &error))
        return failure(error);

    QVariantMap fields;
    fields.insert(QStringLiteral("id"), entry.id);
    fields.insert(QStringLiteral("name"), entry.name);
    fields.insert(QStringLiteral("bundleId"), entry.bundleId);
    fields.insert(QStringLiteral("sequence"), entry.sequence);
    fields.insert(QStringLiteral("timestamp"), entry.timestamp);
    fields.insert(QStringLiteral("pointer"), entry.pointers.value(0));
    fields.insert(QStringLiteral("link"), link(entry.id));
    return success(fields);
}

QString KelpChannels::link(const QString &channelId) const
{
    QByteArray rawChannel;
    QString error;
    const QString canonical = canonicalChannelId(channelId, &rawChannel, &error);
    if (canonical.isEmpty()) {
        qWarning() << "KelpChannels: cannot mint a link:" << error;
        return QString();
    }

    const QVector<Entry> entries = loadEntries(&error);
    if (!error.isEmpty()) {
        qWarning() << "KelpChannels: cannot mint a link:" << error;
        return QString();
    }

    const Entry *found = 0;
    for (int i = 0; i < entries.size() && !found; ++i) {
        if (entries.at(i).id == canonical)
            found = &entries.at(i);
    }
    if (!found) {
        qWarning() << "KelpChannels: cannot mint a link: no such channel" << canonical;
        return QString();
    }
    if (found->secret.size() != Kelp::Channel::ReadSecretSize) {
        qWarning() << "KelpChannels: cannot mint a link: the read secret for"
                   << canonical << "is missing";
        return QString();
    }

    Kelp::Channel::Link channelLink;
    channelLink.id = rawChannel;
    channelLink.readSecret = found->secret;
    channelLink.relays = m_relays;
    return Kelp::Channel::encodeLink(channelLink);
}
