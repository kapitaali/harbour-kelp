/*
 * libkelp unit tests — the crypto and format core.
 *
 * Everything here is hermetic: no device, no network, no filesystem. The
 * vectors marked "pinned" come from outside the implementation (RFC 5869, the
 * GCM specification, and kelp-swarm-wire-spec.md §16), so they catch a
 * self-consistent but wrong implementation.
 */

#include <stdio.h>

#include <algorithm>

#include <QByteArray>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

#include "kelp_bundle.h"
#include "kelp_cbor.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelp_manifest.h"
#include "kelp_object.h"
#include "kelp_store.h"
#include "kelp_wire.h"

using namespace Kelp;

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(const QString &value) { return value; }
QString show(int value) { return QString::number(value); }
QString show(qint64 value) { return QString::number(value); }
QString show(quint64 value) { return QString::number(value); }

void section(const char *name)
{
    g_section = QString::fromLatin1(name);
    printf("\n== %s\n", name);
}

void check(bool condition, const char *expression, int line)
{
    ++g_checks;
    if (condition)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s\n", qPrintable(g_section), line, expression);
}

template <typename A, typename B>
void checkEqual(const A &actual, const B &expected, const char *expression, int line)
{
    ++g_checks;
    if (actual == expected)
        return;
    ++g_failures;
    printf("  FAIL [%s] line %d: %s\n    actual:   %s\n    expected: %s\n",
           qPrintable(g_section), line, expression,
           qPrintable(show(actual)), qPrintable(show(expected)));
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) checkEqual((actual), (expected), #actual " == " #expected, __LINE__)

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(text);
}

QByteArray testHash(const char *seed)
{
    return Crypto::sha256(QByteArray(seed));
}

QByteArray zeroHash()
{
    return QByteArray(Crypto::Sha256Size, '\0');
}

typedef QPair<QByteArray, QByteArray> CborPair;

CborPair pair(const char *key, const QByteArray &value)
{
    return qMakePair(Cbor::encodeText(QString::fromLatin1(key)), value);
}

// --- helpers for the tests ----------------------------------------------

QByteArray entryCbor(const char *name, bool isDir, const QByteArray &hash, quint64 size)
{
    QVector<CborPair> map;
    map.append(pair("h", Cbor::encodeBytes(hash)));
    map.append(pair("n", Cbor::encodeText(QString::fromLatin1(name))));
    map.append(pair("s", Cbor::encodeUnsigned(size)));
    map.append(pair("t", Cbor::encodeText(QString::fromLatin1(isDir ? "d" : "f"))));
    return Cbor::encodeMap(map);
}

QVector<TreeNode> sortedChildren(const TreeNode &node)
{
    QVector<TreeNode> children = node.children;
    std::sort(children.begin(), children.end(),
              [](const TreeNode &a, const TreeNode &b) { return a.name < b.name; });
    return children;
}

// Empty when the trees match; otherwise the location of the first difference.
QString treeDifference(const TreeNode &a, const TreeNode &b, const QString &path)
{
    if (a.isDir != b.isDir)
        return path + QStringLiteral(": file/directory mismatch");
    if (!a.isDir)
        return a.data == b.data ? QString() : path + QStringLiteral(": content mismatch");

    const QVector<TreeNode> left = sortedChildren(a);
    const QVector<TreeNode> right = sortedChildren(b);
    if (left.size() != right.size())
        return path + QStringLiteral(": child count %1 vs %2")
            .arg(left.size()).arg(right.size());

    for (int i = 0; i < left.size(); ++i) {
        if (left.at(i).name != right.at(i).name)
            return path + QStringLiteral(": name %1 vs %2")
                .arg(left.at(i).name, right.at(i).name);
        const QString difference =
            treeDifference(left.at(i), right.at(i), path + QLatin1Char('/') + left.at(i).name);
        if (!difference.isEmpty())
            return difference;
    }
    return QString();
}

// A store that returns one object damaged, to prove the id check is load
// bearing. Writes pass straight through.
class CorruptingStore : public ObjectStore
{
public:
    explicit CorruptingStore(ObjectStore *inner)
        : m_inner(inner), m_corruptAt(-1), m_reads(0) {}

    void corruptReadNumber(int read) { m_corruptAt = read; }

    bool put(const QByteArray &objectBytes, QByteArray *id) override
    { return m_inner->put(objectBytes, id); }

    bool get(const QByteArray &id, QByteArray *objectBytes) override
    {
        if (!m_inner->get(id, objectBytes))
            return false;
        if (m_reads++ == m_corruptAt && !objectBytes->isEmpty())
            (*objectBytes)[0] = char(objectBytes->at(0) ^ 0x01);
        return true;
    }

    bool contains(const QByteArray &id) override { return m_inner->contains(id); }

private:
    ObjectStore *m_inner;
    int m_corruptAt;
    int m_reads;
};

// Turns an in-memory TreeNode into the streaming shape the sealer wants, and
// serves the content back from memory. This is how the tests exercise the same
// code path the app uses to read real files.
class TreeSource : public ContentSource
{
public:
    Kelp::SourceNode build(const TreeNode &node, const QString &prefix = QString())
    {
        const QString path = prefix.isEmpty()
            ? node.name
            : prefix + QLatin1Char('/') + node.name;

        Kelp::SourceNode out;
        out.name = node.name;
        out.isDir = node.isDir;

        if (node.isDir) {
            for (int i = 0; i < node.children.size(); ++i)
                out.children.append(build(node.children.at(i), path));
        } else {
            out.path = path;
            out.size = node.data.size();
            m_content.insert(path, node.data);
        }
        return out;
    }

    bool read(const QString &path, qint64 offset, qint64 length,
              QByteArray *out, QString *error) override
    {
        if (!m_content.contains(path)) {
            *error = QStringLiteral("no such path: %1").arg(path);
            return false;
        }
        const QByteArray data = m_content.value(path);
        if (offset < 0 || length < 0 || offset + length > data.size()) {
            *error = QStringLiteral("read past the end of %1").arg(path);
            return false;
        }
        *out = data.mid(int(offset), int(length));
        return true;
    }

private:
    QHash<QString, QByteArray> m_content;
};

// The receiver's loop, exactly as the client runs it: ask the walker what is
// missing, fetch those objects from the publisher, store them, walk again. It is
// the whole download policy — nothing else decides what to ask for.
bool fetchAll(ObjectStore *from, ObjectStore *into, const QByteArray &key,
              const QByteArray &rootId, int *fetched, int *rounds, QString *error)
{
    QVector<QByteArray> missing;
    for (;;) {
        if (!missingObjects(into, key, rootId, &missing, error))
            return false;
        if (missing.isEmpty())
            return true;

        if (fetched)
            *fetched += missing.size();
        if (rounds)
            ++(*rounds);

        for (int i = 0; i < missing.size(); ++i) {
            QByteArray object;
            if (!from->get(missing.at(i), &object)) {
                *error = QStringLiteral("the publisher does not have an object it named");
                return false;
            }
            if (!storeReceived(into, missing.at(i), object)) {
                *error = QStringLiteral("an object did not hash to the id it was fetched for");
                return false;
            }
        }
    }
}

