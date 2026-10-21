#ifndef KELPPREVIEWSERVER_H
#define KELPPREVIEWSERVER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringList>

#include "kelp_manifest.h"
#include "kelp_store.h"

class KelpSource;
class QTcpServer;
class QTcpSocket;
class QTimer;

/*
 * The loopback side of a preview (design §5.1).
 *
 * A browser can only decrypt inside a *secure context* — https://, or localhost —
 * and the LAN path is deliberately plain http:// on a private address, so a
 * recipient's browser cannot fetch an encrypted bundle from the sender and open
 * it. The recipient's app is what decrypts: it re-serves the bundle as ordinary
 * web content on 127.0.0.1 and hands that URL to the browser.
 *
 * The loopback origin is therefore the security boundary, and this class is most
 * of it:
 *
 *   * it binds 127.0.0.1 only, on an ephemeral port, never every interface;
 *   * every URL carries a 32-byte random token, so another process on the device
 *     cannot read the plaintext by guessing an address;
 *   * every response carries a Content-Security-Policy that keeps the page
 *     offline and same-origin, no-store so nothing decrypted lands in a browser
 *     cache, and nosniff so a type cannot be second-guessed;
 *   * a path that is not in the bundle is a 404 — including ".." and anything
 *     else that does not name an entry — so the URL space is exactly the bundle;
 *   * nothing is ever written to disk: a response is decrypted per request, and
 *     a session is dropped when it has been idle for a while.
 *
 * Serving is always *plaintext to loopback*: the browser is the only audience, and
 * the card is the token in the URL, not the key.
 */
class KelpPreviewServer : public QObject
{
    Q_OBJECT

public:
    // The store a session is served from. Shared, not borrowed: a session outlives
    // the page that opened it (it ends on its own idle timeout, design §5.4/§5.5),
    // and the store must not be freed under it. A caller that merely passes the
    // app's own store says so with a no-op deleter — see the app's previewPage().
    using StorePtr = QSharedPointer<Kelp::ObjectStore>;

    // No store of its own: every session names the one it is served from.
    explicit KelpPreviewServer(QObject *parent = nullptr);
    ~KelpPreviewServer() override;

    // Binds 127.0.0.1. Failing is not fatal: without it there is simply no
    // preview, and a share keeps working.
    bool start(QString *error);
    bool isListening() const;
    int port() const;

    struct Opened {
        bool ok = false;
        QString error;
        QString token;
        QString url;              // http://127.0.0.1:<port>/<token>/
        QString entry;            // the page, bundle-relative
        QString root;             // the directory that is the URL root
        QStringList candidates;   // every HTML file, when there was a choice
    };

    // Opens a session for an already-sealed bundle, finding its page (design
    // §5.3). The bundle id is base32 and the secret base64url — the two strings
    // the link and shares.json already carry.
    //
    // `source` is optional. A session with one fetches an object the store does
    // not hold when a browser asks for it (design §5.1's open item), verifies it
    // against the id, stores it and answers the request. A session without one
    // answers 503 exactly as before. Ownership of a non-null source passes to
    // the server when the session opens; on failure the caller still owns it.
    //
    // `storeLimit` is an absolute byte ceiling for the shared store, or 0 for no
    // limit. A received page passes its 32 MiB ceiling here because the session,
    // not the client, is what stores content fetched on demand; an owner preview
    // serves an on-disk store and leaves it 0. A store that cannot report its
    // size (`ObjectStore::byteCount()` is -1) is not limited by this session.
    Opened open(const StorePtr &store, const QString &bundleId, const QString &secret,
                const QString &name, KelpSource *source = nullptr,
                quint64 storeLimit = 0);

    // The same, for a page the recipient picked out of `candidates`.
    Opened openEntry(const StorePtr &store, const QString &bundleId, const QString &secret,
                     const QString &name, const QString &entry,
                     KelpSource *source = nullptr, quint64 storeLimit = 0);

    // Ends a session. Its URL stops answering at once.
    void close(const QString &token);
    int sessionCount() const;

    // Whether a token still names a session. A caller that kept a URL can be
    // holding one whose session the idle sweep has dropped, and must not hand it
    // out again as if it worked (§3.25).
    bool has(const QString &token) const;

    // What this server has done, for the log and for tests.
    struct Tally {
        qint64 requests = 0;      // files served
        qint64 bytes = 0;         // plaintext bytes written
        qint64 notHeld = 0;       // 503s: the store could not answer yet
        qint64 refused = 0;       // requests for a token this server does not know
        qint64 onDemand = 0;      // objects fetched through a session's source
        qint64 mismatched = 0;    // 502s: bytes that did not hash to the id
        qint64 overLimit = 0;     // 503s: content that would pass the store ceiling
    };
    Tally tally() const { return m_tally; }

