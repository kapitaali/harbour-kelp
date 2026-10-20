/*
 * Scheme activation: a kelp:// link tapped in another app, handed to the running
 * Kelp instead of starting a second one.
 *
 * The system knows Kelp handles the scheme because of the desktop file's
 * `MimeType=x-scheme-handler/kelp`, so it starts the handler with the URL as an
 * argument (see META-INF/scheme-plan.md). That process has two jobs and no
 * window: validate the argument, offer it to the instance that is already
 * serving over a private unix socket, and exit. Only when nothing answers does
 * it become the instance itself, bind the socket, and build the UI.
 *
 * Two rules shape the code:
 *
 *   * A link is a secret (its fragment is the key), so it is never logged, never
 *     written to disk, and never acted on. It is *prefilled* on the Open-a-link
 *     page; the user's button is still the only thing that starts a fetch.
 *   * One instance, or none. Two would each load and rewrite shares.json — the
 *     file holding every share's secret — so a takeover is allowed only when the
 *     connect error proves nobody is listening. A socket that accepts but does
 *     not answer is left alone rather than stolen.
 */

#ifndef KELPHANDOFF_H
#define KELPHANDOFF_H

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

class QLocalSocket;

class KelpHandoff : public QObject
{
    Q_OBJECT

public:
    // The outcome of offering a link to the instance:
    //   NoServer      nobody is listening (no file, or a stale one) — this
    //                 process may become the instance;
    //   Sent          a live instance answered "ok" and will show the page;
    //   Refused       a live instance answered "no" (the text was not a link);
    //   Unresponsive  something is listening but did not answer in time — do not
    //                 take its place.
    enum Result { NoServer, Sent, Refused, Unresponsive };

    explicit KelpHandoff(QObject *parent = nullptr);

    // Where the instance listens. Beside shares.json in the app's own data
    // directory: a place the app already owns, which the sandbox keeps other
    // apps out of.
    static QString socketPath();

    // The link a scheme activation was given, or an empty string. Only the first
    // acceptable kelp:// argument is used; anything else on the command line is
    // ignored. The program name at index 0 is never a link.
    static QString linkFromArguments(const QStringList &arguments);

    // The one validation both ends apply: kelp://, bounded, and one line — no
    // control characters, so the line protocol cannot be smuggled into.
    static bool acceptable(const QString &link);

    // Offers the link to the running instance and waits briefly for its answer.
    Result forward(const QString &link, QString *error = nullptr);

    // True when a connect reaches something. True on a timeout too: a hung
    // instance is still an instance, and starting a second one could corrupt
    // shares.json.
    bool instanceRunning() const;

    // Becomes the instance. `replaceStale` is what allows removing a socket file
    // nobody answers on — the caller may set it only after it has established that
    // nobody is listening (forward() said NoServer, or instanceRunning() said no),
    // so a live socket is never stolen.
    bool listen(bool replaceStale, QString *error = nullptr);

signals:
    // A validated link, exactly as it arrived. Connected to the root object's
    // openLink(); emitted for links that arrive after the UI exists.
    void linkArrived(const QString &link);

private:
    void onConnection();
    void readFrom(QLocalSocket *socket);
    void finish(QLocalSocket *socket, const QByteArray &answer);

    QLocalServer m_server;
};

#endif // KELPHANDOFF_H