// --- tests --------------------------------------------------------------

void testCborEncoding()
{
    section("canonical CBOR encoding");

    CHECK_EQ(Cbor::encodeUnsigned(0), hex("00"));
    CHECK_EQ(Cbor::encodeUnsigned(10), hex("0a"));
    CHECK_EQ(Cbor::encodeUnsigned(23), hex("17"));
    CHECK_EQ(Cbor::encodeUnsigned(24), hex("1818"));
    CHECK_EQ(Cbor::encodeUnsigned(255), hex("18ff"));
    CHECK_EQ(Cbor::encodeUnsigned(256), hex("190100"));
    CHECK_EQ(Cbor::encodeUnsigned(65535), hex("19ffff"));
    CHECK_EQ(Cbor::encodeUnsigned(65536), hex("1a00010000"));
    CHECK_EQ(Cbor::encodeUnsigned(4294967295ULL), hex("1affffffff"));
    CHECK_EQ(Cbor::encodeUnsigned(4294967296ULL), hex("1b0000000100000000"));

    CHECK_EQ(Cbor::encodeInt(0), hex("00"));
    CHECK_EQ(Cbor::encodeInt(1000), hex("1903e8"));
    CHECK_EQ(Cbor::encodeInt(-1), hex("20"));
    CHECK_EQ(Cbor::encodeInt(-24), hex("37"));
    CHECK_EQ(Cbor::encodeInt(-25), hex("3818"));

    CHECK_EQ(Cbor::encodeBytes(QByteArray()), hex("40"));
    CHECK_EQ(Cbor::encodeBytes(hex("0102")), hex("420102"));
    CHECK_EQ(Cbor::encodeText(QString()), hex("60"));
    CHECK_EQ(Cbor::encodeText(QStringLiteral("a")), hex("6161"));
    CHECK_EQ(Cbor::encodeText(QStringLiteral("IETF")), hex("6449455446"));

    QVector<QByteArray> items;
    items << Cbor::encodeUnsigned(1) << Cbor::encodeUnsigned(2) << Cbor::encodeUnsigned(3);
    CHECK_EQ(Cbor::encodeArray(items), hex("83010203"));

    // Canonical maps sort by encoded key bytes, not by insertion order.
    QVector<CborPair> map;
    map.append(pair("v", Cbor::encodeUnsigned(1)));
    map.append(pair("t", Cbor::encodeText(QStringLiteral("d"))));
    map.append(pair("e", Cbor::encodeArray(QVector<QByteArray>())));
    CHECK_EQ(Cbor::encodeMap(map), hex("a361658061746164617601"));

    // Duplicate keys have no canonical form.
    QVector<CborPair> duplicate;
    duplicate.append(pair("e", Cbor::encodeUnsigned(1)));
    duplicate.append(pair("e", Cbor::encodeUnsigned(2)));
    CHECK(Cbor::encodeMap(duplicate).isEmpty());
}

void testManifestVectors()
{
    section("pinned manifest vectors (wire spec §16)");

    const QByteArray childHash =
        hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    Manifest::Entry entry;
    entry.name = QStringLiteral("a.txt");
    entry.isDir = false;
    entry.hash = childHash;
    entry.size = 1234;

    Manifest::Directory directory;
    directory.entries.append(entry);

    const QByteArray leaf = Manifest::encodeDirectory(directory);
    CHECK_EQ(leaf, hex("a3616581a461685820000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f616e65612e74787461731904d26174616661746164617601"));
    CHECK_EQ(Crypto::sha256(leaf), hex("a10359058a699255134d971b806a1c383a46a3a77aaa8a5d867f4b9cecfac798"));

    const QByteArray empty = Manifest::encodeDirectory(Manifest::Directory());
    CHECK_EQ(empty, hex("a361658061746164617601"));
    CHECK_EQ(Crypto::sha256(empty), hex("81edc9892795bd2983a56faeded349074809fb87f899e8a453947ae3f8f51269"));

    // The pinned vector must round-trip.
    QString error;
    Manifest::Directory decoded;
    CHECK(Manifest::decodeDirectory(leaf, &decoded, &error));
    CHECK_EQ(decoded.entries.size(), 1);
    CHECK_EQ(decoded.entries.at(0).name, QStringLiteral("a.txt"));
    CHECK_EQ(decoded.entries.at(0).hash, childHash);
    CHECK_EQ(decoded.entries.at(0).size, quint64(1234));
    CHECK(!decoded.entries.at(0).isDir);
}

void testCborDecoding()
{
    section("strict CBOR decoding");

    QString error;
    Cbor::Value value;
    CHECK(Cbor::decode(hex("a361658061746164617601"), &value, &error));
    CHECK(value.type == Cbor::Value::Map);
    const Cbor::Value *version = value.member("v");
    const Cbor::Value *type = value.member("t");
    CHECK(version != 0);
    CHECK(type != 0);
    if (version)
        CHECK_EQ(version->unsignedValue, quint64(1));
    if (type)
        CHECK_EQ(type->text, QStringLiteral("d"));

    // Every one of these is illegal in canonical CBOR.
    CHECK(!Cbor::decode(hex("1800"), &value, &error));            // non-shortest 0
    CHECK(!Cbor::decode(hex("9f"), &value, &error));              // indefinite array
    CHECK(!Cbor::decode(hex("bf"), &value, &error));              // indefinite map
    CHECK(!Cbor::decode(hex("7f"), &value, &error));              // indefinite text
    CHECK(!Cbor::decode(hex("c001"), &value, &error));            // tag
    CHECK(!Cbor::decode(hex("f90000"), &value, &error));          // float
    CHECK(!Cbor::decode(hex("0000"), &value, &error));            // trailing byte
    CHECK(!Cbor::decode(hex("61ff"), &value, &error));            // invalid UTF-8
    CHECK(!Cbor::decode(hex("82"), &value, &error));              // truncated array
    CHECK(!Cbor::decode(hex("a2617401616501"), &value, &error));  // keys out of order
    CHECK(!Cbor::decode(hex("a2616501616501"), &value, &error));  // duplicate keys
    CHECK(!Cbor::decode(hex("00"), 0, &error));                   // no destination
}

