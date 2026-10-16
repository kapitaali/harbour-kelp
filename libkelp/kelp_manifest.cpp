#include "kelp_manifest.h"

#include <algorithm>

#include "kelp_cbor.h"
#include "kelp_crypto.h"

namespace Kelp {
namespace Manifest {

namespace {

typedef QPair<QByteArray, QByteArray> CborPair;

CborPair textPair(const char *key, const QString &value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeText(value));
}

CborPair uintPair(const char *key, quint64 value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeUnsigned(value));
}

CborPair bytesPair(const char *key, const QByteArray &value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), Cbor::encodeBytes(value));
}

CborPair literalPair(const char *key, const char *value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)),
                     Cbor::encodeText(QString::fromLatin1(value)));
}

bool entryLessThan(const Entry &a, const Entry &b)
{
    return a.name.toUtf8() < b.name.toUtf8();
}

QByteArray encodeEntry(const Entry &entry)
{
    QVector<CborPair> map;
    map.append(bytesPair("h", entry.hash));
    map.append(textPair("n", entry.name));
    map.append(uintPair("s", entry.size));
    map.append(literalPair("t", entry.isDir ? "d" : "f"));
    return Cbor::encodeMap(map);
}

int arrayHeaderLength(quint64 count)
{
    if (count < 24)
        return 1;
    if (count <= 0xFF)
        return 2;
    if (count <= 0xFFFF)
        return 3;
    return 5;
}

// `{"e":[...],"t":"d","v":1}` = a3 | "e" | "t" "d" | "v" 01, plus the array head.
const int kLeafOverhead = 1 + 2 + 2 + 2 + 2 + 1;

bool checkVersionAndType(const Cbor::Value &value, const char *wanted, QString *error)
{
    if (value.type != Cbor::Value::Map) {
        *error = QStringLiteral("manifest is not a CBOR map");
        return false;
    }
    const Cbor::Value *version = value.member("v");
    if (!version || version->type != Cbor::Value::Unsigned || version->unsignedValue != Version) {
        *error = QStringLiteral("unsupported manifest version");
        return false;
    }
    const Cbor::Value *type = value.member("t");
    if (!type || type->type != Cbor::Value::Text
        || type->text != QString::fromLatin1(wanted)) {
        *error = QStringLiteral("manifest has the wrong type");
        return false;
    }
    return true;
}

bool decodeEntry(const Cbor::Value &value, Entry *out, QString *error)
{
    if (value.type != Cbor::Value::Map) {
        *error = QStringLiteral("directory entry is not a CBOR map");
        return false;
    }

    const Cbor::Value *name = value.member("n");
    const Cbor::Value *type = value.member("t");
    const Cbor::Value *hash = value.member("h");
    const Cbor::Value *size = value.member("s");

    if (!name || name->type != Cbor::Value::Text || !isValidName(name->text)) {
        *error = QStringLiteral("entry has no usable 'n'");
        return false;
    }
    if (normalizeName(name->text) != name->text) {
        *error = QStringLiteral("entry name is not NFC-normalised");
        return false;
    }
    if (!type || type->type != Cbor::Value::Text
        || (type->text != QLatin1String("f") && type->text != QLatin1String("d"))) {
        *error = QStringLiteral("entry has no usable 't'");
        return false;
    }
    if (!hash || hash->type != Cbor::Value::Bytes
        || hash->bytes.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("entry hash is not a 32-byte object id");
        return false;
    }
    if (!size || size->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("entry has no unsigned 's'");
        return false;
    }

    out->name = name->text;
    out->isDir = (type->text == QLatin1String("d"));
    out->hash = hash->bytes;
    out->size = size->unsignedValue;
    return true;
}

} // namespace

bool isValidName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QChar(0)))
        return false;
    return true;
}

QString normalizeName(const QString &name)
{
    return name.normalized(QString::NormalizationForm_C);
}

QByteArray encodeFile(const File &file)
{
    QVector<QByteArray> chunks;
    chunks.reserve(file.chunks.size());
    for (int i = 0; i < file.chunks.size(); ++i)
        chunks.append(Cbor::encodeBytes(file.chunks.at(i)));

    QVector<CborPair> map;
    map.append(qMakePair(Cbor::encodeText(QStringLiteral("c")), Cbor::encodeArray(chunks)));
    if (!file.mime.isEmpty())
        map.append(textPair("mime", file.mime));
    map.append(uintPair("s", file.size));
    map.append(literalPair("t", "f"));
    map.append(uintPair("v", Version));
    return Cbor::encodeMap(map);
}

QByteArray encodeDirectory(const Directory &directory)
{
    if (directory.segment) {
        QVector<QByteArray> children;
        children.reserve(directory.children.size());
        for (int i = 0; i < directory.children.size(); ++i)
            children.append(Cbor::encodeBytes(directory.children.at(i)));

        QVector<CborPair> map;
        map.append(qMakePair(Cbor::encodeText(QStringLiteral("c")), Cbor::encodeArray(children)));
        map.append(literalPair("t", "d"));
        map.append(uintPair("v", Version));
        return Cbor::encodeMap(map);
    }

    QVector<Entry> sorted = directory.entries;
    for (int i = 0; i < sorted.size(); ++i) {
        sorted[i].name = normalizeName(sorted.at(i).name);
        if (!isValidName(sorted.at(i).name))
            return QByteArray();
    }
    std::sort(sorted.begin(), sorted.end(), entryLessThan);
    for (int i = 1; i < sorted.size(); ++i)
        if (sorted.at(i).name == sorted.at(i - 1).name)
            return QByteArray();

    QVector<QByteArray> encoded;
    encoded.reserve(sorted.size());
    for (int i = 0; i < sorted.size(); ++i)
        encoded.append(encodeEntry(sorted.at(i)));

    QVector<CborPair> map;
    map.append(qMakePair(Cbor::encodeText(QStringLiteral("e")), Cbor::encodeArray(encoded)));
    map.append(literalPair("t", "d"));
    map.append(uintPair("v", Version));

    const QByteArray leaf = Cbor::encodeMap(map);
    if (leaf.isEmpty() || leaf.size() > MaxManifestBytes)
        return QByteArray();
    return leaf;
}

