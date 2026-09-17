#pragma once
#include <QJsonObject>
#include <QSqlDatabase>

namespace iiSocietySync::detail {
// Part of Replica's SQLite transaction and thread. Storage locations never
// participate in namespace identity or revision ordering.
class NamespaceJournal final {
public:
    QSqlDatabase db;
    QString space, authority, error;
    bool initialize();
    bool clear();
    QJsonObject state() const;
    QJsonObject object(const QString &path) const;
    QJsonObject revision(const QString &id) const;
    QJsonObject commit(const QJsonObject &record, const QString &proposal = {}, const QString &base = {});
    QJsonObject changes(qint64 after, qint64 through, bool history) const;
    bool accept(const QJsonObject &page);
    bool contains(const QJsonObject &record) const;
    bool locate(const QString &version, const QJsonObject &location);
private:
    bool append(const QJsonObject &revision);
};
}