void testManifestRules()
{
    section("manifest rules");

    QString error;

    CHECK(Manifest::isValidName(QStringLiteral("a.txt")));
    CHECK(!Manifest::isValidName(QString()));
    CHECK(!Manifest::isValidName(QStringLiteral(".")));
    CHECK(!Manifest::isValidName(QStringLiteral("..")));
    CHECK(!Manifest::isValidName(QStringLiteral("a/b")));

    // NFC: "e" + U+0301 must normalise to U+00E9.
    CHECK_EQ(Manifest::normalizeName(QString::fromUtf8("\x65\xcc\x81")),
             QString::fromUtf8("\xc3\xa9"));

    // Duplicate names after normalisation are refused.
    Manifest::Directory duplicates;
    Manifest::Entry first;
    first.name = QStringLiteral("a");
    first.hash = zeroHash();
    first.size = 1;
    Manifest::Entry combining = first;
    combining.name = QString::fromUtf8("\x65\xcc\x81");   // "e" + U+0301, normalises to U+00E9
    duplicates.entries.append(combining);
    duplicates.entries.append(combining);
    CHECK(Manifest::encodeDirectory(duplicates).isEmpty());

    // Entries must be sorted by name with no duplicates.
    QVector<QByteArray> entries;
    entries << entryCbor("b", false, testHash("b"), 1)
            << entryCbor("a", false, testHash("a"), 1);
    QVector<CborPair> leaf;
    leaf.append(pair("e", Cbor::encodeArray(entries)));
    leaf.append(pair("t", Cbor::encodeText(QStringLiteral("d"))));
    leaf.append(pair("v", Cbor::encodeUnsigned(1)));

    Manifest::Directory decoded;
    CHECK(!Manifest::decodeDirectory(Cbor::encodeMap(leaf), &decoded, &error));

    // A directory is either a leaf or an index, never both.
    QVector<CborPair> mixed;
    mixed.append(pair("c", Cbor::encodeArray(QVector<QByteArray>())));
    mixed.append(pair("e", Cbor::encodeArray(QVector<QByteArray>())));
    mixed.append(pair("t", Cbor::encodeText(QStringLiteral("d"))));
    mixed.append(pair("v", Cbor::encodeUnsigned(1)));
    CHECK(!Manifest::decodeDirectory(Cbor::encodeMap(mixed), &decoded, &error));

    // The chunk count must agree with the file size.
    Manifest::File oversized;
    oversized.size = 2 * Manifest::ChunkSize + 1;   // needs three chunks
    oversized.chunks.append(testHash("only-one"));
    CHECK(!Manifest::decodeFile(Manifest::encodeFile(oversized), &oversized, &error));

    // And a well-formed file manifest round-trips.
    Manifest::File file;
    file.size = 300000;
    file.chunks.append(testHash("chunk-0"));
    file.chunks.append(testHash("chunk-1"));
    file.mime = QStringLiteral("text/plain");
    const QByteArray encoded = Manifest::encodeFile(file);
    CHECK(!encoded.isEmpty());
    Manifest::File reread;
    CHECK(Manifest::decodeFile(encoded, &reread, &error));
    CHECK_EQ(reread.size, file.size);
    CHECK_EQ(reread.chunks.size(), 2);
    CHECK_EQ(reread.chunks.at(1), file.chunks.at(1));
    CHECK_EQ(reread.mime, file.mime);
}

void testCryptoVectors()
{
    section("crypto vectors");

    CHECK_EQ(Crypto::sha256(QByteArray()),
             hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Crypto::sha256(QByteArray("abc")),
             hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    // RFC 5869, appendix A.1.
    QByteArray okm;
    CHECK(Crypto::hkdfSha256(
        hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
        hex("000102030405060708090a0b0c"),
        hex("f0f1f2f3f4f5f6f7f8f9"), 42, &okm));
    CHECK_EQ(okm, hex("3cb25f25faacd57a90434f64d0362f2a"
                      "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
                      "34007208d5b887185865"));

    // The spec's "\xe2\x88\x85 salt" is 32 zero bytes; HKDF cannot tell the
    // difference between that and an empty salt.
    QByteArray emptySalt;
    QByteArray zeroSalt;
    CHECK(Crypto::hkdfSha256(QByteArray("secret"), QByteArray(),
                             QByteArray("kelp/test"), 32, &emptySalt));
    CHECK(Crypto::hkdfSha256(QByteArray("secret"), QByteArray(32, '\0'),
                             QByteArray("kelp/test"), 32, &zeroSalt));
    CHECK_EQ(emptySalt, zeroSalt);

    // The GCM specification, test case 4 (AES-256-GCM with AAD).
    const QByteArray key =
        hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    const QByteArray nonce = hex("cafebabefacedbaddecaf888");
    const QByteArray plaintext = hex("d9313225f88406e5a55909c5aff5269a"
                                     "86a7a9531534f7da2e4c303d8a318a72"
                                     "1c3c0c95956809532fcf0e2449a6b525"
                                     "b16aedf5aa0de657ba637b39");
    const QByteArray aad = hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");

    QByteArray sealed;
    CHECK(Crypto::aes256GcmEncrypt(key, nonce, plaintext, aad, &sealed));
    CHECK_EQ(sealed, hex("522dc1f099567d07f47f37a32a84427d"
                         "643a8cdcbfe5c0c97598a2bd2555d1aa"
                         "8cb08e48590dbb3da7b08b1056828838"
                         "c5f61e6393ba7a0abcc9f662"
                         "76fc6ece0f4e1768cddf8853bb2d551b"));

    QByteArray opened;
    CHECK(Crypto::aes256GcmDecrypt(key, nonce, sealed, aad, &opened));
    CHECK_EQ(opened, plaintext);

    QByteArray corrupted = sealed;
    corrupted[0] = char(corrupted.at(0) ^ 0x01);
    CHECK(!Crypto::aes256GcmDecrypt(key, nonce, corrupted, aad, &opened));
    CHECK(!Crypto::aes256GcmDecrypt(key, nonce, sealed,
                                    hex("00feedfacedeadbeeffeedfacedeadbeefabaddad2"), &opened));
    CHECK(!Crypto::aes256GcmDecrypt(key, nonce, sealed.left(sealed.size() - 1), aad, &opened));

    // The Kelp derivations, pinned.
    const QByteArray secret = hex("000102030405060708090a0b0c0d0e0f"
                                  "101112131415161718191a1b1c1d1e1f");
    CHECK_EQ(contentKey(secret),
             hex("a08477fb99f94daa1b3ce1c21b3aba33e9e705809491239d48271eb8f53e52db"));
    CHECK_EQ(swarmToken(secret),
             hex("a4e1804d247b842766c24dc8bafe17239edacaf8532e9cc3e1fb8c32b96aaf19"));
    CHECK(contentKey(secret) != swarmToken(secret));
}

void testObjectEnvelope()
{
    section("object envelope");

    const QByteArray key = hex("000102030405060708090a0b0c0d0e0f"
                               "101112131415161718191a1b1c1d1e1f");
    const QByteArray nonce = hex("000102030405060708090a0b");
    const QByteArray plaintext("kelp object vector");

    bool ok = false;
    const QByteArray object = Object::seal(key, plaintext, nonce, &ok);
    CHECK(ok);
    CHECK_EQ(object.size(), 1 + 12 + plaintext.size() + 16);
    CHECK_EQ(object.left(1), hex("01"));
    CHECK_EQ(object.mid(1, 12), nonce);
    CHECK_EQ(Object::id(object), Crypto::sha256(object));
    CHECK_EQ(Object::id(object).size(), 32);

    // Pinned in kelp-swarm-wire-spec.md §16 (envelope version 1, this key and
    // this nonce, plaintext "kelp object vector"). The GCM primitive itself is
    // pinned above against the GCM specification's test case 4, so this vector
    // pins the envelope framing and the object id.
    CHECK_EQ(object, hex("01000102030405060708090a0b"
                         "2c67ba6be58aa071e822e3abc78c1b19eca4eb392c7113e4c96d748088a6c6825897"));
    CHECK_EQ(Object::id(object),
             hex("f3bfa0257b6d7a6a2dfe757ccac665541845d1e8d717ab58c290f0809160fd4d"));

    QString error;
    QByteArray opened;
    CHECK(Object::open(key, object, &opened, &error));
    CHECK_EQ(opened, plaintext);

    CHECK(!Object::open(Crypto::sha256(key), object, &opened, &error));

    QByteArray tampered = object;
    tampered[tampered.size() - 1] = char(tampered.at(tampered.size() - 1) ^ 0x01);
    CHECK(!Object::open(key, tampered, &opened, &error));

    QByteArray wrongVersion = object;
    wrongVersion[0] = char(2);
    CHECK(!Object::open(key, wrongVersion, &opened, &error));

    CHECK(!Object::open(key, object.left(object.size() - 1), &opened, &error));
    CHECK(!Object::open(key, hex("01") + nonce, &opened, &error));
    CHECK(Object::sealRandom(key, plaintext, &ok).size() > 0);
    CHECK(ok);
}

void testStoreContract()
{
    section("object store contract");

    MemoryStore store;
    QByteArray id;
    CHECK(store.put(QByteArray("an object"), &id));
    CHECK_EQ(id, Crypto::sha256(QByteArray("an object")));
    CHECK(store.contains(id));
    CHECK_EQ(store.count(), 1);
    CHECK_EQ(store.byteCount(), qint64(9));

    QByteArray object;
    CHECK(store.get(id, &object));
    CHECK_EQ(object, QByteArray("an object"));
    CHECK(!store.get(zeroHash(), &object));
    CHECK(!store.contains(zeroHash()));
}

// The tree every round-trip test uses: nested directories, an empty file, a
// file that spans several chunks, and deliberately unsorted children.
TreeNode sampleTree()
{
    TreeNode root;
    root.name = QStringLiteral("bundle");
    root.isDir = true;

    TreeNode small;
    small.name = QStringLiteral("small.txt");
    small.data = QByteArray("hello kelp");

    TreeNode empty;
    empty.name = QStringLiteral("empty.bin");

    QByteArray big;
    big.resize(600000);
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 7 + (i >> 8)) & 0xFF);
    TreeNode bigNode;
    bigNode.name = QStringLiteral("big.bin");
    bigNode.data = big;

    TreeNode subsub;
    subsub.name = QStringLiteral("subsub");
    subsub.isDir = true;

    TreeNode letterA;
    letterA.name = QStringLiteral("a.txt");
    letterA.data = QByteArray("a");

    TreeNode letterB;
    letterB.name = QStringLiteral("b.txt");
    letterB.data = QByteArray(100, 'b');

    TreeNode sub;
    sub.name = QStringLiteral("sub");
    sub.isDir = true;
    sub.children << subsub << letterB << letterA;

    root.children << small << empty << bigNode << sub;
    return root;
}

