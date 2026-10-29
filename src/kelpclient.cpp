#include "kelpclient.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#include "kelp_bundle.h"
#include "kelp_crypto.h"
#include "kelpfetch.h"
#include "kelp_link.h"
#include "kelp_manifest.h"
#include "kelp_object.h"
#include "kelp_store.h"
#include "kelp_web.h"
#include "kelpbuild.h"
#include "kelppreviewserver.h"

namespace {

// How many object requests may be in flight at once. On a LAN the round trip is
// tiny, so this is about keeping the pipe full rather than politeness. A relay
// answer is sequential inside one tunnel, so this mostly matters for the LAN.
const int kMaxInflight = 4;

// If no reply finishes for this long the sender has stopped answering — a peer
// that accepts the connection and then says nothing — and the fetch ends instead
// of leaving a spinner up forever.
const int kSilenceMs = 20000;

// How much a *received webpage* may keep in memory (design §5.5). Enough for a
// document-shaped page, not for a gallery. It is enforced against what actually
// arrives: the client caps the manifest index as it stores it, and the preview
// session caps the content it fetches on demand. The sender's total-size claim
// is *not* a gate — a large bundle can have a small index page, and the browser
// asks only for the assets it needs.
const quint64 kViewCeilingBytes = 32ull * 1024 * 1024;

QString unitText(quint64 bytes, quint64 scale, const char *suffix)
{
    const double value = double(bytes) / double(scale);
    const qint64 whole = qint64(value);
    return double(whole) == value
        ? QStringLiteral("%1 %2").arg(whole).arg(QLatin1String(suffix))
        : QStringLiteral("%1 %2").arg(QString::number(value, 'f', 1))
              .arg(QLatin1String(suffix));
}

QString sizeText(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return unitText(bytes, 1024ull * 1024 * 1024, "GB");
    if (bytes >= 1024ull * 1024)
        return unitText(bytes, 1024ull * 1024, "MB");
    if (bytes >= 1024)
        return unitText(bytes, 1024, "kB");
    return QStringLiteral("%1 bytes").arg(bytes);
}

QString overCeiling(quint64 ceiling, quint64 bytes)
{
    return QStringLiteral("This page is %1 — more than the %2 a shared page can be "
                          "viewed with. Ask the sender to share it as a folder instead.")
        .arg(sizeText(bytes), sizeText(ceiling));
}

// The name in a link becomes a folder name, so it is stripped of anything that
// means something to a filesystem, and of leading dots (hidden, and "..").
QString sanitiseName(const QString &name)
{
    QString out;
    for (int i = 0; i < name.size() && out.size() < 64; ++i) {
        const QChar c = name.at(i);
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c.unicode() < 0x20)
            continue;
        out.append(c);
    }
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    return out.trimmed();
}

// A free path beside `parent`: "name", then "name (2)", "name (3)"… A file keeps
// its extension and takes the number in front of it, the way a browser saves a
// second copy.
QString uniquePath(const QString &parent, const QString &name, bool isDir)
{
    QString stem = name;
    QString suffix;
    if (!isDir) {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) {
            stem = name.left(dot);
            suffix = name.mid(dot);
        }
    }

    QString candidate = QDir(parent).filePath(name);
    for (int n = 2; QFileInfo::exists(candidate) && n < 1000; ++n) {
        candidate = QDir(parent).filePath(QStringLiteral("%1 (%2)%3")
            .arg(stem).arg(n).arg(suffix));
    }
    return candidate;
}

// The same depth limit libkelp's opener uses. Keeping it here means a hostile
// manifest cannot make the writer recurse without bound even though the writer
// walks the manifests itself instead of asking open() for the whole tree.
const int kMaxTreeDepth = 128;

