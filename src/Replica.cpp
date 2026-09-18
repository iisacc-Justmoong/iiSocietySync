#include "Replica.h"
#include <FileDirectory.h>
#include "ConfinedFiles.h"
#include "NamespaceJournal.h"
#include "ObjectProvider.h"
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QBuffer>
#include <QImageReader>
#include <QImage>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>
#include <algorithm>

namespace iiSocietySync {
namespace {
constexpr qint64 MaxCounter = 9007199254740991LL;
QByteArray json(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QString digest(const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
bool hex(const QString &s) { static const QRegularExpression rx("\\A[a-f0-9]{64}\\z"); return rx.match(s).hasMatch(); }
bool number(const QJsonValue &v, qint64 *n) { bool ok = false; *n = v.toString().toLongLong(&ok); return ok && *n >= 0 && *n <= MaxCounter; }
QJsonObject payload(const QJsonObject &e) {
    return {{"path", e.value("path")}, {"kind", e.value("kind")}, {"size", e.value("size")}, {"hash", e.value("hash")}, {"clock", e.value("clock")}};
}
QJsonObject versioned(QJsonObject e) { e = payload(e); e.insert("version", digest(json(e))); return e; }
QString physical(const QString &key) {
    const auto section = key.section('/', 0, 0), tail = key.section('/', 1);
    if (tail.isEmpty() || !key.contains('/') || key.size() > 3500 || !key.isValidUtf16() || key.contains('\\') || key.contains(':') || key.contains(QChar::Null)) return {};
    for (const auto &part : key.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 220 || part.startsWith(".society-") || part.startsWith(".iiserverhost-")) return {};
    for (const auto s : iiSocietyContainer::allStoreSections())
        if (section == iiSocietyContainer::storeSectionKey(s)) return iiSocietyContainer::storeSectionName(s) + '/' + tail;
    return {};
}
bool contentEqual(const QJsonObject &a, const QJsonObject &b) {
    return a.value("kind") == b.value("kind") && a.value("hash") == b.value("hash") && a.value("size") == b.value("size");
}
// 1: incoming dominates; -1: local dominates; 0: equal; 2: concurrent.
int compare(const QJsonObject &incoming, const QJsonObject &local) {
    bool greater = false, less = false; QSet<QString> keys;
    for (const auto &k : incoming.keys()) keys.insert(k);
    for (const auto &k : local.keys()) keys.insert(k);
    for (const auto &k : keys) {
        const auto a = incoming.value(k).toString().toLongLong(), b = local.value(k).toString().toLongLong();
        greater |= a > b; less |= a < b;
    }
    return greater && less ? 2 : greater ? 1 : less ? -1 : 0;
}
QJsonObject joined(QJsonObject a, const QJsonObject &b) {
    for (auto i = b.begin(); i != b.end(); ++i)
        if (i.value().toString().toLongLong() > a.value(i.key()).toString().toLongLong()) a.insert(i.key(), i.value());
    return a;
}
int rank(const QJsonObject &e) { return e.value("kind") == "directory" ? 2 : e.value("kind") == "file" ? 1 : 0; }
QString conflictPath(const QJsonObject &loser) {
    const auto path = loser.value("path").toString();
    const auto name = path.section('/', -1);
    return path.left(path.lastIndexOf('/') + 1) + (name.toUtf8().size() <= 160 ? name : QString("file"))
        + ".sync-conflict-" + loser.value("version").toString().left(32);
}
}

class Replica::Private {
public:
    struct Manifest {
        QString id, replica;
        qint64 next = 0, total = 0, after = 0, through = 0, sequence = 0;
        bool complete = false;
        QCryptographicHash digest{QCryptographicHash::Sha256};
    };
    QHash<QString, std::shared_ptr<Manifest>> manifests;
    detail::ConfinedFiles files;
    std::optional<iiSocietyContainer::SocietyDrive> drive;
    QSqlDatabase db; QString connection, scope, id; mutable QString error;
    QString proposal, proposalBase;
    qint64 publishedDataVersion = -1, publishedChanges = -1;
    detail::NamespaceJournal journal() const {
        detail::NamespaceJournal j;
        j.db = db; j.space = drive ? drive->identifier() : QString();
        j.authority = meta("host").isEmpty() ? id : meta("hostReplica");
        return j;
    }
    bool fail(const QString &e) const { error = e; return false; }
    bool sql(const QString &text) const { QSqlQuery q(db); return q.exec(text) || fail(q.lastError().text()); }
    bool ready() {
        error.clear(); files.error.clear();
        if (!db.isOpen() || scope.isEmpty() || id.isEmpty() || !drive || !drive->isValid() || !files.intact()) return fail("container_unavailable_or_replaced");
        detail::FileState state;
        if (!files.state(".society-sync", &state) || state.kind != "directory") return fail("sync_state_redirected");
        for (const auto *name : {"journal.sqlite", "journal.sqlite-wal", "journal.sqlite-shm", "operation.lock"}) {
            const QFileInfo f(QDir(files.root).filePath(".society-sync/" + QString::fromLatin1(name)));
            if (f.isSymLink() || f.isJunction()) return fail("sync_state_redirected");
        }
        return true;
    }
    std::unique_ptr<QLockFile> lock(int timeoutMs = 0) {
        if (!ready()) return {};
        auto lock = std::make_unique<QLockFile>(QDir(files.root).filePath(".society-sync/operation.lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(timeoutMs)) { fail("sync_store_busy"); return {}; }
        return lock;
    }
    QString meta(const QString &key) const {
        QSqlQuery q(db); q.prepare("SELECT value FROM metadata WHERE key=?"); q.addBindValue(key);
        if (!q.exec()) { fail(q.lastError().text()); return {}; } return q.next() ? q.value(0).toString() : QString();
    }
    bool setMeta(const QString &key, const QString &value) {
        QSqlQuery q(db); q.prepare("INSERT INTO metadata(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        q.addBindValue(key); q.addBindValue(value); return q.exec() || fail(q.lastError().text());
    }
    QJsonObject binding() const {
        if (meta("host").isEmpty()) return {};
        return {{"schema", 1}, {"host", meta("host")}, {"replica", meta("hostReplica")},
            {"container", meta("target")}, {"scope", meta("scope")},
            {"complete", meta("bootstrapComplete") == "1"}, {"recovery", meta("detached")}};
    }
    bool writeBinding() {
        const auto value = binding(); if (value.isEmpty()) return true;
        detail::FileState state;
        if (!files.state(".society-sync/mirror.json", &state)) return fail(files.error);
        QSaveFile output(QDir(files.root).filePath(".society-sync/mirror.json"));
        const auto bytes = json(value);
        if (!output.open(QIODevice::WriteOnly) || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            || output.write(bytes) != bytes.size() || !output.commit()) return fail("mirror_binding_write_failed");
        return true;
    }
    // A committed plan precedes every rename. Section roots never disappear,
    // so SocietyDrive can still open the source after a crash at any step.
    bool resumeAdoption() {
        if (meta("adoption") != "detaching") return writeBinding();
        if (!writeBinding()) return false;
        const auto adopted = iiSocietyContainer::SocietyDrive::adoptReplicaIdentity(
            files.root, meta("container"), meta("target"), &error);
        if (!adopted) return false;
        drive = adopted;
        QSqlQuery plan(db);
        if (!plan.exec("SELECT path,target FROM detach ORDER BY path")) return fail(plan.lastError().text());
        while (plan.next()) {
            if (!files.detach(plan.value(0).toString(), plan.value(1).toString())) return fail(files.error);
        }
        plan.finish();
        if (!db.transaction()) return fail("journal_transaction_failed");
        const bool ok = sql("DELETE FROM entries") && sql("DELETE FROM peers") && sql("DELETE FROM detach") && journal().clear()
            && setMeta("container", drive->identifier()) && setMeta("sequence", "0") && setMeta("counter", "0")
            && setMeta("replica", QUuid::createUuid().toString(QUuid::WithoutBraces))
            && setMeta("adoption", "pulling");
        if (!ok || !db.commit()) { db.rollback(); return fail("adoption_commit_failed"); }
        id = meta("replica"); return writeBinding();
    }
    QJsonObject entry(const QString &path, QString *stamp = nullptr) const {
        QSqlQuery q(db); q.prepare("SELECT data,stamp,sequence FROM entries WHERE path=?"); q.addBindValue(path);
        if (!q.exec()) { fail(q.lastError().text()); return {}; } if (!q.next()) return {};
        auto e = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        if (stamp) *stamp = q.value(1).toString(); e.insert("sequence", QString::number(q.value(2).toLongLong())); return e;
    }
    bool save(QJsonObject e, const QString &stamp) {
        const auto input = e; e = versioned(e); const auto previous = entry(e.value("path").toString());
        if (!sql("SAVEPOINT namespace_write")) return false;
        auto abort = [&] { sql("ROLLBACK TO namespace_write"); sql("RELEASE namespace_write"); return false; };
        auto ledger = journal();
        if (meta("host").isEmpty()) {
            e = ledger.commit(e, proposal, proposalBase);
            if (e.isEmpty()) { fail(ledger.error); return abort(); }
        } else if (input.contains("revision")) {
            for (const auto *key : {"namespace", "authority", "revision", "sequence"}) e.insert(key, input.value(key));
        } else {
            // Retain the original confirmed base through repeated offline edits.
            const auto base = input.contains("baseRevision") ? input.value("baseRevision")
                : previous.contains("baseRevision") ? previous.value("baseRevision")
                : previous.contains("revision") ? previous.value("revision")
                : ledger.object(e.value("path").toString()).value("revision");
            e.insert("baseRevision", base.toString());
        }
        qint64 seq = previous.value("sequence").toString().toLongLong();
        if (previous.value("version") != e.value("version")) {
            seq = meta("sequence").toLongLong() + 1;
            if (seq > MaxCounter || !setMeta("sequence", QString::number(seq))) { fail("journal_sequence_limit"); return abort(); }
        }
        QSqlQuery q(db); q.prepare("INSERT INTO entries(path,data,stamp,sequence) VALUES(?,?,?,?) ON CONFLICT(path) DO UPDATE SET data=excluded.data,stamp=excluded.stamp,sequence=excluded.sequence");
        q.addBindValue(e.value("path").toString()); q.addBindValue(json(e)); q.addBindValue(stamp.isNull() ? QString("") : stamp); q.addBindValue(seq);
        if (!q.exec()) { fail(q.lastError().text()); return abort(); }
        if (stamp != "remote" && e.value("kind") == "file" && e.contains("revision")
            && !ledger.locate(e.value("version").toString(), {{"provider", "local"}, {"kind", "local"}, {"key", physical(e.value("path").toString())}})) {
            fail(ledger.error); return abort();
        }
        return sql("RELEASE namespace_write");
    }
    bool observed(const QString &path, const detail::FileState &state, const QString &verifiedHash = {}) {
        QString oldStamp; const auto old = entry(path, &oldStamp);
        if (!error.isEmpty()) return false;
        // A catalog entry without a local payload is not a user deletion.
        if (oldStamp == "remote" && state.kind == "deleted") return true;
        if (old.isEmpty() && state.kind == "deleted") return true;
        if (old.value("kind").toString() == state.kind && oldStamp == state.stamp) return true;
        const auto hash = state.kind == "file" ? (verifiedHash.isEmpty() ? files.hash(physical(path), state.stamp) : verifiedHash) : QString("");
        if (state.kind == "file" && hash.isEmpty()) return fail(files.error);
        QJsonObject e{{"path", path}, {"kind", state.kind}, {"size", QString::number(state.size)}, {"hash", hash}, {"clock", old.value("clock").toObject()}};
        if (contentEqual(e, old)) return save(old, state.stamp);
        auto clock = e.value("clock").toObject();
        const auto counter = qMax(meta("counter").toLongLong(), clock.value(id).toString().toLongLong()) + 1;
        if (counter > MaxCounter || clock.size() > 63 && !clock.contains(id) || !setMeta("counter", QString::number(counter))) return fail("replica_clock_limit");
        clock.insert(id, QString::number(counter)); e.insert("clock", clock); return save(e, state.stamp);
    }
    bool refresh(const QString &path) {
        detail::FileState state;
        if (!files.exactPath(physical(path)) || !files.state(physical(path), &state)) return fail(files.error);
        return observed(path, state);
    }
    QString transfer(const QString &peer, const QJsonObject &e) const {
        return ".society-sync/transfers/" + digest(peer.toUtf8() + '\n' + e.value("version").toString().toUtf8()) + ".part";
    }
    QJsonObject failed(const QString &message = {}) { return {{"ok", false}, {"error", message.isEmpty() ? error : message}}; }
    QJsonObject result() { return {{"ok", true}, {"complete", true}}; }
    // On first pass determine whether bytes are required. On commit apply the
    // same decision against a freshly observed destination, preserving conflicts.
    QJsonObject merge(const QString &peer, const QJsonObject &incoming, bool commit) {
        const auto path = incoming.value("path").toString();
        if (!refresh(path)) return failed();
        QString localStamp; auto local = entry(path, &localStamp);
        const bool absentPayload = localStamp == "remote";
        const auto a = incoming.value("clock").toObject(), b = local.value("clock").toObject();
        const bool mirror = !meta("host").isEmpty();
        const bool proposed = !mirror && incoming.contains("baseRevision");
        if (mirror && (peer != meta("host") || !journal().contains(incoming))) return failed("host_revision_required");
        if (mirror && !local.contains("revision") && compare(b, a) == 1) {
            // The user edited again while the host acknowledged an earlier
            // proposal. Keep the newer working edit and advance only its base.
            local.insert("baseRevision", incoming.value("revision"));
            QString stamp; entry(path, &stamp);
            return save(local, stamp) ? result() : failed();
        }
        const auto confirmed = journal().object(path);
        const auto relation = mirror ? (local.value("version") == incoming.value("version") && !absentPayload ? 0 : 1)
            : proposed ? (local.value("version") == incoming.value("version") ? 0
                : incoming.value("baseRevision").toString() == confirmed.value("revision").toString() ? 1 : 2)
            : local.isEmpty() ? 1 : compare(a, b);
        if (relation == 0 && !contentEqual(incoming, local)) return failed("inconsistent_version_clock");
        if (relation <= 0) {
            if (mirror) { QString stamp; entry(path, &stamp); if (!save(incoming, stamp)) return failed(); }
            return result();
        }
        auto clock = mirror ? a : joined(a, b); if (clock.size() > 64) return failed("replica_clock_limit");
        const bool same = !absentPayload && contentEqual(incoming, local);
        // A nonempty local directory must never be replaced by a file/deletion.
        bool retainDirectory = false;
        const bool fixedDirectory = path.startsWith("files/") && iiSocietyContainer::isFixedFilesDirectory(path.mid(6));
        if (local.value("kind") == "directory" && incoming.value("kind") != "directory") {
            bool ok; const auto children = files.list(physical(path), &ok);
            if (!ok) return failed(files.error);
            retainDirectory = !children.isEmpty() || fixedDirectory;
        }
        if (retainDirectory && fixedDirectory && !mirror) {
            // Publish the invariant as a newer directory revision, so replayed
            // legacy tombstones are acknowledged and cannot stall synchronization.
            const auto counter = qMax(meta("counter").toLongLong(), clock.value(id).toString().toLongLong()) + 1;
            if (counter > MaxCounter || (clock.size() == 64 && !clock.contains(id))
                || !setMeta("counter", QString::number(counter))) return failed("replica_clock_limit");
            clock.insert(id, QString::number(counter));
        }
        if (mirror && retainDirectory && !same) {
            // Unsynchronized children cannot be destroyed to materialize a
            // parent. Submit the directory against this host revision first;
            // the authority preserves a replaced file as a conflict copy.
            for (const auto *key : {"namespace", "authority", "revision", "sequence"}) local.remove(key);
            local.insert("baseRevision", incoming.value("revision")); local.insert("clock", joined(a, b));
            QString stamp; entry(path, &stamp);
            return save(local, stamp) ? result() : failed();
        }
        // A mirror materializes only its host's choice. On the host, a stale
        // proposal cannot replace a newer committed object, regardless of clocks.
        const bool pending = mirror && !local.isEmpty() && !local.contains("revision");
        const bool structural = (incoming.value("kind") == "directory" && local.value("kind") == "file")
            || (incoming.value("kind") == "file" && local.value("kind") == "directory");
        const bool concurrent = relation == 2 || retainDirectory || pending || structural;
        const bool incomingWins = !retainDirectory && (mirror || relation == 1 || same || incoming.value("kind") == "directory" || (!proposed && (rank(incoming) > rank(local)
            || (rank(incoming) == rank(local) && incoming.value("hash").toString() > local.value("hash").toString()))));
        auto winner = incomingWins ? incoming : local;
        const auto loser = incomingWins ? local : incoming;
        const bool conflict = concurrent && !same && loser.value("kind") == "file" && !(incomingWins && absentPayload);
        const bool bytesNeeded = incoming.value("kind") == "file" && !same && (incomingWins || conflict);
        const auto part = transfer(peer, incoming);
        if (bytesNeeded && !commit) {
            detail::FileState partial;
            if (!files.state(part, &partial)) return failed(files.error);
            if (partial.kind == "deleted" && !files.append(part, 0, {})) return failed(files.error);
            if (partial.size > incoming.value("size").toString().toLongLong()) return failed("invalid_partial_transfer");
            return {{"ok", true}, {"complete", false}, {"offset", QString::number(partial.size)}};
        }
        if (bytesNeeded) {
            detail::FileState partial;
            if (!files.state(part, &partial) || partial.kind != "file" || partial.size != incoming.value("size").toString().toLongLong())
                return failed("incomplete_or_corrupt_transfer");
            const auto hash = files.hash(part, partial.stamp);
            if (hash.isEmpty()) return failed(files.error);
            if (hash != incoming.value("hash").toString()) {
                // Keep the destination intact and let the next attempt start
                // again, instead of endlessly resuming a corrupt complete part.
                if (!files.remove(part)) return failed(files.error);
                return failed("incomplete_or_corrupt_transfer");
            }
        }
        if (conflict) {
            auto copy = loser; copy.insert("path", conflictPath(loser)); copy.insert("clock", clock);
            for (const auto *key : {"namespace", "authority", "revision", "sequence"}) copy.remove(key);
            copy.insert("baseRevision", "");
            const auto target = physical(copy.value("path").toString()); if (target.isEmpty()) return failed("conflict_path_too_long");
            detail::FileState existing; if (!files.state(target, &existing)) return failed(files.error);
            if (existing.kind != "deleted") {
                if (existing.kind != "file" || files.hash(target, existing.stamp) != loser.value("hash").toString()) return failed("conflict_name_collision");
            } else if (!(incomingWins ? files.preserve(physical(path), target) : files.install(part, target))) return failed(files.error);
            if (!files.state(target, &existing) || !save(copy, existing.stamp)) return failed(files.error);
        }
        const auto target = physical(path);
        if (incomingWins && !same) {
            const auto kind = winner.value("kind").toString();
            if (kind == "file") {
                if (local.value("kind") == "directory" && !files.remove(target)) return failed(files.error);
                if (!files.install(part, target)) return failed(files.error);
            } else if (kind == "directory") {
                if (local.value("kind") == "file" && !files.remove(target)) return failed(files.error);
                if (!files.mkdir(target)) return failed(files.error);
            } else if (!files.remove(target)) return failed(files.error);
        }
        winner.insert("clock", clock);
        detail::FileState state; if (!files.state(target, &state)) return failed(files.error);
        if (!save(winner, state.stamp)) return failed();
        return result();
    }
};

Replica::Replica() : d(std::make_unique<Private>()) {}
Replica::~Replica() { close(); }
void Replica::close() { d->publishedDataVersion = d->publishedChanges = -1; d->manifests.clear(); d->drive.reset(); d->db.close(); d->db = {}; if (!d->connection.isEmpty()) QSqlDatabase::removeDatabase(d->connection); d->connection.clear(); d->scope.clear(); d->id.clear(); }
QString Replica::errorString() const { return d->error; }
bool Replica::isOpen() const { return d->db.isOpen() && !d->scope.isEmpty() && !d->id.isEmpty(); }
QString Replica::containerId() const { return d->drive ? d->drive->identifier() : QString(); }
QString Replica::replicaId() const { return d->id; }
QString Replica::accountScope() const { return d->scope; }
void Replica::setCancellation(std::function<bool()> cancelled) { d->files.cancelled = std::move(cancelled); }
void Replica::setVerificationProgress(std::function<void(const QString &, qint64, qint64)> progress) {
    d->files.hashProgress = std::move(progress);
}
bool Replica::open(const QString &container, const QString &accountScope) {
    close(); d->error.clear();
    if (!hex(accountScope)) return d->fail("invalid_account_scope");
    const auto filesystem = QStorageInfo(container).fileSystemType().toLower();
    for (const auto *type : {"nfs", "smb", "cifs", "afp", "sshfs", "webdav", "9p"})
        if (filesystem.contains(type)) return d->fail("sync_requires_device_local_storage");
    d->drive = iiSocietyContainer::SocietyDrive::open(container, &d->error);
    if (!d->drive || !d->files.open(d->drive->rootPath())) return d->fail(d->error.isEmpty() ? d->files.error : d->error);
    if (!d->files.mkdir(".society-sync/transfers") || !d->files.mkdir(".society-sync/recovery")) return d->fail(d->files.error);
    const auto state = QDir(d->files.root).filePath(".society-sync");
    for (const auto *name : {"journal.sqlite", "journal.sqlite-wal", "journal.sqlite-shm", "operation.lock"})
        if (QFileInfo(QDir(state).filePath(name)).isSymLink()) return d->fail("sync_state_redirected");
    QLockFile lock(QDir(state).filePath("operation.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock(2000)) return d->fail("sync_store_busy");
    if (d->files.cancelled && d->files.cancelled()) return d->fail("cancelled");
    d->connection = "society-sync-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->db = QSqlDatabase::addDatabase("QSQLITE", d->connection); d->db.setDatabaseName(QDir(state).filePath("journal.sqlite"));
    d->db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    if (!d->db.open()) return d->fail(d->db.lastError().text());
    if (!QFile::setPermissions(d->db.databaseName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return d->fail("state_permissions_failed");
    int schema = -1;
    { QSqlQuery q(d->db); if (q.exec("PRAGMA user_version") && q.next()) schema = q.value(0).toInt(); }
    if (schema < 0 || schema > 3) return d->fail("unsupported_journal_schema");
    if (!d->sql("PRAGMA journal_mode=WAL") || !d->sql("PRAGMA synchronous=FULL")) return false;
    if (schema == 0) {
        if (!d->db.transaction()) return d->fail("journal_transaction_failed");
        const QStringList statements{
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)",
            "CREATE TABLE entries(path TEXT PRIMARY KEY,data BLOB NOT NULL,stamp TEXT NOT NULL,sequence INTEGER NOT NULL)",
            "CREATE INDEX entry_sequence ON entries(sequence)",
            "CREATE TABLE peers(peer TEXT PRIMARY KEY,replica TEXT NOT NULL,container TEXT NOT NULL,pulled INTEGER NOT NULL,pushed INTEGER NOT NULL)",
            "PRAGMA user_version=1"};
        for (const auto &s : statements) if (!d->sql(s)) { d->db.rollback(); return false; }
        if (!d->setMeta("scope", accountScope) || !d->setMeta("container", containerId())
            || !d->setMeta("replica", QUuid::createUuid().toString(QUuid::WithoutBraces)) || !d->setMeta("sequence", "0") || !d->setMeta("counter", "0") || !d->db.commit()) {
            d->db.rollback(); return d->fail("journal_initialization_failed");
        }
    }
    if (schema < 2 && (!d->sql("CREATE TABLE IF NOT EXISTS detach(path TEXT PRIMARY KEY,target TEXT NOT NULL)")
        || !d->sql("PRAGMA user_version=2"))) return false;
    if (!d->journal().initialize()) return d->fail("namespace_initialization_failed");
    if (d->meta("scope") != accountScope
        || (d->meta("container") != containerId()
            && !(d->meta("adoption") == "detaching" && d->meta("target") == containerId()))) {
        close(); return d->fail("container_account_binding_mismatch");
    }
    d->scope = accountScope;
    if (!d->resumeAdoption()) return false;
    d->scope = accountScope; d->id = d->meta("replica");
    if (QUuid(d->id).isNull()) { close(); return d->fail("invalid_replica_identity"); }
    if (schema < 3) {
        if (!d->db.transaction()) return d->fail("journal_transaction_failed");
        bool ok = true;
        if (d->meta("host").isEmpty()) {
            QList<QPair<QJsonObject, QString>> baseline; QSqlQuery q(d->db);
            if (!q.exec("SELECT data,stamp FROM entries ORDER BY sequence")) ok = false;
            while (ok && q.next()) baseline.append({QJsonDocument::fromJson(q.value(0).toByteArray()).object(), q.value(1).toString()});
            q.finish();
            for (const auto &item : baseline) if (!d->save(item.first, item.second)) { ok = false; break; }
        } else {
            // Reconcile the whole canonical snapshot after upgrading a mirror;
            // its existing local edits remain proposals until the host decides.
            ok = d->sql("UPDATE peers SET pulled=0,pushed=0");
        }
        if (!ok || !d->sql("PRAGMA user_version=3") || !d->db.commit()) { d->db.rollback(); return d->fail("namespace_migration_failed"); }
    }
    return true;
}
QJsonObject Replica::binding() const { return isOpen() ? d->binding() : QJsonObject(); }
QJsonObject Replica::binding(const QString &containerPath) {
    const auto drive = iiSocietyContainer::SocietyDrive::open(containerPath);
    if (!drive) return {};
    const QFileInfo directory(QDir(drive->rootPath()).filePath(".society-sync"));
    const QFileInfo info(QDir(directory.filePath()).filePath("mirror.json"));
    if (!directory.isDir() || directory.isSymLink() || directory.isJunction()
        || !info.isFile() || info.isSymLink() || info.isJunction() || info.size() > 8192) return {};
    QFile input(info.filePath()); if (!input.open(QIODevice::ReadOnly)) return {};
    const auto value = QJsonDocument::fromJson(input.readAll()).object();
    if (value.value("schema") != 1 || value.value("host").toString().isEmpty()
        || value.value("host").toString().size() > 256 || QUuid(value.value("container").toString()).isNull()
        || !hex(value.value("scope").toString()) || !value.value("complete").isBool()) return {};
    return value;
}
bool Replica::bootstrapping() const { return isOpen() && !d->meta("host").isEmpty() && d->meta("bootstrapComplete") != "1"; }
QString Replica::primaryHost(const QString &containerPath, const QString &scope) {
    const auto drive = iiSocietyContainer::SocietyDrive::open(containerPath);
    if (!drive || !hex(scope)) return {};
    const QFileInfo dir(QDir(drive->rootPath()).filePath(".society-sync"));
    const QFileInfo info(QDir(dir.filePath()).filePath("primary.json"));
    if (dir.isSymLink() || dir.isJunction() || !info.isFile() || info.isSymLink() || info.size() > 8192) return {};
    QFile input(info.filePath()); if (!input.open(QIODevice::ReadOnly)) return {};
    const auto value = QJsonDocument::fromJson(input.readAll()).object();
    if (value.value("schema") != 1 || value.value("scope") != scope || value.value("container") != drive->identifier()
        || value.value("host").toString().size() > 128) return {};
    return value.value("host").toString();
}
bool Replica::claimPrimaryHost(const QString &containerPath, const QString &scope, const QString &device) {
    const auto drive = iiSocietyContainer::SocietyDrive::open(containerPath);
    if (!drive || !hex(scope) || device.isEmpty() || device.size() > 128 || !binding(containerPath).isEmpty()) return false;
    const auto existing = primaryHost(containerPath, scope);
    if (!existing.isEmpty()) return existing == device;
    detail::ConfinedFiles files;
    if (!files.open(drive->rootPath()) || !files.mkdir(".society-sync")) return false;
    QLockFile lock(QDir(files.root).filePath(".society-sync/operation.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock()) return false;
    detail::FileState state; if (!files.state(".society-sync/primary.json", &state)) return false;
    // Recheck after taking the lock; a concurrent claimant may have committed
    // between the optimistic read above and lock acquisition.
    if (state.kind != "deleted") return primaryHost(containerPath, scope) == device;
    const auto data = json({{"schema", 1}, {"scope", scope}, {"container", drive->identifier()}, {"host", device}});
    QSaveFile output(QDir(files.root).filePath(".society-sync/primary.json"));
    return output.open(QIODevice::WriteOnly) && output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
        && output.write(data) == data.size() && output.commit();
}
bool Replica::completeBootstrap() {
    auto lock = d->lock(); if (!lock) return false;
    if (d->meta("adoption") != "pulling" || d->meta("target") != containerId()) return d->fail("mirror_not_initialized");
    if (!iiSocietyContainer::SocietyDrive::completeReplica(d->files.root, containerId(), &d->error)) return false;
    if (!d->setMeta("bootstrapComplete", "1")) return false;
    return d->writeBinding();
}
bool Replica::bindHost(const QString &peer, const QString &replica, const QString &container) {
    auto lock = d->lock(); if (!lock) return false;
    if (!primaryHost(d->files.root, d->scope).isEmpty()) return d->fail("authority_cannot_become_mirror");
    if (peer.isEmpty() || peer.size() > 256 || QUuid(replica).isNull() || replica == replicaId()
        || QUuid(container).isNull() || QUuid(container).toString(QUuid::WithoutBraces) != container) return d->fail("invalid_host_identity");
    if (!d->meta("host").isEmpty() && d->meta("host") != peer) return d->fail("different_primary_host");
    if (!d->resumeAdoption()) return false;
    if (d->meta("host") == peer && d->meta("target") == container) {
        if (d->meta("hostReplica") != replica) {
            // The same logical host restored its journal. Start its cursors from
            // zero, retaining this replica's durable clocks and offline edits.
            QSqlQuery q(d->db); q.prepare("DELETE FROM peers WHERE peer=?"); q.addBindValue(peer);
            if (!d->db.transaction()) return d->fail("journal_transaction_failed");
            if (!q.exec() || !d->journal().clear() || !d->setMeta("hostReplica", replica) || !d->db.commit()) {
                d->db.rollback(); return d->fail("host_checkpoint_reset_failed");
            }
        }
        return d->writeBinding();
    }
    const auto recovery = ".society-sync/detached/" + containerId() + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d->files.mkdir(recovery) || !d->files.preserve(".society-drive.json", recovery + "/manifest.json")) return d->fail(d->files.error);
    if (!d->db.transaction()) return d->fail("journal_transaction_failed");
    bool ok = d->sql("DELETE FROM detach"); qint64 count = 0;
    for (const auto section : iiSocietyContainer::allStoreSections()) {
        const auto native = iiSocietyContainer::storeSectionName(section); bool listed;
        const auto names = d->files.list(native, &listed); if (!listed) { ok = false; break; }
        for (const auto &name : names) {
            QStringList paths{native + '/' + name};
            if (section == iiSocietyContainer::StoreSection::Files && iiSocietyContainer::isFixedFilesDirectory(name)) {
                // Archive former contents but keep each fixed directory in place
                // throughout host adoption, including an interrupted first mirror.
                paths.clear();
                const auto children = d->files.list(native + '/' + name, &listed);
                if (!listed) { ok = false; break; }
                for (const auto &child : children) paths.append(native + '/' + name + '/' + child);
            }
            for (const auto &path : paths) {
                if (++count > 250000) { ok = false; break; }
                QSqlQuery q(d->db); q.prepare("INSERT INTO detach(path,target) VALUES(?,?)");
                q.addBindValue(path); q.addBindValue(recovery + '/' + path);
                if (!q.exec()) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    ok = ok && d->setMeta("host", peer) && d->setMeta("hostReplica", replica) && d->setMeta("target", container)
        && d->setMeta("detached", recovery) && d->setMeta("bootstrapComplete", "0") && d->setMeta("adoption", "detaching");
    if (!ok || !d->db.commit()) { d->db.rollback(); return d->fail("mirror_plan_failed"); }
    return d->resumeAdoption();
}
bool Replica::validRecord(const QJsonObject &e) {
    if (physical(e.value("path").toString()).isEmpty() || e.value("version").toString() != versioned(e).value("version").toString()) return false;
    const auto kind = e.value("kind").toString(); qint64 size;
    if (!number(e.value("size"), &size) || (kind != "file" && kind != "directory" && kind != "deleted")) return false;
    if (kind == "file" ? !hex(e.value("hash").toString()) : size != 0 || !e.value("hash").toString().isEmpty()) return false;
    if (e.contains("baseRevision") && (!e.value("baseRevision").isString()
        || (!e.value("baseRevision").toString().isEmpty() && !hex(e.value("baseRevision").toString())))) return false;
    const auto clock = e.value("clock").toObject(); if (clock.isEmpty() || clock.size() > 64) return false;
    for (auto i = clock.begin(); i != clock.end(); ++i) { qint64 n; if (QUuid(i.key()).isNull() || !number(i.value(), &n) || !n) return false; }
    return json(e).size() <= 16384;
}
bool Replica::scan() {
    { auto lock = d->lock(2000); if (!lock) return false; }
    struct Work { QString path; detail::FileState state; QString hash; };
    QList<Work> work, pending;
    QSet<QString> seen; bool ok = true;
    std::function<void(QString, QString)> visit = [&](const QString &native, const QString &key) {
        bool read; const auto names = d->files.list(native, &read); if (!read) { ok = d->fail(d->files.error); return; }
        for (const auto &name : names) {
            if (name.startsWith(".society-") || name.startsWith(".iiserverhost-")) continue;
            const auto path = key + '/' + name; detail::FileState state;
            if (physical(path).isEmpty() || seen.size() >= 250000 || !d->files.state(native + '/' + name, &state)) { ok = d->fail("unsupported_or_unavailable_entry"); return; }
            if (state.kind == "deleted") { ok = d->fail("directory_changed_during_scan"); return; }
            seen.insert(path); work.append({path, state, {}});
            if (state.kind == "directory") visit(native + '/' + name, path);
            if (!ok) return;
        }
    };
    for (const auto section : iiSocietyContainer::allStoreSections()) {
        visit(iiSocietyContainer::storeSectionName(section), iiSocietyContainer::storeSectionKey(section)); if (!ok) break;
    }
    if (!ok) return false;
    // Enumerate first, then make directories and small metadata/previews durable
    // before reading a multi-GB model. No database or operation lock spans a hash.
    std::sort(work.begin(), work.end(), [](const Work &a, const Work &b) {
        if (a.state.kind != b.state.kind) return a.state.kind == "directory";
        if (a.state.kind == "directory" && a.path.count('/') != b.path.count('/'))
            return a.path.count('/') < b.path.count('/');
        if (a.state.size != b.state.size) return a.state.size < b.state.size;
        return a.path < b.path;
    });
    QElapsedTimer batch; batch.start();
    const auto flush = [&]() {
        if (pending.isEmpty()) return true;
        auto lock = d->lock(2000); if (!lock) return false;
        if (!d->db.transaction()) return d->fail("journal_transaction_failed");
        for (const auto &item : std::as_const(pending)) {
            detail::FileState current;
            if (!d->files.state(physical(item.path), &current)) { d->db.rollback(); return d->fail(d->files.error); }
            if (current.kind != item.state.kind || current.stamp != item.state.stamp) {
                d->db.rollback(); return d->fail("file_changed_during_scan");
            }
            if (!d->observed(item.path, current, item.hash)) { d->db.rollback(); return false; }
        }
        if (!d->db.commit()) { d->db.rollback(); return d->fail(d->db.lastError().text()); }
        pending.clear(); batch.restart(); return true;
    };
    for (auto &item : work) {
        QString stamp; const auto old = d->entry(item.path, &stamp);
        if (!d->error.isEmpty()) return false;
        if (old.value("kind") == item.state.kind && stamp == item.state.stamp) continue;
        if (item.state.kind == "file") {
            if (item.state.size > 1024 * 1024 && !flush()) return false;
            item.hash = d->files.hash(physical(item.path), item.state.stamp);
            if (item.hash.isEmpty()) return d->fail(d->files.error);
        }
        pending.append(item);
        if ((pending.size() >= 64 || batch.elapsed() >= 50 || item.state.size > 1024 * 1024) && !flush()) return false;
    }
    if (!flush()) return false;
    // Absence from the earlier walk is only a candidate deletion. An upload or
    // another local app may have recreated the path while a model was hashing.
    auto lock = d->lock(2000); if (!lock) return false;
    QStringList absent; QSqlQuery q(d->db);
    if (!q.exec("SELECT path FROM entries")) return d->fail(q.lastError().text());
    while (q.next()) if (!seen.contains(q.value(0).toString())) absent.append(q.value(0).toString());
    q.finish();
    if (!d->db.transaction()) return d->fail("journal_transaction_failed");
    for (const auto &path : absent) {
        detail::FileState current;
        if (!d->files.state(physical(path), &current)) { d->db.rollback(); return d->fail(d->files.error); }
        if (current.kind == "deleted" && !d->observed(path, current)) { d->db.rollback(); return false; }
    }
    if (!d->db.commit()) { d->db.rollback(); return d->fail(d->db.lastError().text()); }
    return true;
}
QJsonObject Replica::record(const QString &path) const { return d->db.isOpen() ? d->entry(path) : QJsonObject(); }
bool Replica::resident(const QString &path) const {
    QString stamp; const auto e = d->entry(path, &stamp);
    if (e.value("kind") != "file" || stamp == "remote") return false;
    detail::FileState state;
    return d->files.state(physical(path), &state) && state.kind == "file" && state.stamp == stamp;
}
bool Replica::acceptMetadata(const QString &peer, const QJsonObject &incoming) {
    auto lock = d->lock(); if (!lock) return false;
    if (!validRecord(incoming) || incoming.value("kind") != "file" || peer != d->meta("host")
        || peer.isEmpty() || !d->journal().contains(incoming)) return d->fail("host_revision_required");
    const auto path = incoming.value("path").toString();
    if (!d->refresh(path)) return false;
    QString stamp; const auto old = d->entry(path, &stamp);
    if (!old.isEmpty() && !old.contains("revision")) return d->fail("pending_local_change_requires_merge");
    if (resident(path) && contentEqual(old, incoming)) return d->save(incoming, stamp);
    if (old.value("kind") == "directory") return d->fail("structural_change_requires_merge");
    // Only invalidate a verified, unmodified cache. Pending local edits always
    // use the conflict-preserving merge path above, never this eviction path.
    if (stamp != "remote" && old.value("kind") == "file" && !d->files.remove(physical(path))) return d->fail(d->files.error);
    return d->save(incoming, "remote");
}
bool Replica::publishStorageMap() {
    auto lock = d->lock(); if (!lock) return false;
    QSqlQuery version(d->db), changes(d->db);
    if (!version.exec("PRAGMA data_version") || !version.next()
        || !changes.exec("SELECT total_changes()") || !changes.next()) return d->fail("catalog_revision_unavailable");
    const auto dataVersion = version.value(0).toLongLong(), localChanges = changes.value(0).toLongLong();
    if (d->publishedDataVersion == dataVersion && d->publishedChanges == localChanges
        && QFileInfo::exists(QDir(d->drive->rootPath()).filePath(".society-sync/catalog.json"))) return true;
    QSqlQuery q(d->db); if (!q.exec("SELECT data,stamp FROM entries ORDER BY path")) return d->fail(q.lastError().text());
    QJsonArray objects;
    while (q.next()) {
        auto e = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        e.insert("resident", e.value("kind") == "file" && q.value(1).toString() != "remote");
        objects.append(e);
    }
    if (!iiSocietyContainer::StorageMap(*d->drive).publish(objects, &d->error)) return false;
    d->publishedDataVersion = dataVersion; d->publishedChanges = localChanges; return true;
}
QStringList Replica::requestedPaths() {
    if (!isOpen()) return {};
    iiSocietyContainer::StorageMap map(*d->drive); QStringList result;
    for (const auto &value : map.pendingRequests()) {
        const auto request = value.toObject(); QStringList paths; bool valid = true;
        for (const auto &item : request.value("objects").toArray()) {
            const auto object = item.toObject(); const auto path = object.value("path").toString();
            auto current = objectMetadata(path);
            if (current.isEmpty()) current = record(path);
            if (current.value("kind") != "file" || current.value("version") != object.value("version")) { valid = false; break; }
            paths.append(path);
        }
        if (!valid) map.finishRequest(request.value("id").toString(), "object_changed_select_again");
        else result.append(paths);
    }
    result.removeDuplicates(); return result;
}
void Replica::completeRequests() {
    if (!isOpen()) return;
    requestedPaths(); // Reject stale versions before reporting local availability.
    iiSocietyContainer::StorageMap map(*d->drive);
    for (const auto &value : map.pendingRequests()) {
        const auto request = value.toObject(); bool ready = true;
        for (const auto &item : request.value("objects").toArray()) {
            const auto e = item.toObject(); ready &= resident(e.value("path").toString());
        }
        if (ready) map.finishRequest(request.value("id").toString());
    }
}
QJsonArray Replica::missingPreviews() const {
    QJsonArray result;
    if (!isOpen()) return result;
    QSqlQuery q(d->db); if (!q.exec("SELECT data FROM entries")) return result;
    const QStringList extensions{"png", "jpg", "jpeg", "webp", "heic", "heif"};
    while (q.next()) {
        auto e = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        const auto path = e.value("path").toString(); const auto hash = e.value("hash").toString();
        if (e.value("kind") != "file" || !e.contains("revision") || path.contains("/.previews/")
            || !extensions.contains(QFileInfo(path).suffix().toLower())) continue;
        detail::FileState state;
        if (d->files.state(".society-sync/previews/" + hash + ".jpg", &state) && state.kind == "file") continue;
        if (d->files.state(".society-sync/previews/" + hash + ".none", &state) && state.kind == "file") continue;
        e = objectMetadata(path); e.remove("locations"); result.append(e);
        // Yield between bounded preview batches so a large gallery cannot
        // postpone a newly requested model or a completed image upload.
        if (result.size() >= 16) break;
    }
    return result;
}
bool Replica::savePreview(const QJsonObject &entry, const QJsonObject &response) {
    auto lock = d->lock(); if (!lock) return false;
    if (!validRecord(entry) || response.value("version") != entry.value("version")) return d->fail("invalid_preview_version");
    const auto decoded = QByteArray::fromBase64Encoding(response.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > 512 * 1024) return d->fail("invalid_preview");
    const auto prefix = ".society-sync/previews/" + entry.value("hash").toString();
    if (!d->files.mkdir(".society-sync/previews")) return d->fail(d->files.error);
    if (!decoded.decoded.isEmpty()) {
        QBuffer buffer; buffer.setData(decoded.decoded); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "JPEG");
        if (reader.size().width() > 512 || reader.size().height() > 512 || !reader.size().isValid()) return d->fail("invalid_preview_dimensions");
    }
    const auto target = prefix + (decoded.decoded.isEmpty() ? ".none" : ".jpg");
    detail::FileState state; if (!d->files.state(target, &state)) return d->fail(d->files.error);
    QSaveFile output(QDir(d->files.root).filePath(target));
    return output.open(QIODevice::WriteOnly) && output.write(decoded.decoded) == decoded.decoded.size() && output.commit();
}
QJsonObject Replica::namespaceState() const {
    if (!isOpen()) return {};
    auto state = d->journal().state(); state.insert("role", d->meta("host").isEmpty() ? "authority" : "replica");
    return state;
}
QJsonObject Replica::objectMetadata(const QString &path) const { return isOpen() ? d->journal().object(path) : QJsonObject(); }
QJsonObject Replica::revision(const QString &id) const { return isOpen() ? d->journal().revision(id) : QJsonObject(); }
bool Replica::placeObject(const QString &path, ObjectProvider &provider) {
    auto lock = d->lock(2000); if (!lock) return false;
    if (!d->meta("host").isEmpty()) return d->fail("provider_requires_authority");
    if (physical(path).isEmpty() || !d->refresh(path)) return d->fail("invalid_provider_object");
    const auto e = d->journal().object(path);
    if (e.value("kind") != "file") return d->fail("provider_requires_file");
    const ObjectIdentity object{containerId(), e.value("hash").toString(), e.value("size").toString().toLongLong()};
    // A receipt belongs only to the verified version, even if the file changes
    // during I/O. Remote storage never holds the authority's database lock.
    lock.reset();
    if (!provider.put(object, QDir(d->files.root).filePath(physical(path)), &d->error, d->files.cancelled)) return false;
    lock = d->lock(2000); if (!lock) return false;
    auto ledger = d->journal();
    return ledger.locate(e.value("version").toString(), {{"provider", provider.id()}, {"kind", provider.kind()}, {"key", object.key()}})
        || d->fail(ledger.error);
}
bool Replica::restoreObject(const QString &path, ObjectProvider &provider) {
    auto lock = d->lock(2000); if (!lock) return false;
    if (!d->meta("host").isEmpty()) return d->fail("provider_requires_authority");
    auto e = d->journal().object(path); detail::FileState state;
    if (physical(path).isEmpty() || e.value("kind") != "file" || !d->files.state(physical(path), &state)
        || state.kind != "deleted" || d->entry(path).value("version") != e.value("version")) return d->fail("provider_restore_would_overwrite_change");
    const ObjectIdentity object{containerId(), e.value("hash").toString(), e.value("size").toString().toLongLong()};
    const auto temporary = ".society-sync/transfers/provider-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".part";
    const auto absolute = QDir(d->files.root).filePath(temporary);
    lock.reset();
    if (!provider.get(object, absolute, &d->error, d->files.cancelled)) { QFile::remove(absolute); return false; }
    lock = d->lock(2000);
    if (!lock || !d->files.state(physical(path), &state) || state.kind != "deleted"
        || d->entry(path).value("version") != e.value("version")) { QFile::remove(absolute); return d->fail("provider_restore_would_overwrite_change"); }
    if (!d->files.install(temporary, physical(path)) || !d->files.state(physical(path), &state)) { QFile::remove(absolute); return d->fail(d->files.error); }
    return d->save(e, state.stamp);
}
QJsonObject Replica::journal(qint64 after, qint64 through) {
    auto lock = d->lock(); return lock ? d->journal().changes(after, through, true) : d->failed();
}
bool Replica::acceptJournal(const QString &peer, const QJsonObject &page) {
    auto lock = d->lock(); if (!lock) return false;
    if (peer != d->meta("host") || peer.isEmpty()) return d->fail("host_revision_required");
    if (!d->db.transaction()) return d->fail("journal_transaction_failed");
    auto ledger = d->journal();
    if (!ledger.accept(page) || !d->db.commit()) { d->db.rollback(); return d->fail(ledger.error.isEmpty() ? "namespace_commit_failed" : ledger.error); }
    return true;
}
QJsonObject Replica::changes(qint64 after, qint64 through) {
    auto lock = d->lock(); if (!lock) return d->failed();
    if (d->meta("host").isEmpty()) {
        auto result = d->journal().changes(after, through, false);
        result.insert("container", containerId()); result.insert("replica", replicaId()); return result;
    }
    const auto current = d->meta("sequence").toLongLong(); if (!through) through = current;
    if (after < 0 || after > through || through > current) return d->failed("invalid_change_cursor");
    QSqlQuery q(d->db); q.prepare("SELECT data,sequence FROM entries WHERE sequence>? AND sequence<=? ORDER BY sequence LIMIT 128");
    q.addBindValue(after); q.addBindValue(through); if (!q.exec()) return d->failed(q.lastError().text());
    QJsonArray entries; qint64 next = after, bytes = 0; int rows = 0; bool limited = false;
    while (q.next()) {
        auto e = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        const auto sequence = q.value(1).toLongLong(); e.insert("sequence", QString::number(sequence));
        ++rows;
        if (e.contains("revision")) { next = sequence; continue; }
        if (!e.contains("baseRevision")) e.insert("baseRevision", "");
        const auto length = json(e).size() + 1;
        if (bytes + length > 256 * 1024) { limited = true; break; }
        entries.append(e); next = sequence; bytes += length;
    }
    const bool more = limited || (rows == 128 && next < through);
    return {{"ok", true}, {"container", containerId()}, {"replica", replicaId()}, {"entries", entries},
        {"through", QString::number(through)}, {"next", QString::number(more ? next : through)}, {"more", more}};
}
QJsonObject Replica::handle(const QString &peer, const QJsonObject &request) {
    return handle(peer, request, true);
}
QJsonObject Replica::handle(const QString &peer, const QJsonObject &request, bool refreshIndex) {
    if (peer.isEmpty() || peer.size() > 256 || json(request).size() > 400000) return d->failed("invalid_peer_or_request");
    const auto action = request.value("action").toString();
    if (request.contains("container") && request.value("container").toString() != containerId()) return d->failed("remote_container_changed");
    if (action == "describe") {
        if (!d->ready()) return d->failed();
        if (!d->meta("host").isEmpty()) return d->failed("mirror_cannot_be_primary_host");
        return {{"ok", true}, {"protocol", 2}, {"namespaceVersion", 1}, {"namespace", namespaceState()}, {"manifestVersion", 1}, {"transferWindow", 4},
            {"container", containerId()}, {"replica", replicaId()}};
    }
    if (action == "journal") {
        qint64 after = 0, through = 0;
        if (!d->meta("host").isEmpty()) return d->failed("mirror_cannot_be_primary_host");
        if (!number(request.value("after"), &after) || !number(request.value("through"), &through)) return d->failed("invalid_namespace_cursor");
        return journal(after, through);
    }
    if (action == "changes") {
        qint64 after = 0, through = 0;
        if ((request.contains("after") && !number(request.value("after"), &after)) || (request.contains("through") && !number(request.value("through"), &through))) return d->failed("invalid_change_cursor");
        if (refreshIndex && !through && !scan()) return d->failed();
        return changes(after, through);
    }
    if (action == "manifest") {
        if (!d->ready()) return d->failed();
        qint64 offset, total, after, through;
        const auto id = request.value("manifest").toString(), replica = request.value("replica").toString();
        const auto entries = request.value("entries").toArray();
        if (!hex(id) || QUuid(replica).isNull() || replica == replicaId()
            || !number(request.value("offset"), &offset) || !number(request.value("total"), &total)
            || !number(request.value("after"), &after) || !number(request.value("through"), &through) || after > through
            || total > 250000 || offset > total || entries.size() > 128 || entries.size() > total - offset
            || !request.value("entries").isArray() || (entries.isEmpty() && total != 0)) return d->failed("invalid_manifest_page");
        if (offset == 0) {
            if (!d->manifests.contains(peer) && d->manifests.size() >= 32) return d->failed("manifest_capacity");
            auto state = std::make_shared<Private::Manifest>();
            state->id = id; state->replica = replica; state->total = total;
            state->after = state->sequence = after; state->through = through; d->manifests.insert(peer, state);
        }
        const auto state = d->manifests.value(peer);
        if (!state || state->complete || state->id != id || state->replica != replica || state->next != offset
            || state->total != total || state->after != after || state->through != through) return d->failed("manifest_page_out_of_order");
        for (const auto &value : entries) {
            const auto entry = value.toObject(); qint64 sequence;
            const QSet<QString> allowed{"path", "kind", "size", "hash", "clock", "version", "sequence", "baseRevision"};
            const auto keys = entry.keys();
            if (!validRecord(entry) || !number(entry.value("sequence"), &sequence) || sequence <= state->sequence || sequence > through
                || !QSet<QString>(keys.begin(), keys.end()).subtract(allowed).isEmpty()) {
                d->manifests.remove(peer); return d->failed("invalid_manifest_entry");
            }
            state->digest.addData(json(entry)); state->digest.addData(QByteArrayView("\n")); state->sequence = sequence;
        }
        state->next += entries.size();
        if (state->next == total) {
            if (QString::fromLatin1(state->digest.result().toHex()) != id) {
                d->manifests.remove(peer); return d->failed("manifest_hash_mismatch");
            }
            state->complete = true;
        }
        // Metadata never creates destination files, transfers, or journal entries.
        return {{"ok", true}, {"manifest", id}, {"next", QString::number(state->next)}, {"complete", state->complete}};
    }
    auto lock = d->lock(); if (!lock) return d->failed();
    if (request.contains("manifest")) {
        const auto state = d->manifests.value(peer);
        if (!state || !state->complete || state->id != request.value("manifest").toString()) return d->failed("manifest_required_before_transfer");
    }
    const auto e = request.value("entry").toObject(); if (!validRecord(e)) return d->failed("invalid_entry");
    const auto path = e.value("path").toString();
    if (action == "preview") {
        if (!d->meta("host").isEmpty() || !d->refresh(path)) return d->failed("preview_requires_authority");
        if (d->entry(path).value("version") != e.value("version")) return d->failed("file_changed_or_invalid_offset");
        QByteArray preview;
        detail::FileState state;
        if (!d->files.state(physical(path), &state)) return d->failed(d->files.error);
        const QStringList formats{"png", "jpg", "jpeg", "webp", "heic", "heif"};
        if (state.kind == "file" && state.size <= 32 * 1024 * 1024 && formats.contains(QFileInfo(path).suffix().toLower())) {
            QByteArray source;
            for (qint64 offset = 0; offset < state.size; offset += 1024 * 1024) {
                const auto block = d->files.read(physical(path), offset, qMin<qint64>(1024 * 1024, state.size - offset), state.stamp);
                if (!d->files.error.isEmpty()) return d->failed(d->files.error);
                source.append(block);
            }
            QBuffer input(&source); input.open(QIODevice::ReadOnly); QImageReader reader(&input);
            const auto size = reader.size();
            if (size.isValid() && qint64(size.width()) * size.height() <= 64000000) {
                reader.setScaledSize(size.scaled(256, 256, Qt::KeepAspectRatio)); reader.setAutoTransform(true);
                const auto image = reader.read(); QBuffer output(&preview); output.open(QIODevice::WriteOnly);
                if (!image.isNull()) image.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&output, "JPEG", 75);
            }
        }
        return {{"ok", true}, {"version", e.value("version")}, {"data", QString::fromLatin1(preview.toBase64())}};
    }
    if (action == "read") {
        if (!d->refresh(path)) return d->failed();
        const auto current = d->entry(path); qint64 offset;
        if (current.value("version") != e.value("version") || e.value("kind") != "file" || !number(request.value("offset"), &offset)) return d->failed("file_changed_or_invalid_offset");
        QString stamp; d->entry(path, &stamp);
        const auto bytes = d->files.read(physical(path), offset, ChunkBytes, stamp);
        if (!d->files.error.isEmpty()) return d->failed(d->files.error);
        return {{"ok", true}, {"offset", QString::number(offset)}, {"data", QString::fromLatin1(bytes.toBase64())}, {"version", e.value("version")}};
    }
    if (action == "begin" || action == "commit" || action == "apply") {
        if (action == "apply" && e.value("kind") == "file") return d->failed("file_requires_transfer");
        d->proposal = e.value("version").toString(); d->proposalBase = e.value("baseRevision").toString();
        const auto result = d->merge(peer, e, action != "begin"); d->proposal.clear(); d->proposalBase.clear(); return result;
    }
    if (action == "chunk") {
        qint64 offset; const auto encoded = request.value("data").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (e.value("kind") != "file" || !number(request.value("offset"), &offset) || !request.value("data").isString() || !decoded
            || decoded.decoded.size() > ChunkBytes || offset > e.value("size").toString().toLongLong() - decoded.decoded.size()) return d->failed("invalid_chunk");
        const auto part = d->transfer(peer, e); detail::FileState state;
        if (!d->files.state(part, &state) || state.kind != "file") return d->failed("begin_transfer_first");
        const auto free = QStorageInfo(d->files.root).bytesAvailable();
        if (free >= 0 && free < decoded.decoded.size() + 1024 * 1024) return d->failed("insufficient_storage");
        if (!d->files.append(part, offset, decoded.decoded)) return d->failed(d->files.error);
        return {{"ok", true}, {"offset", QString::number(offset + decoded.decoded.size())}};
    }
    return d->failed("unsupported_sync_action");
}
QJsonObject Replica::peerState(const QString &peer) const {
    QSqlQuery q(d->db); q.prepare("SELECT replica,container,pulled,pushed FROM peers WHERE peer=?"); q.addBindValue(peer);
    if (!q.exec()) { d->fail(q.lastError().text()); return {}; } if (!q.next()) return {};
    return {{"replica", q.value(0).toString()}, {"container", q.value(1).toString()}, {"pulled", QString::number(q.value(2).toLongLong())}, {"pushed", QString::number(q.value(3).toLongLong())}};
}
bool Replica::savePeerState(const QString &peer, const QString &replica, const QString &container, qint64 pulled, qint64 pushed) {
    auto lock = d->lock(); if (!lock) return false;
    if (peer.isEmpty() || peer.size() > 256 || QUuid(replica).isNull() || QUuid(container).isNull() || pulled < 0 || pushed < 0) return d->fail("invalid_peer_state");
    const auto before = peerState(peer);
    if (!before.isEmpty() && (before.value("replica").toString() != replica || before.value("container").toString() != container)) return d->fail("remote_container_changed");
    QSqlQuery q(d->db); q.prepare("INSERT INTO peers(peer,replica,container,pulled,pushed) VALUES(?,?,?,?,?) ON CONFLICT(peer) DO UPDATE SET pulled=MAX(pulled,excluded.pulled),pushed=MAX(pushed,excluded.pushed)");
    q.addBindValue(peer); q.addBindValue(replica); q.addBindValue(container); q.addBindValue(pulled); q.addBindValue(pushed);
    return q.exec() || d->fail(q.lastError().text());
}
}