void testLink()
{
    section("kelp:// links");

    CHECK_EQ(Link::base32Encode(QByteArray("foobar")), QByteArray("mzxw6ytboi"));

    QByteArray range32;
    for (int i = 0; i < 32; ++i)
        range32.append(char(i));
    CHECK_EQ(Link::base32Encode(range32),
             QByteArray("aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dypq"));
    CHECK_EQ(Link::base64UrlEncode(range32),
             QStringLiteral("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"));
    CHECK_EQ(Link::base64UrlEncode(hex("fbefff")), QStringLiteral("--__"));

    QByteArray decoded;
    QString error;
    CHECK(Link::base32Decode(QStringLiteral("mzxw6ytboi"), &decoded, &error));
    CHECK_EQ(decoded, QByteArray("foobar"));
    CHECK(Link::base32Decode(QStringLiteral("MZXW6YTBOI"), &decoded, &error));  // case-insensitive
    CHECK(!Link::base32Decode(QStringLiteral("mzxw6ytbo1"), &decoded, &error)); // '1' is not base32
    CHECK(!Link::base32Decode(QStringLiteral("mzxw6ytbo"), &decoded, &error));  // non-zero pad bits
    CHECK(Link::base64UrlDecode(QStringLiteral("--__"), &decoded, &error));
    CHECK_EQ(decoded, hex("fbefff"));
    CHECK(!Link::base64UrlDecode(QStringLiteral("A"), &decoded, &error));       // dangling group
    CHECK(!Link::base64UrlDecode(QStringLiteral("!"), &decoded, &error));

    Link::Bundle bundle;
    bundle.bundleId = testHash("bundle");
    bundle.secret = testHash("secret");
    bundle.relays << QStringLiteral("https://relay.kelp.dev")
                  << QStringLiteral("https://relay2.kelp.dev:8443");
    bundle.lan = QStringLiteral("192.168.1.7:7373");
    bundle.name = QStringLiteral("Kelp samples \xc3\xa9");

    const QString link = Link::encode(bundle);
    CHECK(link.startsWith(QStringLiteral("kelp://")));
    CHECK(link.contains(QStringLiteral("#k=")));
    CHECK_EQ(link.indexOf(QLatin1Char('#')), link.lastIndexOf(QLatin1Char('#')));
    // 32 bytes of id is 52 base32 characters.
    CHECK_EQ(link.indexOf(QLatin1Char('?')), 7 + 52);

    Link::Bundle parsed;
    error.clear();
    CHECK(Link::decode(link, &parsed, &error));
    CHECK_EQ(parsed.bundleId, bundle.bundleId);
    CHECK_EQ(parsed.secret, bundle.secret);
    CHECK_EQ(parsed.name, bundle.name);
    CHECK_EQ(parsed.lan, bundle.lan);
    CHECK_EQ(parsed.relays.size(), 2);
    CHECK_EQ(parsed.relays.value(1), bundle.relays.value(1));

    // Where it is served from and what it is called are sealed, not spelled out.
    // The address's host part cannot appear by chance — base64url has no dots —
    // and the label has no plaintext form either.
    CHECK(link.contains(QStringLiteral("q=")));
    CHECK(!link.contains(bundle.lan.left(bundle.lan.indexOf(QLatin1Char(':')))));
    CHECK(!link.contains(QStringLiteral("samples")));

    // A link without a key is a name, not a capability.
    const QString id32 = QString::fromLatin1(Link::base32Encode(bundle.bundleId));
    CHECK(!Link::decode(QStringLiteral("kelp://") + id32, &parsed, &error));
    CHECK(!Link::decode(QStringLiteral("https://example.com/"), &parsed, &error));
    CHECK(!Link::decode(QStringLiteral("kelp://short#k=") + Link::base64UrlEncode(bundle.secret),
                        &parsed, &error));
    CHECK(!Link::decode(QStringLiteral("kelp://") + id32 + QStringLiteral("#k=AAAA"),
                        &parsed, &error));

    // The shape the app produces today: id, label and key, no addresses yet.
    Link::Bundle minimal;
    minimal.bundleId = testHash("minimal");
    minimal.secret = testHash("minimal-secret");
    minimal.name = QStringLiteral("photos");
    Link::Bundle reparsed;
    CHECK(Link::decode(Link::encode(minimal), &reparsed, &error));
    CHECK_EQ(reparsed.bundleId, minimal.bundleId);
    CHECK_EQ(reparsed.secret, minimal.secret);
    CHECK_EQ(reparsed.name, minimal.name);
    CHECK(reparsed.relays.isEmpty());
    CHECK(reparsed.lan.isEmpty());

    // A link minted before the address was sealed carries `l=`/`n=` in the clear
    // and still decodes — links already out there keep working forever.
    const QString legacy = QStringLiteral("kelp://") + id32
        + QStringLiteral("?l=192.168.1.7%3A7373&n=Kelp%20samples")
        + QStringLiteral("#k=") + Link::base64UrlEncode(bundle.secret);
    error.clear();
    CHECK(Link::decode(legacy, &parsed, &error));
    CHECK_EQ(parsed.lan, bundle.lan);
    CHECK_EQ(parsed.name, QStringLiteral("Kelp samples"));

    // Tampering with the sealed block is caught, not silently ignored.
    QString tampered = link;
    const int qAt = tampered.indexOf(QStringLiteral("q=")) + 2;
    tampered[qAt] = (tampered.at(qAt) == QLatin1Char('A')) ? QLatin1Char('B')
                                                           : QLatin1Char('A');
    error.clear();
    CHECK(!Link::decode(tampered, &parsed, &error));

    // The block is bound to its bundle id: the same block sewn onto another
    // link's id does not open.
    Link::Bundle other;
    other.bundleId = testHash("other-bundle");
    other.secret = bundle.secret;
    other.lan = bundle.lan;
    const QString otherLink = Link::encode(other);
    const int blobAt = otherLink.indexOf(QStringLiteral("q=")) + 2;
    const int blobEnd = otherLink.indexOf(QStringLiteral("#k="));
    const QString foreignBlob = otherLink.mid(blobAt, blobEnd - blobAt);
    error.clear();
    CHECK(!Link::decode(QStringLiteral("kelp://") + id32 + QStringLiteral("?q=")
                            + foreignBlob + QStringLiteral("#k=")
                            + Link::base64UrlEncode(bundle.secret),
                        &parsed, &error));
}

