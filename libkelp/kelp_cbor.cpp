#include "kelp_cbor.h"

#include <algorithm>

namespace Kelp {
namespace Cbor {

namespace {

void appendHead(QByteArray *out, quint8 major, quint64 value)
{
    const quint8 base = quint8(major << 5);
    if (value < 24) {
        out->append(char(base | quint8(value)));
    } else if (value <= 0xFF) {
        out->append(char(base | 24));
        out->append(char(value & 0xFF));
    } else if (value <= 0xFFFF) {
        out->append(char(base | 25));
        out->append(char((value >> 8) & 0xFF));
        out->append(char(value & 0xFF));
    } else if (value <= 0xFFFFFFFFULL) {
        out->append(char(base | 26));
        for (int i = 3; i >= 0; --i)
            out->append(char((value >> (8 * i)) & 0xFF));
    } else {
        out->append(char(base | 27));
        for (int i = 7; i >= 0; --i)
            out->append(char((value >> (8 * i)) & 0xFF));
    }
}

// Manifests nest three levels at most; this only exists to make a malicious
// input fail cleanly instead of exhausting the stack.
const int kMaxDepth = 32;

class Reader
{
public:
    explicit Reader(const QByteArray &data) : m_data(data), m_pos(0) {}

    bool read(Value *out, QString *error)
    {
        if (!readItem(out, 0, error))
            return false;
        if (m_pos != m_data.size()) {
            *error = QStringLiteral("trailing bytes after the CBOR item");
            return false;
        }
        return true;
    }

private:
    bool readHead(quint8 *major, quint64 *value, QString *error)
    {
        if (m_pos >= m_data.size()) {
            *error = QStringLiteral("truncated CBOR head");
            return false;
        }
        const quint8 first = quint8(m_data.at(m_pos++));
        *major = quint8(first >> 5);

        const quint8 additional = quint8(first & 0x1F);
        if (additional < 24) {
            *value = additional;
            return true;
        }

        int extra = 0;
        quint64 minimal = 0;
        switch (additional) {
        case 24: extra = 1; minimal = 24; break;
        case 25: extra = 2; minimal = 256; break;
        case 26: extra = 4; minimal = 65536; break;
        case 27: extra = 8; minimal = 4294967296ULL; break;
        default:
            *error = QStringLiteral("indefinite-length or reserved CBOR head");
            return false;
        }

        if (m_pos + extra > m_data.size()) {
            *error = QStringLiteral("truncated CBOR head");
            return false;
        }
        quint64 v = 0;
        for (int i = 0; i < extra; ++i)
            v = (v << 8) | quint64(quint8(m_data.at(m_pos++)));
        if (v < minimal) {
            *error = QStringLiteral("non-canonical (non-shortest) CBOR integer");
            return false;
        }
        *value = v;
        return true;
    }

    bool readStringItem(Value *out, Value::Type type, quint64 length, QString *error)
    {
        if (length > quint64(m_data.size() - m_pos)) {
            *error = QStringLiteral("CBOR string length exceeds the input");
            return false;
        }
        const QByteArray raw = m_data.mid(m_pos, int(length));
        m_pos += int(length);

        if (type == Value::Bytes) {
            out->type = Value::Bytes;
            out->bytes = raw;
            return true;
        }

        const QString text = QString::fromUtf8(raw);
        if (text.toUtf8() != raw) {
            *error = QStringLiteral("CBOR text string is not valid UTF-8");
            return false;
        }
        out->type = Value::Text;
        out->text = text;
        return true;
    }