/*
 * Writes a received bundle one file at a time.
 *
 * `Kelp::open()` reassembles the whole tree, file data included, before the
 * caller can write anything. That is fine for the small bundles the first
 * tests used, but a 335 MiB folder is a `std::bad_alloc` on a low-memory
 * device. This walks the same manifests again, but keeps only the current
 * chunk in memory: a file is opened when its manifest says how big it is, each
 * chunk is verified and written as it arrives, and the file is closed before
 * the next one starts.
 *
 * The walk repeats the checks the opener does — manifest ids, chunk sizes,
 * subtree sizes, and names from the sender — so a bundle that open() would
 * refuse is refused here too. The difference is only that content is not
 * accumulated.
 */
class TreeWriter
{
public:
    TreeWriter(Kelp::ObjectStore *store, const QByteArray &key)
        : m_store(store), m_key(key), m_files(0), m_bytes(0) {}

    bool writeRoot(const QByteArray &rootId, const QString &base,
                   const QString &label, QString *destination, QString *error)
    {
        if (rootId.size() != Kelp::Crypto::Sha256Size) {
            *error = QStringLiteral("bundle id is not a 32-byte object id");
            return false;
        }

        QVector<Kelp::Manifest::Entry> roots;
        if (!readDirectory(rootId, 0, &roots, error))
            return false;

        // A single file becomes that file in the chosen folder; a single folder
        // becomes a folder named for the link, holding what the picked folder
        // held; anything else becomes a folder named for the link. This is the
        // same shape writeFiles() documented before the streaming writer.
        if (roots.size() == 1 && !roots.at(0).isDir) {
            if (!Kelp::Manifest::isValidName(roots.at(0).name)
                || roots.at(0).name.size() > 255) {
                *error = QStringLiteral("the sender used a name that cannot be saved");
                return false;
            }
            const QString path = uniquePath(base, roots.at(0).name, false);
            if (QFileInfo::exists(path)) {
                *error = QStringLiteral("there are already too many copies of this in %1")
                             .arg(QDir(base).dirName());
                return false;
            }
            *destination = path;
            if (!QDir().mkpath(QFileInfo(path).path())) {
                *error = QStringLiteral("could not make a folder to save into");
                return false;
            }
            quint64 size = 0;
            return writeFile(roots.at(0), path, 1, &size, error);
        }

        const QString path = uniquePath(base, label, true);
        if (QFileInfo::exists(path)) {
            *error = QStringLiteral("there are already too many copies of this in %1")
                         .arg(QDir(base).dirName());
            return false;
        }
        *destination = path;
        if (!QDir().mkpath(path)) {
            *error = QStringLiteral("could not make a folder to save into");
            return false;
        }

        quint64 total = 0;
        if (roots.size() == 1) {
            QVector<Kelp::Manifest::Entry> inside;
            if (!readDirectory(roots.at(0).hash, 1, &inside, error))
                return false;
            if (!writeEntries(inside, path, 1, &total, error))
                return false;
            if (total != roots.at(0).size) {
                *error = QStringLiteral("\"%1\" does not match the size in its parent")
                             .arg(roots.at(0).name);
                return false;
            }
        } else if (!writeEntries(roots, path, 0, &total, error)) {
            return false;
        }

        return true;
    }

    int files() const { return m_files; }
    quint64 bytes() const { return m_bytes; }

private:
    bool readObject(const QByteArray &id, QByteArray *plaintext, QString *error)
    {
        QByteArray object;
        if (!m_store->get(id, &object)) {
            *error = QStringLiteral("object %1 is missing or corrupt")
                         .arg(QString::fromLatin1(id.toHex()));
            return false;
        }
        if (!Kelp::Object::open(m_key, object, plaintext, error)) {
            if (error->isEmpty())
                *error = QStringLiteral("an object failed to decrypt");
            return false;
        }
        return true;
    }