QVector<QByteArray> encodeDirectoryLeaves(const QVector<Entry> &entries, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;

    QVector<QByteArray> encoded;
    encoded.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i)
        encoded.append(encodeEntry(entries.at(i)));

    QVector<QByteArray> leaves;
    int start = 0;
    while (start < entries.size()) {
        int count = 0;
        qint64 bytes = 0;
        while (start + count < entries.size()) {
            const qint64 candidate = bytes + encoded.at(start + count).size();
            if (kLeafOverhead + arrayHeaderLength(count + 1) + candidate > MaxManifestBytes)
                break;
            bytes = candidate;
            ++count;
        }
        if (count == 0) {
            *error = QStringLiteral("a single directory entry exceeds the manifest cap");
            return QVector<QByteArray>();
        }

        Directory leaf;
        for (int i = 0; i < count; ++i)
            leaf.entries.append(entries.at(start + i));

        const QByteArray manifest = encodeDirectory(leaf);
        if (manifest.isEmpty()) {
            *error = QStringLiteral("failed to encode a directory leaf");
            return QVector<QByteArray>();
        }
        leaves.append(manifest);
        start += count;
    }
    return leaves;
}

bool decodeFile(const QByteArray &cbor, File *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!out) {
        *error = QStringLiteral("no destination for the manifest");
        return false;
    }
    if (cbor.size() > MaxManifestBytes) {
        *error = QStringLiteral("manifest exceeds the 256 KiB cap");
        return false;
    }

    Cbor::Value value;
    if (!Cbor::decode(cbor, &value, error))
        return false;
    if (!checkVersionAndType(value, "f", error))
        return false;

    const Cbor::Value *size = value.member("s");
    const Cbor::Value *chunks = value.member("c");
    const Cbor::Value *mime = value.member("mime");
    if (!size || size->type != Cbor::Value::Unsigned) {
        *error = QStringLiteral("file manifest has no unsigned 's'");
        return false;
    }
    if (!chunks || chunks->type != Cbor::Value::Array) {
        *error = QStringLiteral("file manifest has no 'c' array");
        return false;
    }

    File file;
    file.size = size->unsignedValue;
    for (int i = 0; i < chunks->array.size(); ++i) {
        const Cbor::Value &chunk = chunks->array.at(i);
        if (chunk.type != Cbor::Value::Bytes || chunk.bytes.size() != Crypto::Sha256Size) {
            *error = QStringLiteral("chunk id is not a 32-byte object id");
            return false;
        }
        file.chunks.append(chunk.bytes);
    }
    if (mime) {
        if (mime->type != Cbor::Value::Text) {
            *error = QStringLiteral("'mime' is not a text string");
            return false;
        }
        file.mime = mime->text;
    }

    const quint64 expected = (file.size == 0)
        ? 0
        : (file.size + ChunkSize - 1) / ChunkSize;
    if (quint64(file.chunks.size()) != expected) {
        *error = QStringLiteral("chunk count does not match the file size");
        return false;
    }

    *out = file;
    return true;
}

bool decodeDirectory(const QByteArray &cbor, Directory *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!out) {
        *error = QStringLiteral("no destination for the manifest");
        return false;
    }
    if (cbor.size() > MaxManifestBytes) {
        *error = QStringLiteral("manifest exceeds the 256 KiB cap");
        return false;
    }

    Cbor::Value value;
    if (!Cbor::decode(cbor, &value, error))
        return false;
    if (!checkVersionAndType(value, "d", error))
        return false;

    const Cbor::Value *entries = value.member("e");
    const Cbor::Value *children = value.member("c");
    if (entries && children) {
        *error = QStringLiteral("directory manifest mixes 'e' and 'c'");
        return false;
    }

    Directory directory;
    if (children) {
        if (children->type != Cbor::Value::Array) {
            *error = QStringLiteral("'c' is not an array");
            return false;
        }
        directory.segment = true;
        for (int i = 0; i < children->array.size(); ++i) {
            const Cbor::Value &child = children->array.at(i);
            if (child.type != Cbor::Value::Bytes || child.bytes.size() != Crypto::Sha256Size) {
                *error = QStringLiteral("segment child is not a 32-byte object id");
                return false;
            }
            directory.children.append(child.bytes);
        }
        if (directory.children.isEmpty()) {
            *error = QStringLiteral("segment index is empty");
            return false;
        }
    } else if (entries) {
        if (entries->type != Cbor::Value::Array) {
            *error = QStringLiteral("'e' is not an array");
            return false;
        }
        for (int i = 0; i < entries->array.size(); ++i) {
            Entry entry;
            if (!decodeEntry(entries->array.at(i), &entry, error))
                return false;
            if (i > 0
                && entry.name.toUtf8() <= directory.entries.at(i - 1).name.toUtf8()) {
                *error = QStringLiteral("directory entries are not sorted with unique names");
                return false;
            }
            directory.entries.append(entry);
        }
    } else {
        *error = QStringLiteral("directory manifest has neither 'e' nor 'c'");
        return false;
    }

    *out = directory;
    return true;
}

} // namespace Manifest
} // namespace Kelp
