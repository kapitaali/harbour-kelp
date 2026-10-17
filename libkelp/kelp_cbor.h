#ifndef KELP_CBOR_H
#define KELP_CBOR_H

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

namespace Kelp {
namespace Cbor {

/*
 * Canonical CBOR (RFC 8949 Core Deterministic Encoding).
 *
 * Only the subset Kelp needs is supported: unsigned/negative integers, byte
 * strings, text strings, arrays, maps, and the two booleans. Definite lengths
 * only, shortest-form integers, map keys sorted bytewise by their encoded
 * bytes. Encoding is strict; decoding rejects anything non-canonical, so a
 * manifest has exactly one byte representation (its hash would otherwise be
 * ambiguous).
 *
 * Booleans are here for the link's sealed hints (design §5.5), which need to
 * say "this share is a page"; they are the only simple values accepted, and
 * accepting them is also what lets an unknown boolean key in a manifest be
 * ignored rather than fail the whole decode (wire spec §17).
 */

// --- Encoding -----------------------------------------------------------
// Each value is returned as its own byte string, so values compose freely.

QByteArray encodeUnsigned(quint64 value);
QByteArray encodeInt(qint64 value);
QByteArray encodeBytes(const QByteArray &value);
QByteArray encodeText(const QString &value);
QByteArray encodeBool(bool value);
QByteArray encodeArray(const QVector<QByteArray> &items);

// Sorts the entries canonically. Returns an empty QByteArray if two keys
// encode identically (a canonical map cannot have duplicate keys).
QByteArray encodeMap(const QVector<QPair<QByteArray, QByteArray> > &entries);

// --- Decoding -----------------------------------------------------------

struct Value {
    enum Type { Invalid, Unsigned, Negative, Bytes, Text, Array, Map, Bool };

    Value() : type(Invalid), unsignedValue(0), negativeValue(0), boolValue(false) {}

    Type type;
    quint64 unsignedValue;
    qint64 negativeValue;               // the actual value, e.g. -1 for 0x20
    bool boolValue;
    QByteArray bytes;
    QString text;
    QVector<Value> array;
    QVector<QPair<Value, Value> > map;  // in wire (canonical) order

    bool isMap() const { return type == Map; }

    qint64 integerValue() const
    { return type == Unsigned ? qint64(unsignedValue) : negativeValue; }

    // Map lookup by text key; null when absent or when this is not a map.
    const Value *member(const char *key) const;
};

// Strict: rejects non-canonical integers, indefinite lengths, tags, floats,
// simple values other than the two booleans, unsorted or duplicate map keys,
// invalid UTF-8 and trailing bytes.
bool decode(const QByteArray &input, Value *out, QString *error);

} // namespace Cbor
} // namespace Kelp

#endif // KELP_CBOR_H
