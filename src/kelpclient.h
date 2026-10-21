#ifndef KELPCLIENT_H
#define KELPCLIENT_H

#include <QByteArray>
#include <QQueue>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QVariantMap>

#include <QObject>

#include "kelp_link.h"
#include "kelp_store.h"

class KelpPreviewServer;
class KelpSettings;
class KelpSource;
class QTimer;

/*
 * The receiving end of a kelp:// link.
 *
 * Given a link this fetches what the bundle needs — the root manifest first,
 * then the manifests it names, then the content they point at — from whichever
 * source the link offers: the publisher on the LAN (`l=`, wire spec §7.2) or a
 * relay (`r=`, §12.1). Which one that is is `KelpSource`'s business; the walk
 * here is the same either way, and nothing is stored until it hashes to the id
 * it was asked for, so a source can withhold but never substitute.
 *
 * The two decisions — what is still missing, and what counts as an object worth
 * keeping — live in libkelp. This class is only the bookkeeping the UI binds to:
 * what to ask for, a handful in flight, and a deadline when the sender goes
 * quiet. When nothing is missing the bundle is opened and written into the
 * user's chosen download folder (Downloads by default): a bundle holding a
 * single file is that file, anything else becomes a folder named for the link.
 *
 * There is a second, entirely different ending, for a link that declares itself a
 * *webpage* (design §5.5): fetchForViewing() fetches only the manifests — enough
 * to resolve the page and its paths — into a memory store of its own and writes
 * nothing. openPageView() then serves the page to this device's browser through
 * the preview listener, whose session fetches the content the browser asks for
 * (design §5.1). The memory is shared with the session that serves it, so a
 * session may outlive the page safely; it is gone when the session's own idle
 * timeout drops it, or when the app exits. See META-INF/lazy-open-plan.md.
 */
class KelpClient : public QObject
{
    Q_OBJECT

