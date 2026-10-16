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
#include "kelpstore.h"

namespace {

// How many object requests may be in flight at once. On a LAN the round trip is
// tiny, so this is about keeping the pipe full rather than politeness. A relay
// answer is sequential inside one tunnel, so this mostly matters for the LAN.
const int kMaxInflight = 4;

// If no reply finishes for this long the sender has stopped answering — a peer
// that accepts the connection and then says nothing — and the fetch ends instead
// of leaving a spinner up forever.
const int kSilenceMs = 20000;

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

// One entry of a bundle, written at `path`. Names come from the sender, so each
// one is checked again here: a manifest may only name one entry per folder, and
// it may not name a path.
bool writeNode(const Kelp::TreeNode &node, const QString &path,
               int *files, quint64 *bytes, QString *error)
{
    if (!Kelp::Manifest::isValidName(node.name) || node.name.size() > 255) {
        *error = QStringLiteral("the sender used a name that cannot be saved");
        return false;
    }

    if (node.isDir) {
        if (!QDir().mkpath(path)) {
            *error = QStringLiteral("could not make the folder %1").arg(node.name);
            return false;
        }
        for (int i = 0; i < node.children.size(); ++i) {
            const Kelp::TreeNode &child = node.children.at(i);
            if (!writeNode(child, path + QLatin1Char('/') + child.name, files, bytes, error))
                return false;
        }
        return true;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("could not write %1").arg(node.name);
        return false;
    }
    if (file.write(node.data) != node.data.size()) {
        *error = QStringLiteral("could not write all of %1").arg(node.name);
        return false;
    }
    file.close();
    ++(*files);
    *bytes += quint64(node.data.size());
    return true;
}

bool writeChildren(const QVector<Kelp::TreeNode> &children, const QString &dir,
                   int *files, quint64 *bytes, QString *error)
{
    for (int i = 0; i < children.size(); ++i) {
        const Kelp::TreeNode &child = children.at(i);
        if (!writeNode(child, dir + QLatin1Char('/') + child.name, files, bytes, error))
            return false;
    }
    return true;
}

} // namespace

KelpClient::KelpClient(KelpObjectStore *store, QObject *parent)
    : QObject(parent),
      m_store(store),
      m_source(nullptr),
      m_deadline(new QTimer(this)),
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
    emit changed();
}

void KelpClient::cancel()
{
    const bool wasBusy = busy();
    dropSource();
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
    qWarning() << "KelpClient: fetch failed:" << message
               << "(objects stored:" << m_fetched << ")";
    emit changed();
    return false;
}

bool KelpClient::fetch(const QString &linkText)
{
    if (busy()) {
        m_error = QStringLiteral("another fetch is still running");
        emit changed();
        return false;
    }
    reset();

    if (!m_store)
        return failed(QStringLiteral("no object store to fetch into"));

    Kelp::Link::Bundle link;
    QString error;
    if (!Kelp::Link::decode(linkText.trimmed(), &link, &error))
        return failed(error);

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
    m_phase = Checking;
    emit changed();

    qInfo() << "KelpClient: fetching" << m_bundleIdText.left(16) << "from"
            << m_sourceLabel << "as"
            << (m_name.isEmpty() ? QStringLiteral("(unnamed)") : m_name);

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

    // What the sender claims the bundle holds. It only shapes the progress
    // display; what is downloaded and what is accepted is decided by the walk.
    m_total = qMax(0, objects);

    m_phase = Fetching;
    emit changed();
    walk();
}

void KelpClient::onFailed(const QString &message)
{
    if (m_phase == Idle || m_phase == Done || m_phase == Failed)
        return;
    failed(message);
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

    QVector<QByteArray> missing;
    QString error;
    if (!Kelp::missingObjects(m_store, m_key, m_bundleId, &missing, &error)) {
        failed(error);
        return;
    }

    if (missing.isEmpty()) {
        qInfo() << "KelpClient: all" << m_fetched << "objects are here after"
                << m_rounds << "rounds";
        writeFiles();
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

    if (!Kelp::storeReceived(m_store, id, bytes)) {
        failed(QStringLiteral("something arrived that was not the object asked for"));
        return;
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

    Kelp::TreeNode tree;
    QString error;
    if (!Kelp::open(m_store, m_key, m_bundleId, &tree, &error)) {
        failed(error);
        return;
    }
    if (!tree.isDir) {
        failed(QStringLiteral("the bundle root is not a folder"));
        return;
    }

    // Where the files are written. The user picks this on the link page; an
    // empty choice means the platform's Downloads folder.
    QString base = m_directory;
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/Downloads");
    }

    // Where it lands. A bundle root is a container, and today the Add page puts
    // the one item that was picked in it — so the item is what the user asked
    // for, not a folder to be nested inside a copy of itself:
    //
    //   * a single file becomes that file in the chosen folder;
    //   * a single folder becomes a folder named for the link, holding what the
    //     picked folder held;
    //   * anything else becomes a folder named for the link.
    const QString label = m_name.isEmpty() ? m_bundleIdText.left(8) : m_name;
    const QVector<Kelp::TreeNode> roots = tree.children;

    int files = 0;
    quint64 bytes = 0;
    QString destination;

    if (roots.size() == 1 && !roots.at(0).isDir) {
        destination = uniquePath(base, roots.at(0).name, false);
        if (QFileInfo::exists(destination)) {
            failed(QStringLiteral("there are already too many copies of this in %1")
                       .arg(QDir(base).dirName()));
            return;
        }
        m_destination = destination;
        if (!QDir().mkpath(QFileInfo(destination).path())) {
            failed(QStringLiteral("could not make a folder to save into"));
            return;
        }
        if (!writeNode(roots.at(0), destination, &files, &bytes, &error)) {
            m_files = files;
            m_bytes = bytes;
            failed(error);
            return;
        }
    } else {
        const QVector<Kelp::TreeNode> inside =
            roots.size() == 1 ? roots.at(0).children : roots;
        destination = uniquePath(base, label, true);
        if (QFileInfo::exists(destination)) {
            failed(QStringLiteral("there are already too many copies of this in %1")
                       .arg(QDir(base).dirName()));
            return;
        }
        m_destination = destination;
        if (!QDir().mkpath(destination)) {
            failed(QStringLiteral("could not make a folder to save into"));
            return;
        }
        if (!writeChildren(inside, destination, &files, &bytes, &error)) {
            m_files = files;
            m_bytes = bytes;
            failed(error);
            return;
        }
    }

    m_files = files;
    m_bytes = bytes;
    m_phase = Done;
    qInfo() << "KelpClient: wrote" << files << "files," << bytes << "bytes to"
            << destination;
    emit changed();
}