    bool readDirectory(const QByteArray &id, int depth,
                       QVector<Kelp::Manifest::Entry> *entries, QString *error)
    {
        if (depth > kMaxTreeDepth) {
            *error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        QByteArray manifest;
        if (!readObject(id, &manifest, error))
            return false;

        Kelp::Manifest::Directory directory;
        if (!Kelp::Manifest::decodeDirectory(manifest, &directory, error))
            return false;

        if (!directory.segment) {
            *entries = directory.entries;
            return true;
        }

        for (int i = 0; i < directory.children.size(); ++i) {
            QByteArray leafManifest;
            if (!readObject(directory.children.at(i), &leafManifest, error))
                return false;
            Kelp::Manifest::Directory leaf;
            if (!Kelp::Manifest::decodeDirectory(leafManifest, &leaf, error))
                return false;
            if (leaf.segment) {
                *error = QStringLiteral("segment index points at another segment index");
                return false;
            }
            entries->append(leaf.entries);
        }
        return true;
    }

    bool writeEntries(const QVector<Kelp::Manifest::Entry> &entries,
                      const QString &dir, int depth, quint64 *total, QString *error)
    {
        if (depth > kMaxTreeDepth) {
            *error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        quint64 sum = 0;
        for (int i = 0; i < entries.size(); ++i) {
            const Kelp::Manifest::Entry &entry = entries.at(i);
            if (!Kelp::Manifest::isValidName(entry.name) || entry.name.size() > 255) {
                *error = QStringLiteral("the sender used a name that cannot be saved");
                return false;
            }

            const QString path = dir + QLatin1Char('/') + entry.name;
            if (entry.isDir) {
                if (!QDir().mkpath(path)) {
                    *error = QStringLiteral("could not make the folder %1").arg(entry.name);
                    return false;
                }
                QVector<Kelp::Manifest::Entry> children;
                if (!readDirectory(entry.hash, depth + 1, &children, error))
                    return false;
                quint64 childTotal = 0;
                if (!writeEntries(children, path, depth + 1, &childTotal, error))
                    return false;
                if (childTotal != entry.size) {
                    *error = QStringLiteral("\"%1\" does not match the size in its parent")
                                 .arg(entry.name);
                    return false;
                }
                sum += childTotal;
            } else {
                quint64 written = 0;
                if (!writeFile(entry, path, depth + 1, &written, error))
                    return false;
                sum += written;
            }
        }
        *total = sum;
        return true;
    }

    bool writeFile(const Kelp::Manifest::Entry &entry, const QString &path,
                   int depth, quint64 *size, QString *error)
    {
        if (depth > kMaxTreeDepth) {
            *error = QStringLiteral("bundle tree is nested too deeply");
            return false;
        }

        QByteArray manifest;
        if (!readObject(entry.hash, &manifest, error))
            return false;

        Kelp::Manifest::File file;
        if (!Kelp::Manifest::decodeFile(manifest, &file, error))
            return false;
        if (file.size != entry.size) {
            *error = QStringLiteral("\"%1\" does not match the size in its parent")
                         .arg(entry.name);
            return false;
        }

        QFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            *error = QStringLiteral("could not write %1").arg(entry.name);
            return false;
        }

        for (int i = 0; i < file.chunks.size(); ++i) {
            const quint64 offset = quint64(i) * Kelp::Manifest::ChunkSize;
            const quint64 remaining = file.size - offset;
            const quint64 expected =
                qMin<quint64>(Kelp::Manifest::ChunkSize, remaining);

            QByteArray chunk;
            if (!readObject(file.chunks.at(i), &chunk, error)) {
                out.close();
                return false;
            }
            if (quint64(chunk.size()) != expected) {
                *error = QStringLiteral("a chunk does not have the size the manifest promises");
                out.close();
                return false;
            }
            if (out.write(chunk) != chunk.size()) {
                *error = QStringLiteral("could not write all of %1").arg(entry.name);
                out.close();
                return false;
            }
        }
        out.close();

        ++m_files;
        m_bytes += file.size;
        *size = file.size;
        return true;
    }

    Kelp::ObjectStore *m_store;
    QByteArray m_key;
    int m_files;
    quint64 m_bytes;
};

} // namespace