    bool readItem(Value *out, int depth, QString *error)
    {
        if (depth > kMaxDepth) {
            *error = QStringLiteral("CBOR nesting is too deep");
            return false;
        }

        quint8 major = 0;
        quint64 argument = 0;
        if (!readHead(&major, &argument, error))
            return false;

        switch (major) {
        case 0: // unsigned
            out->type = Value::Unsigned;
            out->unsignedValue = argument;
            return true;

        case 1: // negative
            if (argument > 0x7FFFFFFFFFFFFFFFULL) {
                *error = QStringLiteral("negative CBOR integer outside the int64 range");
                return false;
            }
            out->type = Value::Negative;
            out->negativeValue = -1 - qint64(argument);
            return true;

        case 2: // byte string
            return readStringItem(out, Value::Bytes, argument, error);

        case 3: // text string
            return readStringItem(out, Value::Text, argument, error);

        case 4: { // array
            if (argument > quint64(m_data.size() - m_pos)) {
                *error = QStringLiteral("CBOR array length exceeds the input");
                return false;
            }
            out->type = Value::Array;
            out->array.reserve(int(argument));
            for (quint64 i = 0; i < argument; ++i) {
                Value item;
                if (!readItem(&item, depth + 1, error))
                    return false;
                out->array.append(item);
            }
            return true;
        }

        case 5: { // map
            if (argument > quint64(m_data.size() - m_pos)) {
                *error = QStringLiteral("CBOR map length exceeds the input");
                return false;
            }
            out->type = Value::Map;
            out->map.reserve(int(argument));

            bool first = true;
            QByteArray previousKey;
            for (quint64 i = 0; i < argument; ++i) {
                const int keyStart = m_pos;
                Value key;
                if (!readItem(&key, depth + 1, error))
                    return false;
                if (key.type != Value::Text) {
                    *error = QStringLiteral("CBOR map key is not a text string");
                    return false;
                }
                const QByteArray encodedKey = m_data.mid(keyStart, m_pos - keyStart);
                if (!first && encodedKey <= previousKey) {
                    *error = QStringLiteral("CBOR map keys are not in canonical order");
                    return false;
                }
                first = false;
                previousKey = encodedKey;

                Value item;
                if (!readItem(&item, depth + 1, error))
                    return false;
                out->map.append(qMakePair(key, item));
            }
            return true;
        }

        default: // 6 = tag, 7 = simple value / float
            *error = QStringLiteral("unsupported CBOR major type (tag, simple value or float)");
            return false;
        }
    }

    QByteArray m_data;
    int m_pos;
};

} // namespace

QByteArray encodeUnsigned(quint64 value)
{
    QByteArray out;
    appendHead(&out, 0, value);
    return out;
}

QByteArray encodeInt(qint64 value)
{
    QByteArray out;
    if (value >= 0)
        appendHead(&out, 0, quint64(value));
    else
        appendHead(&out, 1, quint64(-(value + 1)));
    return out;
}

QByteArray encodeBytes(const QByteArray &value)
{
    QByteArray out;
    appendHead(&out, 2, quint64(value.size()));
    out.append(value);
    return out;
}

QByteArray encodeText(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    QByteArray out;
    appendHead(&out, 3, quint64(utf8.size()));
    out.append(utf8);
    return out;
}

QByteArray encodeArray(const QVector<QByteArray> &items)
{
    QByteArray out;
    appendHead(&out, 4, quint64(items.size()));
    for (int i = 0; i < items.size(); ++i)
        out.append(items.at(i));
    return out;
}

QByteArray encodeMap(const QVector<QPair<QByteArray, QByteArray> > &entries)
{
    QVector<QPair<QByteArray, QByteArray> > sorted = entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const QPair<QByteArray, QByteArray> &a, const QPair<QByteArray, QByteArray> &b) {
                  return a.first < b.first;
              });

    for (int i = 1; i < sorted.size(); ++i)
        if (sorted.at(i).first == sorted.at(i - 1).first)
            return QByteArray();

    QByteArray out;
    appendHead(&out, 5, quint64(sorted.size()));
    for (int i = 0; i < sorted.size(); ++i) {
        out.append(sorted.at(i).first);
        out.append(sorted.at(i).second);
    }
    return out;
}

const Value *Value::member(const char *key) const
{
    if (type != Map)
        return 0;
    const QString wanted = QString::fromLatin1(key);
    for (int i = 0; i < map.size(); ++i)
        if (map.at(i).first.text == wanted)
            return &map.at(i).second;
    return 0;
}

bool decode(const QByteArray &input, Value *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!out) {
        *error = QStringLiteral("no destination for the decoded value");
        return false;
    }
    Reader reader(input);
    return reader.read(out, error);
}

} // namespace Cbor
} // namespace Kelp
