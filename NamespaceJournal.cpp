#include "NamespaceJournal.h"
#include "Replica.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace iiSocietySync::detail {
namespace {
QByteArray bytes(const QJsonObject &v) { return QJsonDocument(v).toJson(QJsonDocument::Compact); }
QString hash(const QJsonObject &v) { return QString::fromLatin1(QCryptographicHash::hash(bytes(v), QCryptographicHash::Sha256).toHex()); }
QJsonObject content(const QJsonObject &e) {
    QJsonObject result;
    for (const auto *key : {"path", "kind", "size", "hash", "clock", "version"}) result.insert(key, e.value(key));
    return result;
}
QJsonObject projected(const QJsonObject &r) {
    auto e = r.value("object").toObject();
    for (const auto *key : {"namespace", "authority", "revision", "sequence"}) e.insert(key, r.value(key));
    return e;
}
qint64 counter(const QJsonValue &v) {
    bool ok; const auto n = v.toString().toLongLong(&ok);
    return ok && n >= 0 && n <= 9007199254740991LL ? n : -1;
}
QJsonObject failure(const QString &error) { return {{"ok", false}, {"error", error}}; }
}
bool NamespaceJournal::initialize() {
    for (const auto *sql : {
        "CREATE TABLE IF NOT EXISTS namespace_revisions(sequence INTEGER PRIMARY KEY,revision TEXT NOT NULL UNIQUE,path TEXT NOT NULL,data BLOB NOT NULL)",
        "CREATE INDEX IF NOT EXISTS namespace_path ON namespace_revisions(path,sequence)",
        "CREATE TABLE IF NOT EXISTS object_locations(version TEXT NOT NULL,provider TEXT NOT NULL,data BLOB NOT NULL,PRIMARY KEY(version,provider))"}) {
        QSqlQuery q(db); if (!q.exec(sql)) { error = q.lastError().text(); return false; }
    }
    return true;
}
bool NamespaceJournal::clear() {
    for (const auto *sql : {"DELETE FROM namespace_revisions", "DELETE FROM object_locations"}) {
        QSqlQuery q(db); if (!q.exec(sql)) { error = q.lastError().text(); return false; }
    }
    return true;
}
QJsonObject NamespaceJournal::state() const {
    QSqlQuery q(db); q.exec("SELECT sequence,revision FROM namespace_revisions ORDER BY sequence DESC LIMIT 1");
    const bool found = q.next();
    return {{"namespace", space}, {"authority", authority}, {"sequence", found ? q.value(0).toString() : "0"},
        {"head", found ? q.value(1).toString() : QString()}, {"namespaceVersion", 1}};
}
QJsonObject NamespaceJournal::revision(const QString &id) const {
    QSqlQuery q(db); q.prepare("SELECT data FROM namespace_revisions WHERE revision=?"); q.addBindValue(id);
    return q.exec() && q.next() ? QJsonDocument::fromJson(q.value(0).toByteArray()).object() : QJsonObject();
}
QJsonObject NamespaceJournal::object(const QString &path) const {
    QSqlQuery q(db); q.prepare("SELECT data FROM namespace_revisions WHERE path=? ORDER BY sequence DESC LIMIT 1"); q.addBindValue(path);
    if (!q.exec() || !q.next()) return {};
    auto result = projected(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
    q.finish(); q.prepare("SELECT data FROM object_locations WHERE version=? ORDER BY provider"); q.addBindValue(result.value("version").toString());
    QJsonArray locations;
    if (q.exec()) while (q.next()) locations.append(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
    result.insert("locations", locations); return result;
}
bool NamespaceJournal::append(const QJsonObject &r) {
    QSqlQuery q(db); q.prepare("INSERT INTO namespace_revisions(sequence,revision,path,data) VALUES(?,?,?,?)");
    q.addBindValue(counter(r.value("sequence"))); q.addBindValue(r.value("revision").toString());
    q.addBindValue(r.value("object").toObject().value("path").toString()); q.addBindValue(bytes(r));
    if (!q.exec()) { error = q.lastError().text(); return false; } return true;
}
QJsonObject NamespaceJournal::commit(const QJsonObject &record, const QString &proposal, const QString &base) {
    const auto old = object(record.value("path").toString());
    if (old.value("version") == record.value("version")) { auto e = old; e.remove("locations"); return e; }
    const auto head = state(); const auto sequence = counter(head.value("sequence")) + 1;
    if (sequence > 9007199254740991LL || !Replica::validRecord(record)) { error = "invalid_namespace_commit"; return {}; }
    QJsonArray parents;
    if (!old.isEmpty()) parents.append(old.value("revision"));
    if (!base.isEmpty() && old.value("revision") != base && !revision(base).isEmpty()) parents.append(base);
    QJsonObject r{{"namespace", space}, {"authority", authority}, {"sequence", QString::number(sequence)},
        {"previous", head.value("head")}, {"parents", parents}, {"object", content(record)}, {"proposal", proposal}};
    r.insert("revision", hash(r));
    return append(r) ? projected(r) : QJsonObject();
}
QJsonObject NamespaceJournal::changes(qint64 after, qint64 through, bool history) const {
    const auto current = state().value("sequence").toString().toLongLong(); if (!through) through = current;
    if (after < 0 || after > through || through > current) return failure("invalid_namespace_cursor");
    QSqlQuery q(db);
    // Keep a real bounded snapshot even if an object changes while pages are read.
    q.prepare(history ? "SELECT data,sequence FROM namespace_revisions WHERE sequence>? AND sequence<=? ORDER BY sequence LIMIT 128"
        : "SELECT r.data,r.sequence FROM namespace_revisions r WHERE r.sequence>? AND r.sequence<=? AND NOT EXISTS(SELECT 1 FROM namespace_revisions newer WHERE newer.path=r.path AND newer.sequence>r.sequence AND newer.sequence<=?) ORDER BY r.sequence LIMIT 128");
    q.addBindValue(after); q.addBindValue(through); if (!history) q.addBindValue(through);
    if (!q.exec()) return failure("namespace_read_failed");
    QJsonArray entries; qint64 next = after, total = 0; bool limited = false;
    while (q.next()) {
        auto value = QJsonDocument::fromJson(q.value(0).toByteArray()).object(); if (!history) value = projected(value);
        const auto length = bytes(value).size();
        if (total + length > 256 * 1024) { limited = true; break; }
        entries.append(value); next = q.value(1).toLongLong(); total += length;
    }
    const bool more = limited || (entries.size() == 128 && next < through);
    return {{"ok", true}, {"namespace", space}, {"authority", authority}, {"entries", entries},
        {"through", QString::number(through)}, {"next", QString::number(more ? next : through)}, {"more", more}};
}
bool NamespaceJournal::accept(const QJsonObject &page) {
    auto head = state(); auto sequence = counter(head.value("sequence")); auto previous = head.value("head").toString();
    const auto next = counter(page.value("next")), through = counter(page.value("through"));
    if (page.value("namespace") != space || page.value("authority") != authority || !page.value("entries").isArray()
        || !page.value("ok").toBool() || !page.value("more").isBool() || next < sequence || next > through
        || (page.value("more").toBool() ? next <= sequence || next >= through : next != through)
        || page.value("entries").toArray().size() > 128 || bytes(page).size() > 400000) { error = "invalid_namespace_page"; return false; }
    for (const auto &value : page.value("entries").toArray()) {
        auto r = value.toObject(); const auto id = r.take("revision").toString();
        const auto parents = r.value("parents").toArray();
        if (id != hash(r) || r.value("namespace") != space || r.value("authority") != authority
            || counter(r.value("sequence")) != sequence + 1 || r.value("previous") != previous
            || !r.value("parents").isArray() || parents.size() > 2 || !Replica::validRecord(r.value("object").toObject())) {
            error = "invalid_namespace_revision"; return false;
        }
        const auto old = object(r.value("object").toObject().value("path").toString());
        if (!old.isEmpty() && !parents.contains(old.value("revision"))) { error = "namespace_parent_missing"; return false; }
        for (const auto &parent : parents) if (!parent.isString() || revision(parent.toString()).isEmpty()) { error = "namespace_parent_missing"; return false; }
        r.insert("revision", id); if (!append(r)) return false;
        ++sequence; previous = id;
    }
    if (next != sequence) { error = "namespace_sequence_gap"; return false; }
    return true;
}
bool NamespaceJournal::contains(const QJsonObject &record) const {
    const auto r = revision(record.value("revision").toString());
    return !r.isEmpty() && record.value("namespace") == space && record.value("authority") == authority
        && projected(r) == record;
}
bool NamespaceJournal::locate(const QString &version, const QJsonObject &location) {
    QSqlQuery q(db); q.prepare("INSERT INTO object_locations(version,provider,data) VALUES(?,?,?) ON CONFLICT(version,provider) DO UPDATE SET data=excluded.data");
    q.addBindValue(version); q.addBindValue(location.value("provider").toString()); q.addBindValue(bytes(location));
    if (!q.exec()) { error = q.lastError().text(); return false; } return true;
}
}