void testBundleRoundTrip()
{
    section("bundle round-trip (chunk -> manifest -> hash -> verify -> decrypt)");

    const QByteArray secret = hex("000102030405060708090a0b0c0d0e0f"
                                  "101112131415161718191a1b1c1d1e1f");
    const QByteArray key = contentKey(secret);
    const TreeNode tree = sampleTree();

    TreeSource source;
    const Kelp::SourceNode sourceTree = source.build(tree);

    MemoryStore store;
    SealedBundle sealed;
    QString error;
    CHECK(seal(&store, key, sourceTree, &source, &sealed, &error));
    if (!error.isEmpty())
        printf("  seal error: %s\n", qPrintable(error));

    // "hello kelp" (10) + empty (0) + big.bin (600000) + "a" (1) + 100 x 'b'.
    CHECK_EQ(sealed.totalBytes, quint64(600000 + 10 + 1 + 100));
    // 6 data objects, 5 file manifests, 3 directory manifests.
    CHECK_EQ(sealed.objectCount, 14);
    CHECK_EQ(sealed.fileCount, 5);
    CHECK_EQ(store.count(), 14);

    TreeNode opened;
    CHECK(open(&store, key, sealed.rootId, &opened, &error));
    if (!error.isEmpty())
        printf("  open error: %s\n", qPrintable(error));
    CHECK_EQ(treeDifference(tree, opened, QStringLiteral("bundle")), QString());
    CHECK(opened.isDir);
    CHECK_EQ(opened.name, QString());

    // A bundle is a fresh set of objects every time (fresh nonces) but always
    // opens to the same bytes.
    MemoryStore second;
    SealedBundle resealed;
    CHECK(seal(&second, key, sourceTree, &source, &resealed, &error));
    CHECK(resealed.rootId != sealed.rootId);
    TreeNode reopened;
    CHECK(open(&second, key, resealed.rootId, &reopened, &error));
    CHECK_EQ(treeDifference(tree, reopened, QStringLiteral("bundle")), QString());

    // The wrong key decrypts nothing.
    TreeNode rejected;
    CHECK(!open(&store, contentKey(testHash("other-secret")), sealed.rootId, &rejected, &error));

    // A single-file bundle is still a directory manifest.
    TreeNode single;
    single.isDir = true;
    TreeNode only;
    only.name = QStringLiteral("only.txt");
    only.data = QByteArray("only");
    single.children << only;

    TreeSource singleSource;
    MemoryStore singleStore;
    SealedBundle singleSealed;
    CHECK(seal(&singleStore, key, singleSource.build(single), &singleSource,
               &singleSealed, &error));
    TreeNode singleOpened;
    CHECK(open(&singleStore, key, singleSealed.rootId, &singleOpened, &error));
    CHECK_EQ(treeDifference(single, singleOpened, QStringLiteral("single")), QString());

    // An empty bundle is legal: one empty directory manifest, no data objects.
    TreeNode emptyRoot;
    emptyRoot.isDir = true;
    TreeSource emptySource;
    MemoryStore emptyStore;
    SealedBundle emptySealed;
    CHECK(seal(&emptyStore, key, emptySource.build(emptyRoot), &emptySource,
               &emptySealed, &error));
    CHECK_EQ(emptySealed.objectCount, 1);
    CHECK_EQ(emptySealed.totalBytes, quint64(0));
    TreeNode emptyOpened;
    CHECK(open(&emptyStore, key, emptySealed.rootId, &emptyOpened, &error));
    CHECK_EQ(emptyOpened.children.size(), 0);

    // A file is not a bundle root.
    SealedBundle ignored;
    CHECK(!seal(&store, key, source.build(only), &source, &ignored, &error));

    // Names that could escape a directory are refused.
    TreeNode hostile;
    hostile.isDir = true;
    TreeNode escape;
    escape.name = QStringLiteral("..");
    escape.data = QByteArray("x");
    hostile.children << escape;
    TreeSource hostileSource;
    CHECK(!seal(&store, key, hostileSource.build(hostile), &hostileSource,
                &ignored, &error));

    TreeNode duplicates;
    duplicates.isDir = true;
    TreeNode one;
    one.name = QStringLiteral("same.txt");
    one.data = QByteArray("1");
    TreeNode two;
    two.name = QStringLiteral("same.txt");
    two.data = QByteArray("2");
    duplicates.children << one << two;
    TreeSource duplicateSource;
    CHECK(!seal(&store, key, duplicateSource.build(duplicates), &duplicateSource,
                &ignored, &error));
}

