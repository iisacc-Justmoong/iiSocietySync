#include "Replica.h"
#include "ConfinedFiles.h"
#include <SocietyDrive.h>
#include <QCryptographicHash>
#include <QDir>
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
    detail::ConfinedFiles files;
    std::optional<iiSocietyContainer::SocietyDrive> drive;
    QSqlDatabase db; QString connection, scope, id; mutable QString error;
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
    std::unique_ptr<QLockFile> lock() {
        if (!ready()) return {};
        auto lock = std::make_unique<QLockFile>(QDir(files.root).filePath(".society-sync/operation.lock"));
        lock->setStaleLockTime(0);
        if (!lock->tryLock()) { fail("sync_store_busy"); return {}; }
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
        const bool ok = sql("DELETE FROM entries") && sql("DELETE FROM peers") && sql("DELETE FROM detach")
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
        e = versioned(e); const auto previous = entry(e.value("path").toString());
        qint64 seq = previous.value("sequence").toString().toLongLong();
        if (previous.value("version") != e.value("version")) {
            seq = meta("sequence").toLongLong() + 1;
            if (seq > MaxCounter || !setMeta("sequence", QString::number(seq))) return fail("journal_sequence_limit");
        }
        QSqlQuery q(db); q.prepare("INSERT INTO entries(path,data,stamp,sequence) VALUES(?,?,?,?) ON CONFLICT(path) DO UPDATE SET data=excluded.data,stamp=excluded.stamp,sequence=excluded.sequence");
        q.addBindValue(e.value("path").toString()); q.addBindValue(json(e)); q.addBindValue(stamp.isNull() ? QString("") : stamp); q.addBindValue(seq);
        return q.exec() || fail(q.lastError().text());
    }
    bool observed(const QString &path, const detail::FileState &state) {
        QString oldStamp; const auto old = entry(path, &oldStamp);
        if (!error.isEmpty()) return false;
        if (old.isEmpty() && state.kind == "deleted") return true;
        if (old.value("kind").toString() == state.kind && oldStamp == state.stamp) return true;
        const auto hash = state.kind == "file" ? files.hash(physical(path), state.stamp) : QString("");
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
        auto local = entry(path); const auto a = incoming.value("clock").toObject(), b = local.value("clock").toObject();
        const auto relation = local.isEmpty() ? 1 : compare(a, b);
        if (relation == 0 && !contentEqual(incoming, local)) return failed("inconsistent_version_clock");
        if (relation <= 0) return result();
        const auto clock = joined(a, b); if (clock.size() > 64) return failed("replica_clock_limit");
        const bool same = contentEqual(incoming, local);
        // A nonempty local directory must never be replaced by a file/deletion.
        bool retainDirectory = false;
        if (local.value("kind") == "directory" && incoming.value("kind") != "directory") {
            bool ok; retainDirectory = !files.list(physical(path), &ok).isEmpty();
            if (!ok) return failed(files.error);
        }
        const bool concurrent = relation == 2 || retainDirectory;
        const bool incomingWins = !retainDirectory && (relation == 1 || same || rank(incoming) > rank(local)
            || (rank(incoming) == rank(local) && incoming.value("hash").toString() > local.value("hash").toString()));
        auto winner = incomingWins ? incoming : local;
        const auto loser = incomingWins ? local : incoming;
        const bool conflict = concurrent && !same && loser.value("kind") == "file";
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
void Replica::close() { d->drive.reset(); d->db.close(); d->db = {}; if (!d->connection.isEmpty()) QSqlDatabase::removeDatabase(d->connection); d->connection.clear(); d->scope.clear(); d->id.clear(); }
QString Replica::errorString() const { return d->error; }
bool Replica::isOpen() const { return d->db.isOpen() && !d->scope.isEmpty() && !d->id.isEmpty(); }
QString Replica::containerId() const { return d->drive ? d->drive->identifier() : QString(); }
QString Replica::replicaId() const { return d->id; }
QString Replica::accountScope() const { return d->scope; }
void Replica::setCancellation(std::function<bool()> cancelled) { d->files.cancelled = std::move(cancelled); }
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
    if (!lock.tryLock()) return d->fail("sync_store_busy");
    d->connection = "society-sync-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->db = QSqlDatabase::addDatabase("QSQLITE", d->connection); d->db.setDatabaseName(QDir(state).filePath("journal.sqlite"));
    d->db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    if (!d->db.open()) return d->fail(d->db.lastError().text());
    if (!QFile::setPermissions(d->db.databaseName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return d->fail("state_permissions_failed");
    int schema = -1;
    { QSqlQuery q(d->db); if (q.exec("PRAGMA user_version") && q.next()) schema = q.value(0).toInt(); }
    if (schema != 0 && schema != 1 && schema != 2) return d->fail("unsupported_journal_schema");
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
    if (d->meta("scope") != accountScope
        || (d->meta("container") != containerId()
            && !(d->meta("adoption") == "detaching" && d->meta("target") == containerId()))) {
        close(); return d->fail("container_account_binding_mismatch");
    }
    d->scope = accountScope;
    if (!d->resumeAdoption()) return false;
    d->scope = accountScope; d->id = d->meta("replica");
    if (QUuid(d->id).isNull()) { close(); return d->fail("invalid_replica_identity"); }
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
    if (peer.isEmpty() || peer.size() > 256 || QUuid(replica).isNull() || replica == replicaId()
        || QUuid(container).isNull() || QUuid(container).toString(QUuid::WithoutBraces) != container) return d->fail("invalid_host_identity");
    if (!d->meta("host").isEmpty() && d->meta("host") != peer) return d->fail("different_primary_host");
    if (!d->resumeAdoption()) return false;
    if (d->meta("host") == peer && d->meta("target") == container) {
        if (d->meta("hostReplica") != replica) {
            // The same logical host restored its journal. Start its cursors from
            // zero, retaining this replica's durable clocks and offline edits.
            QSqlQuery q(d->db); q.prepare("DELETE FROM peers WHERE peer=?"); q.addBindValue(peer);
            if (!q.exec() || !d->setMeta("hostReplica", replica)) return d->fail("host_checkpoint_reset_failed");
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
            if (++count > 250000) { ok = false; break; }
            QSqlQuery q(d->db); q.prepare("INSERT INTO detach(path,target) VALUES(?,?)");
            q.addBindValue(native + '/' + name); q.addBindValue(recovery + '/' + native + '/' + name);
            if (!q.exec()) { ok = false; break; }
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
    const auto clock = e.value("clock").toObject(); if (clock.isEmpty() || clock.size() > 64) return false;
    for (auto i = clock.begin(); i != clock.end(); ++i) { qint64 n; if (QUuid(i.key()).isNull() || !number(i.value(), &n) || !n) return false; }
    return json(e).size() <= 16384;
}
bool Replica::scan() {
    auto lock = d->lock(); if (!lock) return false;
    if (!d->db.transaction()) return d->fail("journal_transaction_failed");
    QSet<QString> seen; bool ok = true;
    std::function<void(QString, QString)> visit = [&](const QString &native, const QString &key) {
        bool read; const auto names = d->files.list(native, &read); if (!read) { ok = d->fail(d->files.error); return; }
        for (const auto &name : names) {
            if (name.startsWith(".society-") || name.startsWith(".iiserverhost-")) continue;
            const auto path = key + '/' + name; detail::FileState state;
            if (physical(path).isEmpty() || seen.size() >= 250000 || !d->files.state(native + '/' + name, &state)) { ok = d->fail("unsupported_or_unavailable_entry"); return; }
            if (state.kind == "deleted") { ok = d->fail("directory_changed_during_scan"); return; }
            seen.insert(path); if (!d->observed(path, state)) { ok = false; return; }
            if (state.kind == "directory") visit(native + '/' + name, path);
            if (!ok) return;
        }
    };
    for (const auto section : iiSocietyContainer::allStoreSections()) {
        visit(iiSocietyContainer::storeSectionName(section), iiSocietyContainer::storeSectionKey(section)); if (!ok) break;
    }
    if (ok) {
        QStringList absent; QSqlQuery q(d->db);
        if (!q.exec("SELECT path FROM entries")) ok = d->fail(q.lastError().text());
        while (ok && q.next()) if (!seen.contains(q.value(0).toString())) absent.append(q.value(0).toString());
        q.finish();
        for (const auto &path : absent) if (d->entry(path).value("kind") != "deleted" && !d->observed(path, {"deleted", {}, 0})) { ok = false; break; }
    }
    if (!ok) { d->db.rollback(); return false; }
    return d->db.commit() || d->fail(d->db.lastError().text());
}
QJsonObject Replica::record(const QString &path) const { return d->db.isOpen() ? d->entry(path) : QJsonObject(); }
QJsonObject Replica::changes(qint64 after, qint64 through) {
    auto lock = d->lock(); if (!lock) return d->failed();
    const auto current = d->meta("sequence").toLongLong(); if (!through) through = current;
    if (after < 0 || after > through || through > current) return d->failed("invalid_change_cursor");
    QSqlQuery q(d->db); q.prepare("SELECT data,sequence FROM entries WHERE sequence>? AND sequence<=? ORDER BY sequence LIMIT 128");
    q.addBindValue(after); q.addBindValue(through); if (!q.exec()) return d->failed(q.lastError().text());
    QJsonArray entries; qint64 next = after, bytes = 0; bool limited = false;
    while (q.next()) {
        auto e = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
        const auto sequence = q.value(1).toLongLong(); e.insert("sequence", QString::number(sequence));
        const auto length = json(e).size() + 1;
        if (bytes + length > 256 * 1024) { limited = true; break; }
        entries.append(e); next = sequence; bytes += length;
    }
    const bool more = limited || (entries.size() == 128 && next < through);
    return {{"ok", true}, {"container", containerId()}, {"replica", replicaId()}, {"entries", entries},
        {"through", QString::number(through)}, {"next", QString::number(more ? next : through)}, {"more", more}};
}
QJsonObject Replica::handle(const QString &peer, const QJsonObject &request) {
    if (peer.isEmpty() || peer.size() > 256 || json(request).size() > 400000) return d->failed("invalid_peer_or_request");
    const auto action = request.value("action").toString();
    if (request.contains("container") && request.value("container").toString() != containerId()) return d->failed("remote_container_changed");
    if (action == "describe") {
        if (!d->ready()) return d->failed();
        if (!d->meta("host").isEmpty()) return d->failed("mirror_cannot_be_primary_host");
        return {{"ok", true}, {"protocol", 2}, {"container", containerId()}, {"replica", replicaId()}};
    }
    if (action == "changes") {
        qint64 after = 0, through = 0;
        if ((request.contains("after") && !number(request.value("after"), &after)) || (request.contains("through") && !number(request.value("through"), &through))) return d->failed("invalid_change_cursor");
        if (!through && !scan()) return d->failed();
        return changes(after, through);
    }
    auto lock = d->lock(); if (!lock) return d->failed();
    const auto e = request.value("entry").toObject(); if (!validRecord(e)) return d->failed("invalid_entry");
    const auto path = e.value("path").toString();
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
        return d->merge(peer, e, action != "begin");
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
