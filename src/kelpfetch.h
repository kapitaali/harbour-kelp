#ifndef KELPFETCH_H
#define KELPFETCH_H

#include <QByteArray>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include "kelp_link.h"

class KelpObjectStore;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Parses a relay address from a link or settings — "host[:port]", default port
// 9443 — into a host and port a TLS connection can be made to. Returns false if
// it is not usable as one (it must not carry a path, scheme or credentials).
bool kelpRelayHostPort(const QString &relay, QString *host, quint16 *port);

/*
 * Where a recipient's objects come from.
 *
 * A kelp:// link names its sources (wire spec §5): `l=` is the publisher on the
 * LAN, `r=` is a relay. Both serve the same two things — the bundle's shape and
 * one object by id — and both are only ever trusted to withhold, because the
 * walk verifies every object against its id. So the two transports differ in
 * mechanism (plain HTTP to a LAN address vs HTTP inside a TLS 1.3 tunnel to a
 * relay) but not in contract, and the fetch loop is written once against this.
 *
 * One request per call: `begin()` asks for the shape, `requestObject()` asks for
 * an object, and the two signals carry the answers. A source may answer several
 * `requestObject()` calls in any order; the caller matches by id.
 */
class KelpSource : public QObject
{
    Q_OBJECT

public:
    explicit KelpSource(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~KelpSource() {}

    // Chooses a source for `link` — the LAN address if it has one, else its first
    // relay. Returns nullptr (and sets `error`) when the link names neither.
    static KelpSource *create(const Kelp::Link::Bundle &link, QObject *parent, QString *error);

    // The same for the link's first relay only, ignoring its LAN address. This is
    // the retry KelpClient makes when the address a link carries cannot answer
    // (a publisher on mobile data advertises an address no other network can
    // route to): the LAN is preferred while it works, not the only path tried.
    static KelpSource *createFromRelay(const Kelp::Link::Bundle &link, QObject *parent,
                                       QString *error);

    // What the sender says the bundle holds. Also the first thing asked for.
    virtual void begin() = 0;

    // Ask for one object by id.
    virtual void requestObject(const QByteArray &id) = 0;

    // Abandon everything in flight.
    virtual void stop() = 0;

    // A short human string for the UI — "10.0.2.15:1234" or "relay host:port".
    virtual QString describe() const = 0;

    // Whether this source reaches the publisher through a relay. KelpClient uses
    // it to decide whether a failure is worth retrying the other way; a source
    // that already goes through a relay has nowhere else to go.
    virtual bool fromRelay() const { return false; }

signals:
    void infoReady(int objects, int files, quint64 bytes);
    void objectReady(const QByteArray &id, const QByteArray &bytes);
    void failed(const QString &message);
};

/*
 * The LAN half of KelpSource (wire spec §7.2): plain HTTP GETs against the
 * publisher's address, one object per request.
 *
 * The constructor takes the RAW swarm token bytes and encodes them itself, in
 * exactly one place — see infoRequest()/objectRequest(). The Authorization
 * header carries the base64url TEXT of the token, and the sender decodes it
 * and compares bytes, so handing raw bytes to the header is a silent 401
 * ("the sender did not recognise this link"). That is what happened when the
 * encoding lived at the call site; keep it here, where it cannot drift from
 * the requests that carry it.
 */
class LanSource : public KelpSource
{
    Q_OBJECT

public:
    LanSource(const QUrl &base, const QByteArray &swarm, const QString &label,
              const QString &bundleIdText, QObject *parent = nullptr);

    void begin() override;
    void requestObject(const QByteArray &id) override;
    void stop() override;
    QString describe() const override;

    // The requests begin()/requestObject() actually send, exposed so a test
    // can pin the header format both ends must agree on.
    QNetworkRequest infoRequest() const;
    QNetworkRequest objectRequest(const QByteArray &id) const;

    // For a test: how long begin()'s request may stay unanswered before the
    // address is called unreachable. Milliseconds; the default is a few seconds.
    void setConnectTimeout(int ms);

private:
    QUrl endpoint(const QString &suffix) const;
    bool usable(QNetworkReply *reply, QString *error) const;
    void onInfo(QNetworkReply *reply);
    void onObject(QNetworkReply *reply);
    void noteReached();
    void onConnectTimeout();

    QNetworkAccessManager *m_network;
    QUrl m_base;
    QString m_label;
    QByteArray m_swarm;               // raw bytes; encoded at request time
    QString m_bundleIdText;
    QSet<QNetworkReply *> m_replies;

    // The first request doubles as a reachability probe (see begin()). A peer
    // that answers — with anything, even a 401 — is reachable; one that never
    // says anything is called unreachable rather than left to the silence
    // deadline, whose "stopped answering" blames a sender that was never heard.
    QTimer *m_reachability;
    bool m_reached = false;
    bool m_unreachable = false;
};

#endif // KELPFETCH_H
