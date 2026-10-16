/*
 * Persisted share list.
 *
 * The bundle list lives in a QML ListModel, which is memory-only: before this,
 * restarting the app silently forgot every share, even though the sealed objects
 * stayed on disk. That is the gap this closes — the rows (name, size, object
 * counts, bundle id and secret) are written to a small JSON file beside the log,
 * so the next launch can serve them again instead of pretending they never
 * existed.
 *
 * The rows carry each share's secret — the key that is also in the link's
 * fragment — so the file is written owner-only (0600) and never logged. The
 * address captured in a link at share time is deliberately NOT stored: the port
 * is ephemeral, so it is regenerated fresh on every launch (see
 * KelpBundles::republish).
 */

#ifndef KELPSHARES_H
#define KELPSHARES_H

#include <QObject>
#include <QString>
#include <QVariantList>

class KelpShares : public QObject
{
    Q_OBJECT

public:
    explicit KelpShares(QObject *parent = nullptr);

    // The JSON file the shares are stored in. Static: the path is a property of
    // the app, not of any one instance.
    static QString filePath();

    // The rows shared so far, as they were stored. A missing file is an empty
    // list, not an error — the first run has nothing to restore — and a corrupt
    // one is reported and treated the same way, so a bad write cannot wedge
    // startup.
    Q_INVOKABLE QVariantList load() const;

    // Replaces the stored rows. Each row is a plain map of the fields the list
    // model keeps. Returns false and logs if the file cannot be written.
    Q_INVOKABLE bool save(const QVariantList &shares) const;
};

#endif // KELPSHARES_H