void testMissingObjects()
{
    section("fetch planning: walk, ask, fetch, repeat");

    const QByteArray key = contentKey(testHash("fetch-secret"));
    const TreeNode tree = sampleTree();

    TreeSource source;
    MemoryStore origin;
    SealedBundle sealed;
    QString error;
    CHECK(seal(&origin, key, source.build(tree), &source, &sealed, &error));

    // Nothing has been fetched yet, so the root manifest is the whole list: its
    // entries cannot be known until it arrives.
    MemoryStore client;
    QVector<QByteArray> missing;
    CHECK(missingObjects(&client, key, sealed.rootId, &missing, &error));
    CHECK_EQ(missing.size(), 1);
    CHECK_EQ(missing.value(0), sealed.rootId);

    // A bundle that is already whole needs nothing at all.
    CHECK(missingObjects(&origin, key, sealed.rootId, &missing, &error));
    CHECK_EQ(missing.size(), 0);

    // The walk only descends through manifests it has, so the download takes
    // several rounds — and asks for every object exactly once.
    int fetched = 0;
    int rounds = 0;
    CHECK(fetchAll(&origin, &client, key, sealed.rootId, &fetched, &rounds, &error));
    if (!error.isEmpty())
        printf("  fetch error: %s\n", qPrintable(error));
    CHECK(rounds > 1);
    CHECK_EQ(fetched, sealed.objectCount);
    CHECK_EQ(client.count(), origin.count());

    TreeNode opened;
    CHECK(open(&client, key, sealed.rootId, &opened, &error));
    CHECK_EQ(treeDifference(tree, opened, QStringLiteral("fetched")), QString());

    // Asking a complete store again is free and changes nothing.
    CHECK(fetchAll(&origin, &client, key, sealed.rootId, &fetched, &rounds, &error));
    CHECK_EQ(fetched, sealed.objectCount);
    CHECK_EQ(client.count(), origin.count());

    // The wrong key is not a fetching problem: the objects are all here and none
    // of them open, and that is an error, not an endless fetch list.
    CHECK(!missingObjects(&origin, contentKey(testHash("some-other-secret")),
                          sealed.rootId, &missing, &error));
    CHECK(!error.isEmpty());

    // Nor is an id that could never name an object.
    CHECK(!missingObjects(&origin, key, QByteArray("short"), &missing, &error));

    // The receiver's rule: bytes are stored only under the id they hash to, so a
    // peer cannot answer a request with something else under that name.
    QByteArray object;
    CHECK(origin.get(sealed.rootId, &object));
    QByteArray damaged = object;
    damaged[0] = char(damaged.at(0) ^ 0x01);

    MemoryStore guarded;
    CHECK(!storeReceived(&guarded, sealed.rootId, damaged));
    CHECK_EQ(guarded.count(), 0);
    CHECK(!guarded.contains(sealed.rootId));
    CHECK(storeReceived(&guarded, sealed.rootId, object));
    CHECK(guarded.contains(sealed.rootId));
}

void testBundleObjects()
{
    section("object listing: every object a bundle is made of");

    const QByteArray key = contentKey(testHash("reach-secret"));
    const TreeNode tree = sampleTree();

    TreeSource source;
    MemoryStore origin;
    SealedBundle sealed;
    QString error;
    CHECK(seal(&origin, key, source.build(tree), &source, &sealed, &error));

    QVector<QByteArray> ids;
    CHECK(bundleObjects(&origin, key, sealed.rootId, &ids, &error));
    if (!error.isEmpty())
        printf("  listing error: %s\n", qPrintable(error));

    // The store holds this bundle and nothing else, so the list is the whole store:
    // every manifest and every chunk, and nothing invented.
    CHECK_EQ(ids.size(), origin.count());
    CHECK_EQ(ids.size(), sealed.objectCount);
    CHECK(ids.contains(sealed.rootId));

    // Nothing twice — a share's object set is a set, and an id reached by two
    // paths (or a chunk named by two files) must be counted once.
    QSet<QByteArray> unique;
    for (int i = 0; i < ids.size(); ++i)
        unique.insert(ids.at(i));
    CHECK_EQ(unique.size(), ids.size());

    for (int i = 0; i < ids.size(); ++i)
        CHECK_EQ(origin.contains(ids.at(i)), true);

    // The wrong key cannot read the manifests, so nothing can be enumerated — and
    // that is an error, not an empty list. This is the property storage management
    // leans on: an object it cannot account for is never called unreferenced.
    ids.clear();
    CHECK(!bundleObjects(&origin, contentKey(testHash("other-secret")),
                         sealed.rootId, &ids, &error));
    CHECK(!error.isEmpty());

    // Refill: the wrong-key case cleared the list.
    CHECK(bundleObjects(&origin, key, sealed.rootId, &ids, &error));

    // Drop one object at a time from a copy of the store. A chunk is named by its
    // file manifest rather than read, so losing one leaves the listing whole;
    // losing a manifest is refused, because what is under it is then unknown.
    int chunk = 0;
    int manifest = 0;
    for (int i = 0; i < ids.size(); ++i) {
        MemoryStore trial = origin;
        trial.remove(ids.at(i));

        QVector<QByteArray> sub;
        QString subError;
        if (bundleObjects(&trial, key, sealed.rootId, &sub, &subError)) {
            ++chunk;
            CHECK_EQ(sub.size(), ids.size());
        } else {
            ++manifest;
        }
    }
    CHECK(manifest > 0);
    CHECK(chunk > 0);

    // The root is the one manifest that is always needed: without it there is no
    // bundle to walk at all.
    MemoryStore headless = origin;
    headless.remove(sealed.rootId);
    CHECK(!bundleObjects(&headless, key, sealed.rootId, &ids, &error));

    // Nor is an id that could never name an object a bundle.
    CHECK(!bundleObjects(&origin, key, QByteArray("short"), &ids, &error));
}

