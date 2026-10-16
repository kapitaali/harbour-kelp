#ifndef KELPCLIENT_H
#define KELPCLIENT_H

#include <QByteArray>
#include <QQueue>
#include <QSet>
#include <QString>

#include <QObject>

class KelpObjectStore;
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
    // Base folder the bundle is written into. Empty means the platform's
    // Downloads folder. Kept across fetches so a choice is not lost on retry.
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY changed)

public:
    explicit KelpClient(KelpObjectStore *store, QObject *parent = nullptr);

    // Starts fetching the bundle behind `link`. Returns false when it cannot even
    // be tried — a link that does not parse, or names no source — and leaves the
    // reason in `error`. Otherwise the work happens asynchronously and the
    // properties above follow it.
    Q_INVOKABLE bool fetch(const QString &link);

    // Stops a fetch in flight. Objects already stored are kept: each one is
    // verified content, just an incomplete bundle.
    Q_INVOKABLE void cancel();

    // Clears the result of the last fetch so the page can be used again.
    Q_INVOKABLE void reset();

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

    // Where the next fetch is written. Empty restores the Downloads default.
    void setDirectory(const QString &path);

signals:
    void changed();

private slots:
    void onInfo(int objects, int files, quint64 bytes);
    void onObject(const QByteArray &id, const QByteArray &bytes);
    void onFailed(const QString &message);
    void timedOut();

private:
    enum Phase { Idle, Checking, Fetching, Saving, Done, Failed };

    void walk();
    void pump();
    void writeFiles();
    bool failed(const QString &message);
    void dropSource();

    KelpObjectStore *m_store;
    KelpSource *m_source;
    QTimer *m_deadline;
    QQueue<QByteArray> m_queue;
    QSet<QByteArray> m_inflight;

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
