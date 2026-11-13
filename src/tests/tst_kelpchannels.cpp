/*
 * kelpchannels unit test — the publisher's channel registry.
 *
 * Three things have to hold and are pinned here:
 *
 *   1. A channel survives a restart: create() writes enough for link() to mint
 *      the same link and publish() to sign the next pointer.
 *   2. The two secrets never leak into the QML-facing rows, and the Ed25519
 *      private key is not in the metadata file at all.
 *   3. Both files are owner-only, and a damaged one is refused rather than
 *      overwritten — a corrupt file must not silently delete channels.
 *
 * QStandardPaths test mode redirects the app data directory to a throwaway
 * location, so this writes real files without touching the user's.
 */

#include <stdio.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileDevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "kelp_channel.h"
#include "kelp_crypto.h"
#include "kelp_link.h"
#include "kelpchannels.h"
#include "kelplog.h"
#include "kelpstore.h"

namespace {

int g_checks = 0;
int g_failures = 0;
QString g_section;

QString show(const QString &value) { return value; }
QString show(const QByteArray &value) { return QString::fromLatin1(value.toHex()); }
QString show(int value) { return QString::number(value); }
QString show(quint64 value) { return QString::number(value); }
QString show(bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); }

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

bool ownerOnly(const QString &path)
{
    const QFileDevice::Permissions perms = QFile::permissions(path);
    const QFileDevice::Permissions others =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    return (perms & others) == 0
        && (perms & QFileDevice::ReadOwner) != 0
        && (perms & QFileDevice::WriteOwner) != 0;
}

QString dataDirectory;

// The registry only asks whether the root object is in the store, so a real
// sealed manifest is not needed; hashing arbitrary bytes through put() gives a
// stable 32-byte address, which is all a bundle id is here.
QString putBundle(KelpObjectStore &store, const char *seed)
{
    QByteArray id;
    if (!store.put(QByteArray("bundle-root-") + QByteArray(seed), &id))
        return QString();
    return QString::fromLatin1(Kelp::Link::base32Encode(id));
}

QJsonArray readJsonArray(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QJsonArray();
    return QJsonDocument::fromJson(file.readAll()).array();
}

// Records what a publisher would hand to its relay tunnels.
class RecordingSink : public KelpChannelSink
{
public:
    QVector<QByteArray> ids;
    QVector<QByteArray> updates;

    int sendChannelUpdate(const QByteArray &channelId, const QByteArray &update) override
    {
        ids.append(channelId);
        updates.append(update);
        return 1;
    }
};

QString channelIdFromRow(const QVariantMap &row)
{
    return row.value(QStringLiteral("id")).toString();
}