void testCorruptionIsCaught()
{
    section("verification: a damaged object is never used");

    const QByteArray key = contentKey(testHash("corruption-secret"));
    const TreeNode tree = sampleTree();

    TreeSource source;
    MemoryStore store;
    SealedBundle sealed;
    QString error;
    CHECK(seal(&store, key, source.build(tree), &source, &sealed, &error));

    for (int read = 0; read < 8; ++read) {
        CorruptingStore corrupt(&store);
        corrupt.corruptReadNumber(read);
        TreeNode opened;
        CHECK(!open(&corrupt, key, sealed.rootId, &opened, &error));
    }
}

void testLargeDirectorySegmentation()
{
    section("a large directory becomes a segment index");

    const QByteArray key = contentKey(testHash("segmentation-secret"));

    const int fileCount = 6000;
    TreeNode root;
    root.isDir = true;
    for (int i = 0; i < fileCount; ++i) {
        TreeNode file;
        file.name = QStringLiteral("f%1").arg(i, 4, 10, QLatin1Char('0'));
        file.data = QByteArray("x");
        root.children.append(file);
    }

    TreeSource source;
    MemoryStore store;
    SealedBundle sealed;
    QString error;
    CHECK(seal(&store, key, source.build(root), &source, &sealed, &error));
    CHECK_EQ(sealed.totalBytes, quint64(fileCount));
    CHECK_EQ(sealed.fileCount, fileCount);

    // The root manifest must be the index form, not a leaf.
    QByteArray object;
    CHECK(store.get(sealed.rootId, &object));
    QByteArray manifest;
    CHECK(Object::open(key, object, &manifest, &error));
    Cbor::Value value;
    CHECK(Cbor::decode(manifest, &value, &error));
    CHECK(value.member("c") != 0);
    CHECK(value.member("e") == 0);

    TreeNode opened;
    CHECK(open(&store, key, sealed.rootId, &opened, &error));
    if (!error.isEmpty())
        printf("  open error: %s\n", qPrintable(error));
    CHECK_EQ(opened.children.size(), fileCount);
    CHECK_EQ(treeDifference(root, opened, QStringLiteral("many")), QString());
    CHECK_EQ(opened.children.size() > 0 ? opened.children.at(0).name : QString(),
             QStringLiteral("f0000"));
    CHECK_EQ(opened.children.size() > 0 ? opened.children.at(fileCount - 1).name : QString(),
             QStringLiteral("f5999"));

    // The fetch planner walks the index and each leaf it points at. With only the
    // index in hand it must name the leaves, not the files underneath them.
    MemoryStore receiver;
    QVector<QByteArray> missing;
    CHECK(missingObjects(&receiver, key, sealed.rootId, &missing, &error));
    CHECK_EQ(missing.size(), 1);

    QByteArray indexObject;
    CHECK(store.get(sealed.rootId, &indexObject));
    CHECK(storeReceived(&receiver, sealed.rootId, indexObject));

    CHECK(missingObjects(&receiver, key, sealed.rootId, &missing, &error));
    CHECK(missing.size() > 0);
    for (int i = 0; i < missing.size(); ++i)
        CHECK(store.contains(missing.at(i)));

    int fetched = 0;
    int rounds = 0;
    CHECK(fetchAll(&store, &receiver, key, sealed.rootId, &fetched, &rounds, &error));
    CHECK_EQ(fetched, sealed.objectCount - 1);
    CHECK_EQ(receiver.count(), store.count());
}

