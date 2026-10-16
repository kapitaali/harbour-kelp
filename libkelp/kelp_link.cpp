#include "kelp_link.h"

#include <QUrl>

#include "kelp_crypto.h"

namespace Kelp {
namespace Link {

namespace {

const char kBase32Alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
const char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

const char kScheme[] = "kelp://";

} // namespace

QByteArray base32Encode(const QByteArray &data)
{
    QByteArray out;
    out.reserve((data.size() * 8 + 4) / 5);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < data.size(); ++i) {
        buffer = (buffer << 8) | quint32(quint8(data.at(i)));
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.append(kBase32Alphabet[(buffer >> bits) & 0x1F]);
        }
    }
    if (bits > 0)
        out.append(kBase32Alphabet[(buffer << (5 - bits)) & 0x1F]);
    return out;
}

bool base32Decode(const QString &text, QByteArray *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    out->clear();
    out->reserve(text.size() * 5 / 8);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        int value = -1;
        if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            value = c.unicode() - 'a';
        else if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            value = c.unicode() - 'A';
        else if (c >= QLatin1Char('2') && c <= QLatin1Char('7'))
            value = 26 + (c.unicode() - '2');
        if (value < 0) {
            *error = QStringLiteral("'%1' is not a base32 character").arg(c);
            return false;
        }

        buffer = (buffer << 5) | quint32(value);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out->append(char((buffer >> bits) & 0xFF));
        }
    }

    // The leftover bits of an unpadded encoding must be zero, or two different
    // strings would decode to the same bytes.
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        *error = QStringLiteral("base32 input has non-zero padding bits");
        return false;
    }
    return true;
}

QString base64UrlEncode(const QByteArray &data)
{
    QByteArray out;
    out.reserve((data.size() * 4 + 2) / 3);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < data.size(); ++i) {
        buffer = (buffer << 8) | quint32(quint8(data.at(i)));
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.append(kBase64UrlAlphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0)
        out.append(kBase64UrlAlphabet[(buffer << (6 - bits)) & 0x3F]);
    return QString::fromLatin1(out);
}

bool base64UrlDecode(const QString &text, QByteArray *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    out->clear();
    out->reserve(text.size() * 3 / 4);

    quint32 buffer = 0;
    int bits = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        int value = -1;
        if (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            value = c.unicode() - 'A';
        else if (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            value = 26 + (c.unicode() - 'a');
        else if (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            value = 52 + (c.unicode() - '0');
        else if (c == QLatin1Char('-') || c == QLatin1Char('+'))
            value = 62;
        else if (c == QLatin1Char('_') || c == QLatin1Char('/'))
            value = 63;
        else if (c == QLatin1Char('='))
            continue;                      // padding is optional here
        else {
            *error = QStringLiteral("'%1' is not a base64url character").arg(c);
            return false;
        }

        buffer = (buffer << 6) | quint32(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->append(char((buffer >> bits) & 0xFF));
        }
    }

    if (bits >= 6) {
        *error = QStringLiteral("base64url input has a dangling group");
        return false;
    }
    if (bits > 0 && (buffer & ((1u << bits) - 1)) != 0) {
        *error = QStringLiteral("base64url input has non-zero padding bits");
        return false;
    }
    return true;
}

QString encode(const Bundle &bundle)
{
    QString text = QString::fromLatin1(kScheme);
    text += QString::fromLatin1(base32Encode(bundle.bundleId));

    QStringList query;
    for (int i = 0; i < bundle.relays.size(); ++i) {
        query.append(QStringLiteral("r=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(bundle.relays.at(i))));
    }
    if (!bundle.lan.isEmpty()) {
        query.append(QStringLiteral("l=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(bundle.lan)));
    }
    if (!bundle.name.isEmpty()) {
        query.append(QStringLiteral("n=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(bundle.name)));
    }
    if (!query.isEmpty())
        text += QLatin1Char('?') + query.join(QLatin1Char('&'));

    text += QStringLiteral("#k=") + base64UrlEncode(bundle.secret);
    return text;
}

bool decode(const QString &text, Bundle *out, QString *error)
{
    QString scratch;
    if (!error)
        error = &scratch;
    if (!out) {
        *error = QStringLiteral("no destination for the link");
        return false;
    }

    QString rest = text.trimmed();
    if (!rest.startsWith(QString::fromLatin1(kScheme), Qt::CaseInsensitive)) {
        *error = QStringLiteral("link does not start with kelp://");
        return false;
    }
    rest = rest.mid(int(sizeof(kScheme) - 1));

    QString fragment;
    const int hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        fragment = rest.mid(hash + 1);
        rest = rest.left(hash);
    }

    QString query;
    const int question = rest.indexOf(QLatin1Char('?'));
    if (question >= 0) {
        query = rest.mid(question + 1);
        rest = rest.left(question);
    }

    Bundle bundle;
    if (!base32Decode(rest, &bundle.bundleId, error))
        return false;
    if (bundle.bundleId.size() != Crypto::Sha256Size) {
        *error = QStringLiteral("bundle id is not 32 bytes");
        return false;
    }

    bool haveSecret = false;
    const QStringList fragmentParts = fragment.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < fragmentParts.size(); ++i) {
        const QString &part = fragmentParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0 || part.left(equals) != QLatin1String("k"))
            continue;
        if (!base64UrlDecode(part.mid(equals + 1), &bundle.secret, error))
            return false;
        haveSecret = true;
    }
    if (!haveSecret) {
        *error = QStringLiteral("link has no key fragment");
        return false;
    }
    if (bundle.secret.size() != Crypto::KeySize) {
        *error = QStringLiteral("secret is not 32 bytes");
        return false;
    }

    const QStringList queryParts = query.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (int i = 0; i < queryParts.size(); ++i) {
        const QString &part = queryParts.at(i);
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = part.left(equals);
        const QString value =
            QUrl::fromPercentEncoding(part.mid(equals + 1).toUtf8());
        if (key == QLatin1String("r"))
            bundle.relays.append(value);
        else if (key == QLatin1String("l"))
            bundle.lan = value;
        else if (key == QLatin1String("n"))
            bundle.name = value;
    }

    *out = bundle;
    return true;
}

} // namespace Link
} // namespace Kelp
