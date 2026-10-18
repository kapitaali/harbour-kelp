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

// How large a *received webpage* may be before it is refused (design §5.5): the
// whole bundle is held in memory while it is looked at, so there is a ceiling,
// and this is it. Enough for a document-shaped page, not for a gallery. The
// sender's claimed size is checked against it before anything is fetched, and
// what actually arrives is counted against it too — the claim is only a claim.
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
        // Nothing can be serving an incomplete view: a session only ever opens
        // once the whole bundle is here.
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
    if (m_viewing)
        m_viewSecret = Kelp::Link::base64UrlEncode(link.secret);
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
    if (m_phase != Checking)
        return;

    // A page is held in memory while it is looked at, so it has a ceiling — and
    // it is checked against the sender's claim *before* anything is asked for, so
    // an oversized page costs one /info and not 30 MB of transfers.
    if (m_viewing && bytes > m_viewCeiling) {
        qInfo() << "KelpClient: refusing a" << sizeText(bytes) << "page (ceiling"
                << sizeText(m_viewCeiling) << ") before fetching anything";
        failed(overCeiling(m_viewCeiling, bytes));
        return;
    }

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
    if (!Kelp::missingObjects(activeStore(), m_key, m_bundleId, &missing, &error)) {
        failed(error);
        return;
    }

    if (missing.isEmpty()) {
        qInfo() << "KelpClient: all" << m_fetched << "objects are here after"
                << m_rounds << "rounds";
        if (m_viewing)
            finishViewing();
        else
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

bool KelpClient::isHandling(const QString &link) const
{
    const QString trimmed = link.trimmed();
    return m_phase != Idle && !trimmed.isEmpty() && trimmed == m_link;
}

void KelpClient::finishViewing()
{
    // No files, no folder, no destination: the bundle stays in the view store, and
    // the page is served out of it from here on (design §5.5).
    m_deadline->stop();
    m_phase = Done;
    qInfo() << "KelpClient: view store holds" << m_fetched << "objects,"
            << sizeText(m_viewBytes) << "in memory; nothing was written";
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
        m_viewBytes = 0;
        m_viewStore.clear();
        return result;
    }

    const KelpPreviewServer::Opened opened =
        m_preview->open(m_viewStore, m_bundleIdText, m_viewSecret, m_name);
    result.insert(QStringLiteral("ok"), opened.ok);
    result.insert(QStringLiteral("error"), opened.error);
    result.insert(QStringLiteral("url"), opened.url);
    result.insert(QStringLiteral("entry"), opened.entry);
    if (!opened.ok) {
        qWarning() << "KelpClient: could not serve the page:" << opened.error;
        return result;
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
