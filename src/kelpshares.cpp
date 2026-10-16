#include "kelpshares.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "kelplog.h"

KelpShares::KelpShares(QObject *parent) : QObject(parent)
{
}

QString KelpShares::filePath()
{
    return KelpLog::dataDirectory() + QStringLiteral("/shares.json");
}

QVariantList KelpShares::load() const
{
    QFile file(filePath());
    if (!file.exists())
        return QVariantList();

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "KelpShares: cannot read" << filePath() << ":" << file.errorString();
        return QVariantList();
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        qWarning() << "KelpShares: ignoring unreadable" << filePath() << ":"
                   << error.errorString();
        return QVariantList();
    }

    QVariantList shares;
    const QJsonArray array = document.array();
    for (int i = 0; i < array.size(); ++i) {
        if (array.at(i).isObject())
            shares.append(array.at(i).toObject().toVariantMap());
    }
    return shares;
}

bool KelpShares::save(const QVariantList &shares) const
{
    QJsonArray array;
    for (int i = 0; i < shares.size(); ++i)
        array.append(QJsonObject::fromVariantMap(shares.at(i).toMap()));

    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Indented);

    QDir().mkpath(KelpLog::dataDirectory());

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "KelpShares: cannot write" << filePath() << ":" << file.errorString();
        return false;
    }
    // The rows hold each share's secret, so tighten the file before any content
    // is written. A write that fails here still leaves owner-only permissions.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const bool ok = file.write(data) == data.size();
    file.close();

    if (!ok)
        qWarning() << "KelpShares: short write to" << filePath();
    return ok;
}