    // A test affordance: how long a session may sit unused before it is dropped.
    // Milliseconds.
    void setIdleTimeout(int ms);

    // A test affordance, like the idle timeout: how long a single on-demand
    // object may stay unanswered, and how long a parked browser request may wait
    // in all. Milliseconds; the production value is twenty seconds.
    void setFetchTimeout(int ms);

signals:
    // A session opened or went away, so the app can hold (or release) the request
    // that keeps it alive while a page is being served.
    void sessionsChanged();

private slots:
    void onConnection();
    void readRequest(QTcpSocket *socket);
    void sweep();

private:
    struct Session {
        StorePtr store;           // shared: outlives whoever handed it over
        QByteArray bundleId;
        QByteArray key;           // the content key, derived from the secret
        QString name;             // for the log only
        QString root;             // bundle-relative directory; "" is the root
        QString entry;            // the page, relative to `root`
        quint64 storeLimit = 0;   // absolute view-store ceiling; 0 is unlimited
        qint64 lastMs = 0;
    };

    // A connection that has not delivered a complete request yet.
    struct Pending {
        QByteArray buffer;
        qint64 accepted = 0;
    };

    // What a request line turned into.
    struct Target {
        int status = 404;
        QString location;         // a 301's target, when a directory needs its slash
        QString path;             // bundle-relative path of the file
        Kelp::Manifest::File file;
        QByteArray hash;
    };

    // A browser request waiting for objects the store does not hold yet. The
    // answer is not started until every object it needs is here, so a page is
    // never half-served and a failure is still an honest status line.
    struct ParkedRequest {
        QTcpSocket *socket = nullptr;
        QString token;
        QString path;
        Kelp::Manifest::File file;
        QByteArray rangeHeader;
        bool headOnly = false;
        qint64 parkedMs = 0;
        QSet<QByteArray> outstanding;
    };

    // One session's on-demand fetching. There is at most one source per session,
    // because a source belongs to the link whose page is being served; waiters
    // are keyed by object id so two requests for the same hole join one fetch.
    struct FetchState {
        KelpSource *source = nullptr;
        QSet<QByteArray> inFlight;
        QSet<QByteArray> queued;
        QQueue<QByteArray> queue;
        QHash<QByteArray, qint64> startedMs;
        QHash<QByteArray, QSet<QTcpSocket *>> waiters;
    };

    Opened openPath(const StorePtr &store, const QByteArray &id, const QByteArray &key,
                    const QString &name, const QString &entry, KelpSource *source,
                    quint64 storeLimit);

    void handle(QTcpSocket *socket, const QByteArray &request);
    Target resolve(Session *session, const QString &urlPath, bool trailingSlash);
    void serve(QTcpSocket *socket, Session *session, const QString &token,
               const Target &target, const QByteArray &rangeHeader, bool headOnly);
    void parkRequest(QTcpSocket *socket, const QString &token, const Target &target,
                     const QByteArray &rangeHeader, bool headOnly,
                     const QSet<QByteArray> &missing);
    void pumpFetches(const QString &token);
    void onObject(const QString &token, const QByteArray &id, const QByteArray &bytes);
    void onFetchFailed(const QString &token, const QString &message);
    void failWaiters(const QString &token, const QByteArray &id, int status,
                     const char *reason, const QByteArray &body);
    void failAllFetches(const QString &token, const QByteArray &body);
    void removeParked(QTcpSocket *socket);
    void dropFetch(const QString &token, bool answerParked);
    void timeoutFetches(qint64 now);
    void writeHead(QTcpSocket *socket, int status, const char *reason,
                   const QByteArray &extraHeaders, bool headOnly);
    void respond(QTcpSocket *socket, int status, const char *reason,
                 const char *contentType, const QByteArray &body, bool headOnly,
                 const QByteArray &extraHeaders = QByteArray());
    static QByteArray securityHeaders();

    QTcpServer *m_server;
    QTimer *m_sweeper;
    QTimer *m_fetchSweeper;
    QHash<QTcpSocket *, Pending> m_pending;
    QHash<QString, Session> m_sessions;
    QHash<QTcpSocket *, ParkedRequest> m_parked;
    QHash<QString, FetchState> m_fetches;
    Tally m_tally;
    int m_idleTimeoutMs;
    int m_fetchTimeoutMs;
};

#endif // KELPPREVIEWSERVER_H