KelpClient::KelpClient(Kelp::ObjectStore *store, QObject *parent)
    : QObject(parent),
      m_store(store),
      m_source(nullptr),
      m_preview(nullptr),
      m_settings(nullptr),
      m_deadline(new QTimer(this)),
      m_viewing(false),
      m_viewBytes(0),
      m_viewCeiling(kViewCeilingBytes),
      m_phase(Idle),
      m_total(0),
      m_fetched(0),
      m_storedThisRound(0),
      m_rounds(0),
      m_files(0),
      m_bytes(0)
{
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(kSilenceMs);
    connect(m_deadline, &QTimer::timeout, this, &KelpClient::timedOut);
}

void KelpClient::setPreviewServer(KelpPreviewServer *preview)
{
    m_preview = preview;
}

void KelpClient::setViewCeiling(quint64 bytes)
{
    m_viewCeiling = bytes;
}

void KelpClient::setSettings(KelpSettings *settings)
{
    m_settings = settings;
}

Kelp::ObjectStore *KelpClient::activeStore() const
{
    return m_viewing ? m_viewStore.data() : m_store;
}

QString KelpClient::phase() const
{
    switch (m_phase) {
    case Checking:
        return QStringLiteral("checking");
    case Fetching:
        return QStringLiteral("fetching");
    case Saving:
        return QStringLiteral("saving");
    case Done:
        return QStringLiteral("done");
    case Failed:
        return QStringLiteral("failed");
    case Idle:
        break;
    }
    return QStringLiteral("idle");
}

bool KelpClient::busy() const
{
    return m_phase == Checking || m_phase == Fetching || m_phase == Saving;
}

void KelpClient::setDirectory(const QString &path)
{
    if (path == m_directory)
        return;
    m_directory = path;
    qInfo() << "KelpClient: download folder set to"
            << (path.isEmpty() ? QStringLiteral("Downloads (default)") : path);
    emit changed();
}