void testWire()
{
    section("tunnel wire: framing, handshake MAC, messages");

    // --- framing ---------------------------------------------------------
    // A frame round-trips through the reader. The length field counts the type
    // byte plus the payload: 1 + 3 bytes of payload = 4.
    const QByteArray frame = Wire::encodeFrame(Wire::Want, hex("aabbcc"));
    CHECK_EQ(frame.left(4), hex("00000004"));
    CHECK_EQ(frame.size(), 8);
    CHECK_EQ(int(quint8(frame.at(4))), int(Wire::Want));

    {
        Wire::FrameReader reader;
        reader.push(frame);
        quint8 type = 0;
        QByteArray payload;
        CHECK(reader.next(&type, &payload));
        CHECK_EQ(int(type), int(Wire::Want));
        CHECK_EQ(payload, hex("aabbcc"));
        CHECK(!reader.next(&type, &payload));       // nothing left
        CHECK(!reader.failed());
    }

    // Chunk boundaries are the reader's whole reason to exist: bytes arrive
    // one at a time, frames come out whole.
    {
        Wire::FrameReader reader;
        quint8 type = 0;
        QByteArray payload;
        for (int i = 0; i < frame.size(); ++i) {
            reader.push(frame.mid(i, 1));
            if (i < frame.size() - 1)
                CHECK(!reader.next(&type, &payload));
        }
        CHECK(reader.next(&type, &payload));
        CHECK_EQ(payload, hex("aabbcc"));
    }

    // Two frames in one buffer, and a partial third.
    {
        const QByteArray first = Wire::encodeFrame(Wire::Data, hex("01"));
        const QByteArray second = Wire::encodeFrame(Wire::Reject, hex("02"));
        const QByteArray third = Wire::encodeFrame(Wire::Ping, hex("03"));

        Wire::FrameReader reader;
        reader.push(first + second + third.left(4));
        quint8 type = 0;
        QByteArray payload;
        CHECK(reader.next(&type, &payload));
        CHECK_EQ(int(type), int(Wire::Data));
        CHECK(reader.next(&type, &payload));
        CHECK_EQ(int(type), int(Wire::Reject));
        CHECK(!reader.next(&type, &payload));       // header only
        reader.push(third.mid(4));
        CHECK(reader.next(&type, &payload));
        CHECK_EQ(int(type), int(Wire::Ping));
    }

    // An oversized frame is refused from its header, not buffered.
    {
        Wire::FrameReader reader;
        QByteArray header;
        header.append(char(0x7f)); header.append(char(0xff));
        header.append(char(0xff)); header.append(char(0xff));
        reader.push(header);
        CHECK(reader.failed());
        quint8 type = 0;
        QByteArray payload;
        CHECK(!reader.next(&type, &payload));
    }

    // A zero-length frame is impossible (the type byte alone is one byte).
    {
        Wire::FrameReader reader;
        reader.push(hex("00000000"));
        CHECK(reader.failed());
    }

    // --- handshake MAC (spec §8) ----------------------------------------
    // Pinned HMAC vector, RFC 4231 test case 2, so a wrong HMAC is caught.
    {
        QByteArray mac;
        CHECK(Crypto::hmacSha256(QByteArrayLiteral("Jefe"),
                                 QByteArrayLiteral("what do ya want for nothing?"), &mac));
        CHECK_EQ(mac, hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    }

    const QByteArray token = testHash("swarm-token");
    const QByteArray nonceA = hex("0102030405060708");
    const QByteArray nonceB = hex("1112131415161718");
    const QByteArray bundleId = testHash("bundle");

    const QByteArray initiator =
        Wire::handshakeMac(token, 'i', nonceA, nonceB, bundleId);
    const QByteArray responder =
        Wire::handshakeMac(token, 'r', nonceB, nonceA, bundleId);

    CHECK_EQ(initiator.size(), Crypto::Sha256Size);
    CHECK(initiator != responder);                  // prefix and nonce order both matter
    CHECK(Wire::macEquals(initiator, Wire::handshakeMac(token, 'i', nonceA, nonceB, bundleId)));
    CHECK(!Wire::macEquals(initiator, responder));
    CHECK(!Wire::macEquals(initiator, initiator.left(31)));   // length difference
    // A different token, or swapped nonces, is a different MAC — the whole point.
    CHECK(initiator != Wire::handshakeMac(testHash("other-token"), 'i', nonceA, nonceB, bundleId));
    CHECK(initiator != Wire::handshakeMac(token, 'i', nonceB, nonceA, bundleId));

    // --- messages --------------------------------------------------------
    {
        Wire::HelloMessage hello;
        hello.bundleId = bundleId;
        hello.peerId = testHash("peer");
        hello.nonce = nonceA;
        Wire::HelloMessage back;
        QString error;
        CHECK(Wire::decodeHello(Wire::encodeHello(hello), &back, &error));
        CHECK_EQ(back.version, hello.version);
        CHECK_EQ(back.bundleId, hello.bundleId);
        CHECK_EQ(back.peerId, hello.peerId);
        CHECK_EQ(back.nonce, hello.nonce);

        // A short nonce, or an id that is not a hash, is not a HELLO.
        Wire::HelloMessage bad = hello;
        bad.nonce = hex("010203");                  // 3 bytes, below the 4..16 floor
        CHECK(!Wire::decodeHello(Wire::encodeHello(bad), &back, &error));
        Wire::HelloMessage wrongId = hello;
        wrongId.bundleId = hex("00");
        CHECK(!Wire::decodeHello(Wire::encodeHello(wrongId), &back, &error));
    }

    {
        Wire::AuthMessage auth;
        auth.mac = initiator;
        Wire::AuthMessage back;
        QString error;
        CHECK(Wire::decodeAuth(Wire::encodeAuth(auth), &back, &error));
        CHECK_EQ(back.mac, auth.mac);
        Wire::AuthMessage bad;
        bad.mac = hex("00");
        CHECK(!Wire::decodeAuth(Wire::encodeAuth(bad), &back, &error));
    }

    {
        Wire::WantMessage want;
        want.hash = testHash("object");
        Wire::WantMessage back;
        QString error;
        CHECK(Wire::decodeWant(Wire::encodeWant(want), &back, &error));
        CHECK_EQ(back.hash, want.hash);
        Wire::WantMessage bad;
        bad.hash = hex("0102");
        CHECK(!Wire::decodeWant(Wire::encodeWant(bad), &back, &error));
    }

    {
        Wire::DataMessage data;
        data.index = 12345;
        data.hash = testHash("object");
        data.bytes = hex("0102030405");
        Wire::DataMessage back;
        QString error;
        CHECK(Wire::decodeData(Wire::encodeData(data), &back, &error));
        CHECK_EQ(back.index, data.index);
        CHECK_EQ(back.hash, data.hash);
        CHECK_EQ(back.bytes, data.bytes);
    }

    {
        Wire::RejectMessage reject;
        reject.hash = testHash("object");
        reject.why = Wire::NotFound;
        Wire::RejectMessage back;
        QString error;
        CHECK(Wire::decodeReject(Wire::encodeReject(reject), &back, &error));
        CHECK_EQ(back.why, reject.why);
        CHECK_EQ(back.hash, reject.hash);
    }

    {
        quint64 back = 0;
        QString error;
        CHECK(Wire::decodePing(Wire::encodePing(987654321), &back, &error));
        CHECK_EQ(back, quint64(987654321));
    }

    {
        Wire::ErrorMessage errorOut;
        errorOut.code = Wire::BadToken;
        errorOut.message = QStringLiteral("bad token");
        Wire::ErrorMessage back;
        QString error;
        CHECK(Wire::decodeError(Wire::encodeError(errorOut), &back, &error));
        CHECK_EQ(back.code, errorOut.code);
        CHECK_EQ(back.message, errorOut.message);
    }

    // A message that is not a map is refused with a reason.
    {
        Wire::WantMessage back;
        QString error;
        CHECK(!Wire::decodeWant(Cbor::encodeUnsigned(1), &back, &error));
        CHECK(!error.isEmpty());
    }

    // REGISTER carries the relay-scoped credential, so the relay never needs the
    // swarm token.
    {
        Wire::RegisterMessage registration;
        registration.bundleId = bundleId;
        registration.token = relayToken(token);
        registration.objects = 22;
        registration.files = 6;
        registration.bytes = 1687667;
        Wire::RegisterMessage back;
        QString error;
        CHECK(Wire::decodeRegister(Wire::encodeRegister(registration), &back, &error));
        CHECK_EQ(back.bundleId, registration.bundleId);
        CHECK_EQ(back.token, registration.token);
        CHECK_EQ(back.objects, registration.objects);
        CHECK_EQ(back.files, registration.files);
        CHECK_EQ(back.bytes, registration.bytes);

        Wire::RegisterMessage bad = registration;
        bad.token = hex("00");
        CHECK(!Wire::decodeRegister(Wire::encodeRegister(bad), &back, &error));
    }

    // The three key derivations are independent: knowing one says nothing about
    // the others, which is exactly why the relay gets its own.
    {
        const QByteArray secret = testHash("secret");
        const QByteArray content = contentKey(secret);
        const QByteArray swarm = swarmToken(secret);
        const QByteArray relay = relayToken(secret);
        CHECK_EQ(content.size(), int(Crypto::KeySize));
        CHECK_EQ(swarm.size(), int(Crypto::KeySize));
        CHECK_EQ(relay.size(), int(Crypto::KeySize));
        CHECK(content != swarm);
        CHECK(swarm != relay);
        CHECK(content != relay);
        CHECK_EQ(relay, relayToken(secret));            // deterministic
        CHECK(relay != relayToken(testHash("secret2")));
    }
}

} // namespace

int main()
{
    printf("libkelp unit tests\n");

    testCborEncoding();
    testManifestVectors();
    testCborDecoding();
    testManifestRules();
    testCryptoVectors();
    testObjectEnvelope();
    testStoreContract();
    testLink();
    testBundleRoundTrip();
    testMissingObjects();
    testBundleObjects();
    testCorruptionIsCaught();
    testLargeDirectorySegmentation();
    testWire();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