    // "idle", "checking", "fetching", "saving", "done" or "failed".
    Q_PROPERTY(QString phase READ phase NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // Objects the sender says the bundle holds, and how many have arrived.
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(int fetched READ fetched NOTIFY changed)
    // What was actually written out, once it is done.
    Q_PROPERTY(int files READ files NOTIFY changed)
    Q_PROPERTY(quint64 bytes READ bytes NOTIFY changed)
    // The source being fetched from, and the folder the files landed in.
    Q_PROPERTY(QString source READ source NOTIFY changed)
    Q_PROPERTY(QString destination READ destination NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    // True while this client is fetching for *viewing* rather than saving: the
    // objects go into a memory store and nothing is written (design §5.5).
    Q_PROPERTY(bool viewing READ viewing NOTIFY changed)
    // What that memory store holds *now*, in bytes, including objects the preview
    // session fetched after the page opened. Zero once a view is over.
    Q_PROPERTY(quint64 viewBytes READ viewBytes NOTIFY changed)
    // Base folder the bundle is written into. Empty means the platform's
    // Downloads folder. Kept across fetches so a choice is not lost on retry.
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY changed)

public:
    explicit KelpClient(Kelp::ObjectStore *store, QObject *parent = nullptr);

    // Starts fetching the bundle behind `link`. Returns false when it cannot even
    // be tried — a link that does not parse, or names no source — and leaves the
    // reason in `error`. Otherwise the work happens asynchronously and the
    // properties above follow it.
    //
    // `passphrase` is for a passphrase link (design §12.3): the link holds no key,
    // so the master secret has to be unwrapped from the passphrase before
    // anything can be asked of the network. An ordinary link ignores it. It is
    // used and dropped — this class remembers it no more than it remembers the
    // link's key.
    Q_INVOKABLE bool fetch(const QString &link, const QString &passphrase = QString());

    // The same fetch, for a link that declares itself a webpage (design §5.5):
    // only the manifests are fetched, into a memory store belonging to this view,
    // and *nothing* is written. The page's content is fetched by the session that
    // openPageView() starts, when the browser asks for it. A second view never
    // disturbs the first — each one owns its store.
    Q_INVOKABLE bool fetchForViewing(const QString &link,
                                     const QString &passphrase = QString());

    // Serves the page whose manifests are already in the view store to this
    // device's browser through the preview listener, as §5.1 does, and returns
    // { ok, url, entry, candidates, error }. The session fetches the content the
    // browser asks for; pressing the button again returns the same session rather
    // than fetching again — unless the session has since been swept, when a fresh
    // one is minted from the memory still held and the decoded link, so the URL
    // handed to the browser always answers. The caller hands `url` to the browser:
    // this class does not open anything, so a build with no browser (the emulator)
    // still gets a session that a `curl` can prove.
    Q_INVOKABLE QVariantMap openPageView();

    // Stops a fetch in flight. Objects already stored are kept: each one is
    // verified content, just an incomplete bundle.
    Q_INVOKABLE void cancel();

    // Clears the result of the last fetch so the page can be used again. A view's
    // memory store is released here; a session still serving it holds its own
    // reference and is unaffected.
    Q_INVOKABLE void reset();

    // Whether the fetch this client is on — or the one it last finished — belongs
    // to *this* link. This class outlives the QML page that drove it, so a page
    // opened later can find a finished fetch sitting in it that is not its own: a
    // "done" (or a failure) that would otherwise present itself as this link's.
    // Asking by link is how the page tells (§3.25). The link is the identity of
    // everything the client holds for one fetch — the state, the summary, the
    // store, the session.
    Q_INVOKABLE bool isHandling(const QString &link) const;

    QString phase() const;
    bool busy() const;
    QString name() const { return m_name; }
    int total() const { return m_total; }
    int fetched() const { return m_fetched; }
    int files() const { return m_files; }
    quint64 bytes() const { return m_bytes; }
    QString source() const { return m_sourceLabel; }
    QString destination() const { return m_destination; }
    QString error() const { return m_error; }
    QString directory() const { return m_directory; }
    bool viewing() const { return m_viewing; }
    quint64 viewBytes() const
    {
        return m_viewStore ? quint64(m_viewStore->byteCount()) : 0;
    }

    // Where the next fetch is written. Empty restores the Downloads default.
    void setDirectory(const QString &path);

    // A test affordance, in the same spirit as the preview server's idle timeout:
    // how large a *viewed* bundle may grow before it is refused. The production
    // ceiling is 32 MiB, and the storing-time check is only reachable in a test
    // with a smaller one.
    void setViewCeiling(quint64 bytes);

    // The listener a received page is served through (design §5.5). Without one a
    // view can fetch but not be opened.
    void setPreviewServer(KelpPreviewServer *preview);

    // The user's own preference objects; only the link-logging gate is read here,
    // through KelpBuild::linksInLog.
    void setSettings(KelpSettings *settings);

signals:
    void changed();

private slots:
    void onInfo(int objects, int files, quint64 bytes);
    void onObject(const QByteArray &id, const QByteArray &bytes);
    void onFailed(const QString &message);
    void timedOut();

private:
    enum Phase { Idle, Checking, Fetching, Saving, Done, Failed };

    bool beginFetch(const QString &linkText, bool viewing, const QString &passphrase);
    Kelp::ObjectStore *activeStore() const;
    void walk();
    void pump();
    void writeFiles();
    void finishViewing();
    bool failed(const QString &message);
    void dropSource();

    Kelp::ObjectStore *m_store;
    KelpSource *m_source;
    KelpPreviewServer *m_preview;
    KelpSettings *m_settings;
    QTimer *m_deadline;
    QQueue<QByteArray> m_queue;
    QSet<QByteArray> m_inflight;

    // The view (design §5.5). Shared with the session that serves it, so the store
    // outlives this client's interest in it; it is freed when the session ends.
    QSharedPointer<Kelp::MemoryStore> m_viewStore;
    bool m_viewing;
    QString m_link;            // the link this fetch belongs to; its identity
    Kelp::Link::Bundle m_viewLink; // decoded link, for a source a re-minted session needs
    QString m_viewSecret;      // the link's secret, for the preview server
    QString m_viewToken;
    QString m_viewUrl;
    quint64 m_viewBytes;       // what this client put in the view store (the index)
    quint64 m_viewCeiling;     // the whole view's ceiling (a test can lower it)

    QByteArray m_bundleId;
    QByteArray m_key;
    QString m_bundleIdText;
    QString m_name;
    QString m_sourceLabel;
    QString m_destination;
    QString m_directory;
    QString m_error;
    Phase m_phase;
    int m_total;
    int m_fetched;
    int m_storedThisRound;
    int m_rounds;
    int m_files;
    quint64 m_bytes;
};

#endif // KELPCLIENT_H