void KelpClient::dropSource()
{
    m_deadline->stop();
    m_queue.clear();
    m_inflight.clear();
    if (m_source) {
        // Disconnected first: this is not a failure to report, it is us leaving.
        m_source->disconnect(this);
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
}

void KelpClient::reset()
{
    dropSource();
    m_error.clear();
    m_name.clear();
    m_sourceLabel.clear();
    m_destination.clear();
    m_total = 0;
    m_fetched = 0;
    m_storedThisRound = 0;
    m_rounds = 0;
    m_files = 0;
    m_bytes = 0;
    m_phase = Idle;
    // A view's own bookkeeping goes with it, and its memory is released here if
    // nothing else holds it: any preview session that was serving it holds a
    // reference of its own and keeps working.
    m_viewing = false;
    m_link.clear();
    m_fetchLink = Kelp::Link::Bundle();
    m_relayRetried = false;
    m_viewLink = Kelp::Link::Bundle();
    m_viewSecret.clear();
    m_viewToken.clear();
    m_viewUrl.clear();
    m_viewBytes = 0;
    m_viewStore.clear();
    emit changed();
}

void KelpClient::cancel()
{
    const bool wasBusy = busy();
    dropSource();
    if (m_viewing) {
        // Cancel only reaches a view whose manifests are still arriving: once
        // Done the session owns the source and the client is no longer busy, so
        // there is no session to disturb.
        m_viewBytes = 0;
        m_viewStore.clear();
    }
    if (wasBusy) {
        m_phase = Idle;
        qInfo() << "KelpClient: fetch cancelled after" << m_fetched << "objects";
        emit changed();
    }
}

bool KelpClient::failed(const QString &message)
{
    dropSource();
    m_error = message;
    m_phase = Failed;
    if (m_viewing) {
        // Nothing will serve a view that failed, so let its memory go now rather
        // than at the next reset.
        m_viewBytes = 0;
        m_viewStore.clear();
    }
    qWarning() << "KelpClient: fetch failed:" << message
               << "(objects stored:" << m_fetched << ")";
    emit changed();
    return false;
}

bool KelpClient::fetch(const QString &linkText, const QString &passphrase)
{
    return beginFetch(linkText, false, passphrase);
}

bool KelpClient::fetchForViewing(const QString &linkText, const QString &passphrase)
{
    return beginFetch(linkText, true, passphrase);
}

bool KelpClient::beginFetch(const QString &linkText, bool viewing,
                            const QString &passphrase)
{
    if (busy()) {
        m_error = QStringLiteral("another fetch is still running");
        emit changed();
        return false;
    }
    reset();

    // The link this fetch is about, trimmed exactly as it is decoded below: the
    // identity a page asks by when it needs to know whether what it sees in this
    // client is its own fetch or a visit that came before it (§3.25).
    m_link = linkText.trimmed();

    // A view gets a store of its own, on the heap and shared with whatever ends
    // up serving it: an earlier session, if any, is untouched by this (design
    // §5.5, and C4a2-plan.md D4a).
    m_viewing = viewing;
    if (m_viewing) {
        m_viewStore = QSharedPointer<Kelp::MemoryStore>::create();
        m_viewBytes = 0;
    }

    if (!activeStore())
        return failed(QStringLiteral("no object store to fetch into"));

    Kelp::Link::Bundle link;
    QString error;
    if (!Kelp::Link::decode(linkText.trimmed(), &link, &error))
        return failed(error);

    // A passphrase link (design §12.3) carries no key at all: the secret is
    // wrapped under the passphrase, and until it is unwrapped there is nothing to
    // fetch and no address to fetch from — the hints are sealed under that same
    // secret. So this happens before a source is created: a wrong passphrase costs
    // the derivation and not a network round trip.
    if (Kelp::Link::needsPassphrase(link)) {
        if (passphrase.isEmpty())
            return failed(QStringLiteral("this link needs a passphrase"));
        if (!Kelp::Link::unlock(&link, passphrase, &error))
            return failed(error);
    }

    // Kept for the whole fetch: a link that names both an address and a relay is
    // retried on the relay if the address cannot answer (see onFailed()). After
    // unlock(), so a passphrase link's retry has the secret the first attempt used.
    m_fetchLink = link;
    m_relayRetried = false;

    // The LAN address is preferred when the link has one — it needs no third
    // party — and a relay is the fallback for everything wider than one network.
    KelpSource *source = KelpSource::create(link, this, &error);
    if (!source)
        return failed(error);

    const QByteArray key = Kelp::contentKey(link.secret);
    if (key.size() != Kelp::Crypto::KeySize) {
        source->deleteLater();
        return failed(QStringLiteral("could not derive a key from this link"));
    }

    m_source = source;
    m_key = key;
    m_bundleId = link.bundleId;
    m_bundleIdText = QString::fromLatin1(Kelp::Link::base32Encode(link.bundleId));
    m_name = sanitiseName(link.name);
    m_sourceLabel = source->describe();
    if (m_viewing) {
        m_viewSecret = Kelp::Link::base64UrlEncode(link.secret);
        // Kept, not logged: a swept session may need a fresh source for the
        // same page, and a passphrase link cannot be re-dialled from its text
        // alone. The secret is already in memory for the whole view.
        m_viewLink = link;
    }
    m_phase = Checking;
    emit changed();

    qInfo() << "KelpClient: fetching" << m_bundleIdText.left(16) << "from"
            << m_sourceLabel << "as"
            << (m_name.isEmpty() ? QStringLiteral("(unnamed)") : m_name)
            << (m_viewing ? QStringLiteral("(a webpage: nothing will be written)")
                          : QString());

    connect(source, &KelpSource::infoReady, this, &KelpClient::onInfo);
    connect(source, &KelpSource::objectReady, this, &KelpClient::onObject);
    connect(source, &KelpSource::failed, this, &KelpClient::onFailed);

    m_deadline->start();
    source->begin();
    return true;
}

void KelpClient::onInfo(int objects, int files, quint64 bytes)
{
    Q_UNUSED(files);
    Q_UNUSED(bytes);
    if (m_phase != Checking)
        return;

    // The sender's total-size claim is not a gate for a view: a large bundle may
    // hold a small page, and the browser asks only for the assets it needs. The
    // claim is not a promise anyway. What is actually kept is bounded instead —
    // the manifest walk by onObject(), the content by the preview session.
    //
    // An ordinary fetch still uses the claim only for progress; what is
    // downloaded and accepted is decided by the walk.

    m_total = qMax(0, objects);

    m_phase = Fetching;
    emit changed();
    walk();
}

void KelpClient::onFailed(const QString &message)
{
    if (m_phase == Idle || m_phase == Done || m_phase == Failed)
        return;

    // LAN first, relay second. A link minted by a publisher that has both names
    // both, and the address is only useful on the publisher's own network: a
    // phone on mobile data advertises a 10.x or 100.64.x address nothing else can
    // route to. `KelpSource::create` prefers the address and stops there, so an
    // off-network recipient would otherwise be told the sender stopped answering
    // while the link carried a perfectly good relay.
    //
    // Only before the sender has said what it holds: at that point nothing has
    // been asked for, so restarting on the relay cannot duplicate or half-fill a
    // bundle. `fromRelay()` keeps a relay source from being retried on a relay.
    if (m_phase == Checking && m_source && !m_source->fromRelay() && !m_relayRetried
        && !m_fetchLink.relays.isEmpty()) {
        retryOnRelay();
        return;
    }

    failed(message);
}

// Starts the same fetch again over the link's first relay. The link itself is
// unchanged — only the path to the publisher — so the bundle id, the key and the
// password-derived secret stay exactly as beginFetch() made them.
bool KelpClient::retryOnRelay()
{
    m_relayRetried = true;

    // Leaving the address source is not a failure to report: drop it silently,
    // then let the same walk begin again over the relay.
    dropSource();

    QString error;
    KelpSource *source = KelpSource::createFromRelay(m_fetchLink, this, &error);
    if (!source)
        return failed(error);

    m_source = source;
    m_sourceLabel = source->describe();
    m_phase = Checking;
    m_rounds = 0;
    m_storedThisRound = 0;
    emit changed();

    qInfo() << "KelpClient: the link's address did not answer; trying"
            << m_sourceLabel << "for" << m_bundleIdText.left(16);

    connect(source, &KelpSource::infoReady, this, &KelpClient::onInfo);
    connect(source, &KelpSource::objectReady, this, &KelpClient::onObject);
    connect(source, &KelpSource::failed, this, &KelpClient::onFailed);

    m_deadline->start();
    source->begin();
    return true;
}

void KelpClient::walk()
{
    // A round that stored nothing can never finish: the next walk would ask for
    // exactly the same objects again.
    if (m_rounds > 0 && m_storedThisRound == 0) {
        failed(QStringLiteral("the objects that arrived could not be stored"));
        return;
    }
    m_storedThisRound = 0;
    ++m_rounds;

    // A view fetches the shape first: the root and every manifest name the page
    // and its paths, and the browser then asks the session for the data chunks it
    // actually needs (design §5.1). An ordinary fetch still walks everything.
    QVector<QByteArray> missing;
    QString error;
    const bool walked = m_viewing
        ? Kelp::missingManifests(activeStore(), m_key, m_bundleId, &missing, &error)
        : Kelp::missingObjects(activeStore(), m_key, m_bundleId, &missing, &error);
    if (!walked) {
        failed(error);
        return;
    }

    if (missing.isEmpty()) {
        if (m_viewing)
            finishViewing();
        else {
            qInfo() << "KelpClient: all" << m_fetched << "objects are here after"
                    << m_rounds << "rounds";
            writeFiles();
        }
        return;
    }

    qInfo() << "KelpClient: round" << m_rounds << "needs" << missing.size() << "objects"
            << "(have" << m_fetched << ")";
    for (int i = 0; i < missing.size(); ++i)
        m_queue.enqueue(missing.at(i));

    // Walking the tree is our own work, and it reads the disk: the deadline is
    // the sender's, so it starts when we ask, not when we last heard from it.
    m_deadline->start();
    pump();
}

void KelpClient::pump()
{
    while (!m_queue.isEmpty() && m_inflight.size() < kMaxInflight) {
        const QByteArray id = m_queue.dequeue();
        m_inflight.insert(id);
        m_source->requestObject(id);
    }

    // Nothing left to ask for: back to the walk, which either names the next
    // round or finds the bundle complete.
    if (m_queue.isEmpty() && m_inflight.isEmpty() && m_phase == Fetching)
        walk();
}

void KelpClient::onObject(const QByteArray &id, const QByteArray &bytes)
{
    if (m_phase != Fetching)
        return;
    m_inflight.remove(id);

    m_deadline->start();            // progress: silence starts a fresh deadline

    if (!Kelp::storeReceived(activeStore(), id, bytes)) {
        failed(QStringLiteral("something arrived that was not the object asked for"));
        return;
    }

    // A view is held in memory, so the ceiling is enforced against what actually
    // arrives as well: /info is the sender's claim, not a promise.
    if (m_viewing) {
        m_viewBytes += quint64(bytes.size());
        if (m_viewBytes > m_viewCeiling) {
            qInfo() << "KelpClient: a page over the ceiling after" << m_fetched
                    << "objects:" << sizeText(m_viewBytes);
            failed(overCeiling(m_viewCeiling, m_viewBytes));
            return;
        }
    }

    ++m_fetched;
    ++m_storedThisRound;
    emit changed();
    pump();
}

void KelpClient::timedOut()
{
    if (busy())
        failed(QStringLiteral("the sender stopped answering"));
}

void KelpClient::writeFiles()
{
    m_deadline->stop();
    m_phase = Saving;
    emit changed();

    // Where the files are written. The user picks this on the link page; an
    // empty choice means the platform's Downloads folder.
    QString base = m_directory;
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/Downloads");
    }

    const QString label = m_name.isEmpty() ? m_bundleIdText.left(8) : m_name;
    TreeWriter writer(m_store, m_key);
    QString destination;
    QString error;
    if (!writer.writeRoot(m_bundleId, base, label, &destination, &error)) {
        m_destination = destination;
        m_files = writer.files();
        m_bytes = writer.bytes();
        failed(error);
        return;
    }

    m_destination = destination;
    m_files = writer.files();
    m_bytes = writer.bytes();
    m_phase = Done;
    qInfo() << "KelpClient: wrote" << m_files << "files," << m_bytes << "bytes to"
            << destination;
    emit changed();
}

