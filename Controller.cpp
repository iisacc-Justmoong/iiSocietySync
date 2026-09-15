#include "Controller.h"
#include "Synchronizer.h"
#include <SharedStorage.h>
#include <QDateTime>
#include <QDir>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <atomic>

namespace iiSocietySync {
class SyncWorker final : public QObject {
    Q_OBJECT
public:
    std::shared_ptr<std::atomic<quint64>> cancellation;
    std::shared_ptr<std::atomic<quint64>> inspection;
    quint64 generation = 0;
    std::unique_ptr<Replica> store;
    Synchronizer *sync = nullptr;
    QTimer *timer = nullptr;
    QTimer *changedFiles = nullptr;
    QFileSystemWatcher *watcher = nullptr;
    QString container;
    bool pending = false;
    QStringList hosts, queue;
    void initialize() {
        store = std::make_unique<Replica>(); sync = new Synchronizer(store.get(), this); timer = new QTimer(this);
        timer->setInterval(1000);
        watcher = new QFileSystemWatcher(this); changedFiles = new QTimer(this);
        changedFiles->setSingleShot(true); changedFiles->setInterval(75);
        const auto changed = [this] { pending = true; if (!changedFiles->isActive()) changedFiles->start(); };
        connect(watcher, &QFileSystemWatcher::directoryChanged, this, changed);
        connect(watcher, &QFileSystemWatcher::fileChanged, this, changed);
        connect(changedFiles, &QTimer::timeout, this, &SyncWorker::tick);
        connect(timer, &QTimer::timeout, this, &SyncWorker::tick);
        connect(sync, &Synchronizer::requestReady, this, [this](auto id, auto peer, auto payload) { emit request(generation, id, peer, payload); });
        connect(sync, &Synchronizer::progress, this, [this](auto path, auto done, auto total) { emit progress(generation, path, done, total); });
        connect(sync, &Synchronizer::mirrorChanged, this, [this](auto binding) { emit mirrorChanged(generation, binding); });
        connect(sync, &Synchronizer::finished, this, [this](const QString &peer, bool ok, const QString &error) {
            refreshWatches();
            emit status(generation, store->isOpen(), false, ok ? QString() : error);
            if (ok) emit synchronized(generation, peer);
            QTimer::singleShot(0, this, &SyncWorker::next);
        });
        timer->start();
    }
    void configure(QString path, QString scope, quint64 revision) {
        if (cancellation->load() != revision) return;
        if (!store) initialize();
        sync->stop(); generation = revision; queue.clear(); hosts.clear(); store->close();
        pending = false; changedFiles->stop(); container.clear();
        const auto paths = watcher->directories() + watcher->files(); if (!paths.isEmpty()) watcher->removePaths(paths);
        const auto shared = cancellation;
        store->setCancellation([shared, revision] { return shared->load() != revision; });
        if (scope.isEmpty()) { emit status(generation, false, false, {}); return; }
        QString error; const auto storage = iiSocietyContainer::SharedStorage::open(path, &error, true);
        const bool ok = storage && store->open(storage->drive().rootPath(), scope);
        if (ok) { container = storage->drive().rootPath(); refreshWatches(); }
        if (ok) emit mirrorChanged(generation, store->binding());
        emit status(generation, ok, false, ok ? QString() : storage ? store->errorString() : error);
    }
    void inspect(QString path, QString scope, quint64 revision) {
        if (inspection->load() != revision) return;
        if (path.isEmpty()) { emit inspected(revision, path, scope, {}, {}, {}); return; }
        const auto binding = Replica::binding(path);
        const auto drive = iiSocietyContainer::SocietyDrive::open(path);
        const auto primary = binding.isEmpty() ? Replica::primaryHost(path, scope) : binding.value("host").toString();
        if (inspection->load() == revision)
            emit inspected(revision, path, scope, binding, drive ? drive->identifier() : QString(), primary);
    }
    void setHosts(QStringList values, quint64 revision) {
        if (generation != revision || !store) return;
        values.removeDuplicates(); values.sort();
        if (hosts == values) return;
        sync->stop(); hosts = values; queue.clear(); tick();
    }
    void tick() {
        if (!store || !store->isOpen() || cancellation->load() != generation) return;
        if (sync->busy()) { pending = true; return; }
        pending = false;
        queue = hosts; next();
    }
    void next() {
        if (!store || !store->isOpen() || sync->busy() || cancellation->load() != generation) return;
        if (queue.isEmpty()) { if (pending && !changedFiles->isActive()) changedFiles->start(); return; }
        const auto peer = queue.takeFirst(); emit status(generation, true, true, {}); sync->start(peer);
    }
    void refreshWatches() {
        if (container.isEmpty()) return;
        QSet<QString> wanted;
        QStringList directories;
        for (const auto section : iiSocietyContainer::allStoreSections())
            directories.append(QDir(container).filePath(iiSocietyContainer::storeSectionName(section)));
        // Limit OS handles; the one-second reconciliation also covers overflow,
        // missed/coalesced events and files replaced by an atomic rename.
        while (!directories.isEmpty() && wanted.size() < 8192 && cancellation->load() == generation) {
            const auto path = directories.takeLast(); const QFileInfo directory(path);
            if (!directory.isDir() || directory.isSymLink()) continue;
            wanted.insert(path);
            const auto children = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
            for (const auto &entry : children) {
                if (entry.fileName().startsWith(".society-") || entry.fileName().startsWith(".iiserverhost-")) continue;
                if (entry.isDir()) directories.append(entry.absoluteFilePath());
                else if (entry.isFile()) wanted.insert(entry.absoluteFilePath());
                if (wanted.size() >= 8192) break;
            }
        }
        const auto paths = watcher->directories() + watcher->files(); const QSet<QString> existing(paths.begin(), paths.end());
        const auto removed = existing - wanted, added = wanted - existing;
        if (!removed.isEmpty()) watcher->removePaths(removed.values());
        if (!added.isEmpty()) watcher->addPaths(added.values());
    }
signals:
    void mirrorChanged(quint64 generation, QJsonObject binding);
    void request(quint64 generation, QString id, QString peer, QJsonObject payload);
    void result(quint64 generation, QString key, QJsonObject response);
    void status(quint64 generation, bool available, bool busy, QString error);
    void synchronized(quint64 generation, QString peer);
    void progress(quint64 generation, QString path, qint64 done, qint64 total);
    void inspected(quint64 revision, QString path, QString scope, QJsonObject binding, QString identifier, QString primary);
};
class Controller::Private {
public:
    struct Job { QJsonObject request, result; bool finished = false; qint64 created = 0; };
    Controller *q; RequestSender send; QThread *thread = new QThread; SyncWorker *worker;
    std::shared_ptr<std::atomic<quint64>> cancellation = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> inspection = std::make_shared<std::atomic<quint64>>(0);
    QMap<QString, Job> jobs; QHash<QString, QString> transportIds; QSet<QString> authorized;
    QString path, scope, error; bool ready = false, busy = false, opening = false, shutdown = false;
    Private(Controller *owner, RequestSender sender) : q(owner), send(std::move(sender)), worker(new SyncWorker) {
        worker->cancellation = cancellation; worker->inspection = inspection; worker->moveToThread(thread);
        thread->setObjectName("iiSocietySync-worker");
        QObject::connect(worker, &SyncWorker::mirrorChanged, q, [this](quint64 generation, const QJsonObject &binding) {
            if (generation == cancellation->load()) emit q->mirrorChanged(binding);
        });
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(worker, &SyncWorker::inspected, q,
            [this](quint64 revision, const QString &path, const QString &scope, const QJsonObject &binding, const QString &identifier, const QString &primary) {
                if (!shutdown && inspection->load() == revision)
                    emit q->containerInspected(path, scope, binding, identifier, primary);
            });
        QObject::connect(worker, &SyncWorker::status, q, [this](quint64 generation, bool available, bool active, const QString &message) {
            if (generation != cancellation->load()) return;
            ready = available; busy = active; opening = false; error = message; emit q->changed();
        });
        QObject::connect(worker, &SyncWorker::request, q, [this](quint64 generation, const QString &id, const QString &peer, const QJsonObject &payload) {
            if (generation != cancellation->load() || !authorized.contains(peer) || !send) return;
            const auto networkId = send(peer, payload);
            if (networkId.isEmpty()) {
                QMetaObject::invokeMethod(worker, [w = worker, id, generation] { if (w->generation == generation) w->sync->receive(id, {{"ok", false}, {"error", "transport_unavailable"}}); });
            } else transportIds.insert(networkId, id);
        });
        QObject::connect(worker, &SyncWorker::result, q, [this](quint64 generation, const QString &key, const QJsonObject &result) {
            if (generation != cancellation->load() || !jobs.contains(key)) return;
            jobs[key].result = result; jobs[key].finished = true;
        });
        QObject::connect(worker, &SyncWorker::synchronized, q, [this](quint64 generation, const QString &peer) { if (generation == cancellation->load()) emit q->synchronized(peer); });
        QObject::connect(worker, &SyncWorker::progress, q, [this](quint64 generation, const QString &path, qint64 done, qint64 total) { if (generation == cancellation->load()) emit q->progress(path, done, total); });
        thread->start();
    }
    void stopWorker() {
        ++*cancellation;
        ++*inspection;
        QObject::disconnect(worker, nullptr, q, nullptr);
        QMetaObject::invokeMethod(worker, [w = worker] { if (w->sync) w->sync->stop(); if (w->store) w->store->close(); QThread::currentThread()->quit(); });
    }
    ~Private() {
        if (shutdown) return; // The thread owns its remaining asynchronous cleanup.
        stopWorker(); thread->wait(); delete thread;
    }
};
Controller::Controller(RequestSender sender, QObject *parent) : QObject(parent), d(std::make_unique<Private>(this, std::move(sender))) {}
Controller::~Controller() { close(); }
bool Controller::available() const { return d->ready; }
bool Controller::busy() const { return d->busy; }
QString Controller::errorString() const { return d->error; }
void Controller::open(const QString &container, const QString &accountScope) {
    if (d->shutdown) return;
    if (d->path == container && d->scope == accountScope && !accountScope.isEmpty() && (d->ready || d->opening)) return;
    const auto revision = ++*d->cancellation;
    d->path = container; d->scope = accountScope; d->ready = false; d->busy = false; d->error.clear();
    d->opening = !accountScope.isEmpty();
    d->jobs.clear(); d->transportIds.clear(); d->authorized.clear();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, container, accountScope, revision] { w->configure(container, accountScope, revision); });
    emit changed();
}
void Controller::close() {
    if (d->scope.isEmpty() && d->path.isEmpty()) return;
    open({}, {});
}
void Controller::closeAndWait() {
    if (d->shutdown) return;
    close();
    // configure() and any cancelled filesystem operation precede this barrier
    // on the same worker queue. No old replica handle survives the return.
    QMetaObject::invokeMethod(d->worker, [] {}, Qt::BlockingQueuedConnection);
}
void Controller::inspectContainer(const QString &container, const QString &accountScope) {
    if (d->shutdown) return;
    const auto revision = ++*d->inspection;
    QMetaObject::invokeMethod(d->worker, [w = d->worker, container, accountScope, revision] { w->inspect(container, accountScope, revision); });
}
void Controller::shutdownAsync() {
    if (d->shutdown) return;
    d->shutdown = true;
    d->ready = false; d->busy = false; d->opening = false;
    d->jobs.clear(); d->transportIds.clear(); d->authorized.clear();
    QObject::connect(d->thread, &QThread::finished, d->thread, &QObject::deleteLater);
    d->stopWorker();
}
void Controller::setPeers(const QStringList &authorizedPeers, const QStringList &remoteHosts) {
    if (d->shutdown) return;
    d->authorized = QSet<QString>(authorizedPeers.begin(), authorizedPeers.end());
    QStringList hosts; for (const auto &host : remoteHosts) if (d->authorized.contains(host)) hosts.append(host);
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, hosts, revision] { w->setHosts(hosts, revision); });
}
void Controller::synchronizeNow() {
    if (d->shutdown) return;
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, revision] { if (w->generation == revision) w->tick(); });
}
QJsonObject Controller::handle(const QString &peer, const QJsonObject &envelope) {
    const auto fail = [](const QString &error) { return QJsonObject{{"ok", false}, {"error", error}}; };
    if (!d->authorized.contains(peer) || d->scope.isEmpty() || envelope.value("scope").toString() != d->scope) return fail("sync_peer_not_authorized");
    if (!d->ready) return fail("sync_container_unavailable");
    const auto token = envelope.value("token").toString();
    if (envelope.value("op") != "society.sync" || envelope.value("protocol").toInt() != 2 || QUuid(token).isNull()
        || !envelope.value("message").isObject() || QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() > 410000) return fail("invalid_sync_envelope");
    const auto messageValue = envelope.value("message").toObject();
    if (messageValue.value("action") != "describe" && QUuid(messageValue.value("container").toString()).isNull())
        return fail("logical_container_required");
    const auto key = peer + '/' + token;
    if (auto found = d->jobs.find(key); found != d->jobs.end()) {
        if (found->request != envelope) return fail("different_request_token_reuse");
        return found->finished ? QJsonObject{{"ok", true}, {"result", found->result}} : QJsonObject{{"ok", true}, {"pending", true}};
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = d->jobs.begin(); it != d->jobs.end();) {
        if (it->finished && now - it->created > 60000) it = d->jobs.erase(it); else ++it;
    }
    while (d->jobs.size() >= 32) {
        auto oldest = d->jobs.end();
        for (auto it = d->jobs.begin(); it != d->jobs.end(); ++it)
            if (it->finished && (oldest == d->jobs.end() || it->created < oldest->created)) oldest = it;
        if (oldest == d->jobs.end()) return fail("sync_endpoint_busy");
        d->jobs.erase(oldest);
    }
    d->jobs.insert(key, {envelope, {}, false, now});
    const auto revision = d->cancellation->load(); const auto message = envelope.value("message").toObject();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, revision, key, peer, message] {
        if (w->generation != revision || w->cancellation->load() != revision) return;
        const auto result = w->store->binding().isEmpty() ? w->store->handle(peer, message)
            : QJsonObject{{"ok", false}, {"error", "mirror_cannot_be_primary_host"}};
        emit w->result(revision, key, result);
    });
    return {{"ok", true}, {"pending", true}};
}
void Controller::receive(const QString &transportRequestId, const QJsonObject &response) {
    if (d->shutdown) return;
    const auto id = d->transportIds.take(transportRequestId); if (id.isEmpty()) return;
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, revision, id, response] { if (w->generation == revision) w->sync->receive(id, response); });
}
}
#include "Controller.moc"