void testChannels()
{
    const QString channelsPath = KelpChannels::filePath();
    const QString keysPath = KelpChannels::keysFilePath();
    QFile::remove(channelsPath);
    QFile::remove(keysPath);

    KelpObjectStore store(dataDirectory + QStringLiteral("/objects"));
    QString error;
    CHECK(store.initialise(&error));

    const QString bundleA = putBundle(store, "a");
    const QString bundleB = putBundle(store, "b");
    // Each bundle's content key, as base64url text: what a share row holds.
    const QString secretA = Kelp::Link::base64UrlEncode(Kelp::Crypto::randomBytes(Kelp::Crypto::KeySize));
    const QString secretB = Kelp::Link::base64UrlEncode(Kelp::Crypto::randomBytes(Kelp::Crypto::KeySize));
    CHECK_EQ(bundleA.size(), 52);
    CHECK_EQ(bundleB.size(), 52);
    CHECK(bundleA != bundleB);

    section("missing files are an empty list");
    {
        KelpChannels channels(&store);
        CHECK_EQ(KelpChannels::filePath(),
                 KelpLog::dataDirectory() + QStringLiteral("/channels.json"));
        CHECK_EQ(KelpChannels::keysFilePath(),
                 KelpLog::dataDirectory() + QStringLiteral("/channel-keys.json"));
        CHECK_EQ(channels.load().size(), 0);
        CHECK(!QFile::exists(channelsPath));
        CHECK(!QFile::exists(keysPath));
    }

    QString firstLink;
    QString channelId;
    QByteArray readSecret;
    QByteArray channelRawId;

    section("create");
    {
        KelpChannels channels(&store);
        channels.setRelays(QStringList() << QStringLiteral("kelp.example:9443")
                                         << QStringLiteral("https://relay.example:9443"));
        const QVariantMap created = channels.create(QStringLiteral("notes"), bundleA, secretA);
        CHECK(created.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(created.value(QStringLiteral("error")).toString(), QString());
        CHECK_EQ(created.value(QStringLiteral("name")).toString(), QStringLiteral("notes"));
        CHECK_EQ(created.value(QStringLiteral("bundleId")).toString(), bundleA);
        CHECK_EQ(created.value(QStringLiteral("sequence")).toULongLong(),
                 Q_UINT64_C(1));
        CHECK(created.value(QStringLiteral("timestamp")).toLongLong() > 0);
        CHECK_EQ(created.value(QStringLiteral("pointer")).toString().size(), 64);

        channelId = channelIdFromRow(created);
        CHECK_EQ(channelId.size(), 52);
        firstLink = created.value(QStringLiteral("link")).toString();
        CHECK(firstLink.startsWith(QStringLiteral("kelpchannel://")));

        Kelp::Channel::Link parsed;
        CHECK(Kelp::Channel::decodeLink(firstLink, &parsed, &error));
        channelRawId = parsed.id;
        readSecret = parsed.readSecret;
        CHECK_EQ(parsed.readSecret.size(), int(Kelp::Channel::ReadSecretSize));
        CHECK_EQ(QString::fromLatin1(Kelp::Link::base32Encode(parsed.id)), channelId);
        CHECK_EQ(parsed.relays.size(), 2);
        CHECK_EQ(parsed.relays.at(0), QStringLiteral("kelp.example:9443"));
        CHECK_EQ(parsed.relays.at(1), QStringLiteral("https://relay.example:9443"));
    }

    section("both files exist and are owner-only");
    {
        CHECK(QFile::exists(channelsPath));
        CHECK(QFile::exists(keysPath));
        CHECK(ownerOnly(channelsPath));
        CHECK(ownerOnly(keysPath));
    }

    section("load never hands out a secret");
    {
        KelpChannels channels(&store);
        const QVariantList rows = channels.load();
        CHECK_EQ(rows.size(), 1);
        const QVariantMap row = rows.at(0).toMap();
        CHECK_EQ(channelIdFromRow(row), channelId);
        CHECK_EQ(row.value(QStringLiteral("name")).toString(), QStringLiteral("notes"));
        CHECK_EQ(row.value(QStringLiteral("bundleId")).toString(), bundleA);
        CHECK_EQ(row.value(QStringLiteral("sequence")).toULongLong(), Q_UINT64_C(1));
        CHECK(row.value(QStringLiteral("hasKey")).toBool());
        CHECK_EQ(row.value(QStringLiteral("pointer")).toString().size(), 64);

        CHECK(!row.contains(QStringLiteral("secret")));
        CHECK(!row.contains(QStringLiteral("privateKey")));
        CHECK(!row.contains(QStringLiteral("link")));
        CHECK(!row.contains(QStringLiteral("publicKey")));

        // The metadata file may hold the read secret (it is what lets the link
        // be minted again) but must not hold the signing key. The key file is
        // the only place that does.
        QFile metadata(channelsPath);
        CHECK(metadata.open(QIODevice::ReadOnly));
        const QByteArray metadataBytes = metadata.readAll();
        metadata.close();

        const QJsonArray keyRows = readJsonArray(keysPath);
        CHECK_EQ(keyRows.size(), 1);
        QByteArray privateKey;
        if (keyRows.size() == 1)
            privateKey = QByteArray::fromHex(
                keyRows.at(0).toObject().value(QStringLiteral("privateKey"))
                    .toString().toLatin1());
        CHECK_EQ(privateKey.size(), int(Kelp::Channel::PrivateKeySize));

        // Hex and raw are both checked: a future writer might encode differently,
        // and either form in the metadata file is the leak that matters.
        CHECK(!metadataBytes.contains(privateKey.toHex()));
        CHECK(!metadataBytes.contains(privateKey));
        CHECK(!metadataBytes.contains("privateKey"));

        // The read secret is supposed to be there — the link is rebuilt from it.
        CHECK(metadataBytes.contains(readSecret.toHex()));
    }

    section("the link survives a restart");
    {
        KelpChannels channels(&store);
        channels.setRelays(QStringList() << QStringLiteral("kelp.example:9443")
                                         << QStringLiteral("https://relay.example:9443"));
        CHECK_EQ(channels.link(channelId), firstLink);
        CHECK(channels.link(QStringLiteral("aaaa")).isEmpty());
        // An upper-case paste is the same channel, not a different one.
        CHECK_EQ(channels.link(channelId.toUpper()), firstLink);
    }

    section("publish moves the pointer to the next bundle");
    {
        const QString firstPointer = QJsonArray(readJsonArray(channelsPath))
            .at(0).toObject().value(QStringLiteral("pointers")).toArray()
            .at(0).toString();

        KelpChannels channels(&store);
        channels.setRelays(QStringList() << QStringLiteral("kelp.example:9443")
                                         << QStringLiteral("https://relay.example:9443"));
        const QVariantMap published = channels.publish(channelId, bundleB, secretB);
        CHECK(published.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(published.value(QStringLiteral("bundleId")).toString(), bundleB);
        CHECK_EQ(published.value(QStringLiteral("sequence")).toULongLong(),
                 Q_UINT64_C(2));
        CHECK_EQ(published.value(QStringLiteral("id")).toString(), channelId);
        CHECK_EQ(published.value(QStringLiteral("link")).toString(), firstLink);
        CHECK(published.value(QStringLiteral("pointer")).toString() != firstPointer);

        // The store really holds the sealed pointer the row names.
        const QByteArray pointerId = QByteArray::fromHex(
            published.value(QStringLiteral("pointer")).toString().toLatin1());
        CHECK_EQ(pointerId.size(), int(Kelp::Crypto::Sha256Size));
        CHECK(store.contains(pointerId));

        const QVariantList rows = channels.load();
        CHECK_EQ(rows.size(), 1);
        CHECK_EQ(rows.at(0).toMap().value(QStringLiteral("bundleId")).toString(),
                 bundleB);
        CHECK_EQ(rows.at(0).toMap().value(QStringLiteral("sequence")).toULongLong(),
                 Q_UINT64_C(2));
        CHECK_EQ(rows.at(0).toMap().value(QStringLiteral("timestamp")).toLongLong() > 0,
                 true);

        // History is newest-first and keeps at most five entries.
        const QJsonObject stored = readJsonArray(channelsPath).at(0).toObject();
        CHECK_EQ(stored.value(QStringLiteral("pointers")).toArray().size(), 2);
        CHECK_EQ(stored.value(QStringLiteral("pointers")).toArray().at(0).toString(),
                 published.value(QStringLiteral("pointer")).toString());
    }

    section("a sealed pointer opens with the read secret and verifies");
    {
        // The publisher's own record must be a pointer a recipient can actually
        // use: open it with the link secret and check the channel id.
        const QJsonArray rows = readJsonArray(channelsPath);
        const QJsonArray pointers =
            rows.at(0).toObject().value(QStringLiteral("pointers")).toArray();
        const QByteArray pointerId =
            QByteArray::fromHex(pointers.at(0).toString().toLatin1());
        QByteArray objectBytes;
        CHECK(store.get(pointerId, &objectBytes));

        Kelp::Channel::Pointer pointer;
        CHECK(Kelp::Channel::open(objectBytes, readSecret, channelRawId, &pointer, &error));
        CHECK_EQ(pointer.name, QStringLiteral("notes"));
        CHECK_EQ(QString::fromLatin1(Kelp::Link::base32Encode(pointer.bundleId)),
                 bundleB);
        CHECK_EQ(pointer.sequence, Q_UINT64_C(2));
    }

    section("refusals");
    {
        KelpChannels channels(&store);

        QVariantMap result = channels.create(QStringLiteral("notes"), bundleB, secretB);
        CHECK(!result.value(QStringLiteral("ok")).toBool());
        CHECK(result.value(QStringLiteral("error")).toString().contains(
            QStringLiteral("already exists")));

        result = channels.create(QStringLiteral("   "), bundleB, secretB);
        CHECK(!result.value(QStringLiteral("ok")).toBool());

        result = channels.create(QStringLiteral("other"), QStringLiteral("not-a-bundle"), secretA);
        CHECK(!result.value(QStringLiteral("ok")).toBool());

        result = channels.create(QStringLiteral("other"), QString::fromLatin1(
            Kelp::Link::base32Encode(QByteArray(32, 'x'))), secretA);    // valid id, not stored
        CHECK(!result.value(QStringLiteral("ok")).toBool());

        result = channels.publish(channelId, QStringLiteral("not-a-bundle"), secretA);
        CHECK(!result.value(QStringLiteral("ok")).toBool());
        result = channels.publish(QString::fromLatin1(
            Kelp::Link::base32Encode(QByteArray(32, 'y'))), bundleA, secretA);
        CHECK(!result.value(QStringLiteral("ok")).toBool());
    }

    section("a damaged key store is refused, not overwritten");
    {
        QByteArray original;
        {
            QFile file(keysPath);
            CHECK(file.open(QIODevice::ReadOnly));
            original = file.readAll();
        }
        {
            QFile file(keysPath);
            CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("{ this is not json");
        }

        KelpChannels channels(&store);
        const QVariantMap result = channels.publish(channelId, bundleA, secretA);
        CHECK(!result.value(QStringLiteral("ok")).toBool());

        // load() still shows the row (the metadata is intact) but says the key
        // is gone, which is exactly what the UI needs to warn about.
        const QVariantList rows = channels.load();
        CHECK_EQ(rows.size(), 1);
        CHECK(!rows.at(0).toMap().value(QStringLiteral("hasKey")).toBool());

        // Creating a new channel must not clobber the unreadable key file: that
        // would destroy the only copy of an older channel's signing key.
        const QVariantMap created = channels.create(QStringLiteral("second"), bundleA, secretA);
        CHECK(!created.value(QStringLiteral("ok")).toBool());

        QFile restore(keysPath);
        CHECK(restore.open(QIODevice::WriteOnly | QIODevice::Truncate));
        restore.write(original);
        restore.close();
    }

    section("a damaged channel list is refused, not overwritten");
    {
        QByteArray original;
        {
            QFile file(channelsPath);
            CHECK(file.open(QIODevice::ReadOnly));
            original = file.readAll();
        }
        {
            QFile file(channelsPath);
            CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("{ this is not json");
        }

        KelpChannels channels(&store);
        CHECK_EQ(channels.load().size(), 0);
        CHECK(channels.link(channelId).isEmpty());
        CHECK(!channels.publish(channelId, bundleA, secretA).value(QStringLiteral("ok")).toBool());
        CHECK(!channels.create(QStringLiteral("second"), bundleA, secretA)
                   .value(QStringLiteral("ok")).toBool());

        QFile restore(channelsPath);
        CHECK(restore.open(QIODevice::WriteOnly | QIODevice::Truncate));
        restore.write(original);
        restore.close();
    }

    section("one malformed row protects the whole list");
    {
        QByteArray original;
        {
            QFile file(channelsPath);
            CHECK(file.open(QIODevice::ReadOnly));
            original = file.readAll();
        }
        QJsonArray rows = QJsonDocument::fromJson(original).array();
        QJsonObject broken;
        broken.insert(QStringLiteral("id"), channelId);
        broken.insert(QStringLiteral("name"), QStringLiteral("broken"));
        rows.append(broken);
        {
            QFile file(channelsPath);
            CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(rows).toJson());
        }

        KelpChannels channels(&store);
        // The good row is not shown alongside a bad one: the caller cannot tell
        // which row an action would affect, and a write must not drop the bad one.
        CHECK_EQ(channels.load().size(), 0);
        CHECK(channels.link(channelId).isEmpty());
        CHECK(!channels.create(QStringLiteral("second"), bundleA, secretA)
                   .value(QStringLiteral("ok")).toBool());

        QFile restore(channelsPath);
        CHECK(restore.open(QIODevice::WriteOnly | QIODevice::Truncate));
        restore.write(original);
        restore.close();
    }

    section("the sealed pointer carries the bundle key");
    {
        // A reader of the channel link can open the current bundle, so the key
        // the pointer carries must be the one the share row holds.
        QByteArray expectedKey;
        QString error;
        CHECK(Kelp::Link::base64UrlDecode(secretB, &expectedKey, &error));

        KelpChannels channels(&store);
        const QVariantList rows = channels.load();
        CHECK_EQ(rows.size(), 1);
        CHECK(!rows.at(0).toMap().contains(QStringLiteral("bundleSecret")));

        const QJsonArray stored = readJsonArray(channelsPath);
        const QJsonArray pointers =
            stored.at(0).toObject().value(QStringLiteral("pointers")).toArray();
        const QByteArray pointerId =
            QByteArray::fromHex(pointers.at(0).toString().toLatin1());
        QByteArray objectBytes;
        CHECK(store.get(pointerId, &objectBytes));

        Kelp::Channel::Pointer pointer;
        CHECK(Kelp::Channel::open(objectBytes, readSecret, channelRawId, &pointer, &error));
        QByteArray rawBundleB;
        CHECK(Kelp::Link::base32Decode(bundleB, &rawBundleB, &error));
        CHECK_EQ(pointer.bundleId, rawBundleB);
        CHECK_EQ(pointer.bundleSecret, expectedKey);

        // A key of the wrong width is refused before anything is written.
        const QVariantMap badKey = channels.publish(
            channelId, bundleB, Kelp::Link::base64UrlEncode(QByteArray(16, 'k')));
        CHECK(!badKey.value(QStringLiteral("ok")).toBool());
        CHECK_EQ(channels.load().at(0).toMap().value(QStringLiteral("sequence")).toULongLong(),
                 Q_UINT64_C(2));
    }

    section("relay announcements");
    {
        RecordingSink sink;
        KelpChannels channels(&store);
        channels.setSink(&sink);

        // A channel with no sink is recorded but reports that no relay has it.
        KelpChannels bare(&store);
        const QVariantMap unrelayed = bare.create(QStringLiteral("unrelayed"), bundleA, secretA);
        CHECK(unrelayed.value(QStringLiteral("ok")).toBool());
        CHECK(!unrelayed.value(QStringLiteral("relayed")).toBool());

        const QVariantMap created = channels.create(QStringLiteral("relayed"), bundleA, secretA);
        CHECK(created.value(QStringLiteral("ok")).toBool());
        CHECK(created.value(QStringLiteral("relayed")).toBool());
        CHECK_EQ(sink.updates.size(), 1);

        QByteArray createdId;
        CHECK(Kelp::Link::base32Decode(created.value(QStringLiteral("id")).toString(),
                                       &createdId, &error));
        Kelp::Channel::Update update;
        CHECK(Kelp::Channel::decodeUpdate(sink.updates.value(0), &update, &error));
        CHECK(Kelp::Channel::verifyUpdate(update, &error));
        CHECK_EQ(update.sequence, Q_UINT64_C(1));
        CHECK_EQ(update.channelId, sink.ids.value(0));
        CHECK_EQ(update.channelId, createdId);

        // The relayed pointer is the one the store holds, byte for byte.
        QByteArray stored;
        const QByteArray pointerId = QByteArray::fromHex(
            created.value(QStringLiteral("pointer")).toString().toLatin1());
        CHECK(store.get(pointerId, &stored));
        CHECK_EQ(update.pointer, stored);

        sink.updates.clear();
        sink.ids.clear();
        const QVariantMap moved = channels.publish(
            created.value(QStringLiteral("id")).toString(), bundleB, secretB);
        CHECK(moved.value(QStringLiteral("relayed")).toBool());
        CHECK_EQ(sink.updates.size(), 1);
        CHECK(Kelp::Channel::decodeUpdate(sink.updates.value(0), &update, &error));
        CHECK(Kelp::Channel::verifyUpdate(update, &error));
        CHECK_EQ(update.sequence, Q_UINT64_C(2));

        // After a restart the publisher puts every current pointer back up.
        sink.updates.clear();
        sink.ids.clear();
        const int channelCount = channels.load().size();
        CHECK(channelCount >= 2);
        CHECK_EQ(channels.reannounce(), channelCount);
        CHECK_EQ(sink.updates.size(), channelCount);
        for (int i = 0; i < sink.updates.size(); ++i) {
            Kelp::Channel::Update again;
            CHECK(Kelp::Channel::decodeUpdate(sink.updates.at(i), &again, &error));
            CHECK(Kelp::Channel::verifyUpdate(again, &error));
        }
    }

    QFile::remove(channelsPath);
    QFile::remove(keysPath);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    printf("kelpchannels unit test\n");

    // Redirect the app data directory before anything asks for it, and use a
    // name of our own so a developer's real channels are never touched.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("org.kapitaali"));
    QCoreApplication::setApplicationName(QStringLiteral("harbour-kelp-channels-test"));
    dataDirectory = KelpLog::dataDirectory();

    testChannels();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