bool KelpClient::isHandling(const QString &link) const
{
    const QString trimmed = link.trimmed();
    return m_phase != Idle && !trimmed.isEmpty() && trimmed == m_link;
}

void KelpClient::finishViewing()
{
    // No files, no folder, no destination. The manifests are here, which is all
    // that is needed to resolve the page and answer its paths; the data chunks are
    // fetched by the preview session as the browser asks for them (design §5.1).
    m_deadline->stop();
    m_phase = Done;
    qInfo() << "KelpClient: page index holds" << m_fetched << "objects,"
            << sizeText(m_viewBytes) << "in memory; content will be fetched on demand";
    emit changed();
}

QVariantMap KelpClient::openPageView()
{
    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("url"), QString());
    result.insert(QStringLiteral("entry"), QString());
    result.insert(QStringLiteral("candidates"), QStringList());
    result.insert(QStringLiteral("pages"), 0);

    if (!m_viewing || m_phase != Done || !m_viewStore) {
        result.insert(QStringLiteral("error"), QStringLiteral("there is no page to open"));
        return result;
    }

    // Pressing the button again is the same session, not a second one: a new
    // token would orphan the first URL the browser already has — *unless* that
    // session has gone, which the idle sweep does to one nobody has asked for in
    // half an hour. Then the URL in hand is dead (a 404 with nothing to look at),
    // and the store still in memory can mint a fresh one for the same page, so
    // what the browser is handed always answers (§3.25).
    if (!m_viewToken.isEmpty()) {
        if (m_preview && m_preview->has(m_viewToken)) {
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("url"), m_viewUrl);
            return result;
        }
        qInfo() << "KelpClient: the view session has gone; serving a fresh one";
        m_viewToken.clear();
        m_viewUrl.clear();
    }

    if (!m_preview) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("this build cannot serve a preview"));
        return result;
    }

    QString error;
    if (!m_preview->start(&error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }

    Kelp::Web::Page page;
    if (!Kelp::Web::page(m_viewStore.data(), m_key, m_bundleId, &page, &error)) {
        result.insert(QStringLiteral("error"), error);
        return result;
    }
    result.insert(QStringLiteral("candidates"), page.candidates);

    if (page.path.isEmpty()) {
        // The link said it was a page and the bundle holds none — a hand-made or
        // dishonest link. There is nothing to look at, and this kind offers no
        // save as a fallback (design §5.5).
        result.insert(QStringLiteral("error"),
                      QStringLiteral("This link says it is a page, but there is no web "
                                     "page in it."));
        qWarning() << "KelpClient: a webpage link holds no page:"
                   << m_bundleIdText.left(16);
        // There will be no session to take the source, so let it go here rather
        // than leave a live connection attached to a view that has ended.
        dropSource();
        m_viewBytes = 0;
        m_viewStore.clear();
        return result;
    }

    // The session gets a source of its own, so an object missing from the view
    // store is fetched when the browser asks for it instead of a 503 (design
    // §5.1). The fetch's own source is reused when it is still here; after a
    // swept session it is gone, and the decoded link can mint another one — a
    // passphrase link is not asked for its passphrase again.
    KelpSource *source = m_source;
    bool minted = false;
    if (!source && !m_viewLink.bundleId.isEmpty()) {
        QString sourceError;
        source = KelpSource::create(m_viewLink, this, &sourceError);
        if (source) {
            minted = true;
        } else {
            qInfo() << "KelpClient: no source for the page view's missing objects:"
                    << sourceError;
        }
    }

    // The ceiling is absolute for the view store, not just for this client's
    // manifest walk: the session is now the thing that stores content, so it is
    // handed the same bound the `/info` check and the old up-front store used.
    const KelpPreviewServer::Opened opened =
        m_preview->open(m_viewStore, m_bundleIdText, m_viewSecret, m_name, source,
                        m_viewCeiling);
    result.insert(QStringLiteral("ok"), opened.ok);
    result.insert(QStringLiteral("error"), opened.error);
    result.insert(QStringLiteral("url"), opened.url);
    result.insert(QStringLiteral("entry"), opened.entry);
    if (!opened.ok) {
        if (minted && source) {
            source->stop();
            delete source;
        }
        qWarning() << "KelpClient: could not serve the page:" << opened.error;
        return result;
    }

    // The session owns it now. The client's own connections were for the
    // up-front walk and must not see the preview's object answers.
    if (source) {
        source->disconnect(this);
        source->setParent(m_preview);
        if (source == m_source)
            m_source = nullptr;
    }

    m_viewToken = opened.token;
    m_viewUrl = opened.url;
    result.insert(QStringLiteral("pages"), page.candidates.size());

    // The URL is a read capability for the *decrypted* page while the session
    // lives, exactly as a preview URL is, so it goes to the log only under the
    // gate a link is under — and never in a release build, whatever that says.
    if (KelpBuild::linksInLog(m_settings))
        qInfo() << "KelpClient: page view served at" << opened.url
                << "from" << m_bundleIdText.left(16) << "(" << page.path << ")";
    else
        qInfo() << "KelpClient: page view serving" << page.path
                << "(the URL is not logged)";

    return result;
}
