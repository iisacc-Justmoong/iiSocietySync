#include "Controller.h"
#include <QElapsedTimer>
#include <QDebug>
#include "Synchronizer.h"
#include "ObjectProvider.h"
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
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace iiSocietySync {
namespace {
qsizetype watchPathLimit() {
    qsizetype limit = 8192;
#ifdef Q_OS_UNIX
    struct rlimit descriptors{};
    if (getrlimit(RLIMIT_NOFILE, &descriptors) == 0 && descriptors.rlim_cur != RLIM_INFINITY)
        limit = qsizetype(qMin<rlim_t>(8192, descriptors.rlim_cur / 8));
#endif
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    limit = qMin<qsizetype>(limit, 128);
#endif
    return limit;
}
}
class ProviderWorker final : public QObject {
    Q_OBJECT
signals:
    void finished(bool success, QString error);
};
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
    QString primaryClaim;
    bool pending = false;
    Synchronizer::ContentPolicy policy = Synchronizer::ContentPolicy::MetadataFirst;
    QElapsedTimer verificationTimer;
    QStringList hosts, queue;
    QJsonObject namespaceSnapshot;
    QThread *indexThread = nullptr;
    std::shared_ptr<std::atomic<bool>> indexCancelled;
    bool indexDirty = true;
    QElapsedTimer indexRefresh;
    ~SyncWorker() override { stopIndexing(); }
    void stopIndexing() {
        if (!indexThread) return;
        indexCancelled->store(true);
        auto task = indexThread; indexThread = nullptr;
        task->wait(); delete task; indexCancelled.reset();
    }
    void indexHost() {
        if (indexThread || !store || !store->isOpen()) return;
        if (!indexDirty && indexRefresh.isValid() && indexRefresh.elapsed() < 30000) return;
        indexDirty = false; indexRefresh.start();
        const auto revision = generation;
        const auto root = container, account = store->accountScope();
        const auto cancelled = cancellation;
        const auto stopped = indexCancelled = std::make_shared<std::atomic<bool>>(false);
        struct Result { bool ok = false; QString error; };
        const auto result = std::make_shared<Result>();
        auto task = QThread::create([this, revision, root, account, cancelled, stopped, result] {
            Replica indexer;
            indexer.setCancellation([=] { return stopped->load() || cancelled->load() != revision; });
            QElapsedTimer progressTimer;
            indexer.setVerificationProgress([this, revision, &progressTimer](const QString &path, qint64 done, qint64 total) {
                if (!done || done == total || !progressTimer.isValid() || progressTimer.elapsed() >= 100) {
                    progressTimer.start(); emit verificationProgress(revision, path, done, total);
                }
            });
            result->ok = indexer.open(root, account) && indexer.binding().isEmpty() && indexer.scan();
            result->error = indexer.errorString();
        });
        indexThread = task; task->setParent(this); task->setObjectName("iiSocietySync-indexer");
        connect(task, &QThread::finished, this, [this, task, revision, result] {
            if (indexThread != task || generation != revision) return;
            indexThread = nullptr; indexCancelled.reset(); task->deleteLater();
            if (cancellation->load() != revision || !store || !store->isOpen()) return;
            const bool ok = result->ok && store->publishStorageMap();
            if (!ok) indexDirty = true;
            refreshWatches(); publishNamespace();
            const auto error = result->ok ? store->errorString() : result->error;
            emit status(generation, true, false, ok ? QString() : error);
        });
        task->start();
    }
    bool pinPrimary() {
        if (primaryClaim.isEmpty()) return true;
        if (!store || !store->isOpen() || !Replica::claimPrimaryHost(container, store->accountScope(), primaryClaim)) return false;
        primaryClaim.clear(); return true;
    }
    void publishNamespace() {
        const auto state = store->namespaceState();
        if (state != namespaceSnapshot) { namespaceSnapshot = state; emit namespaceChanged(generation, state); }
    }
    void initialize() {
        store = std::make_unique<Replica>(); sync = new Synchronizer(store.get(), this); timer = new QTimer(this);
        if (qEnvironmentVariableIntValue("SOCIETY_SYNC_TRACE") == 1)
            qInfo("Society sync: watcher budget=%lld", qlonglong(watchPathLimit()));
        sync->setContentPolicy(policy);
        store->setVerificationProgress([this](const QString &path, qint64 done, qint64 total) {
            if (!done || done == total || !verificationTimer.isValid() || verificationTimer.elapsed() >= 100) {
                verificationTimer.start();
                emit verificationProgress(generation, path, done, total);
            }
        });
        timer->setInterval(1000);
        watcher = new QFileSystemWatcher(this); changedFiles = new QTimer(this);
        changedFiles->setSingleShot(true); changedFiles->setInterval(75);
        const auto changed = [this] { indexDirty = true; pending = true; if (!changedFiles->isActive()) changedFiles->start(); };
        connect(watcher, &QFileSystemWatcher::directoryChanged, this, changed);
        connect(watcher, &QFileSystemWatcher::fileChanged, this, changed);
        connect(changedFiles, &QTimer::timeout, this, &SyncWorker::tick);
        connect(timer, &QTimer::timeout, this, &SyncWorker::tick);
        connect(sync, &Synchronizer::requestReady, this, [this](auto id, auto peer, auto payload) { emit request(generation, id, peer, payload); });
        connect(sync, &Synchronizer::progress, this, [this](auto path, auto done, auto total) { emit progress(generation, path, done, total); });
        connect(sync, &Synchronizer::mirrorChanged, this, [this](auto binding) {
            stopIndexing(); emit mirrorChanged(generation, binding);
        });
        connect(sync, &Synchronizer::finished, this, [this](const QString &peer, bool ok, const QString &error) {
            refreshWatches();
            publishNamespace();
            emit status(generation, store->isOpen(), false, ok ? QString() : error);
            if (ok) emit synchronized(generation, peer);
            QTimer::singleShot(0, this, &SyncWorker::next);
        });
        timer->start();
    }
    void configure(QString path, QString scope, quint64 revision) {
        if (cancellation->load() != revision) return;
        if (!store) initialize();
        stopIndexing();
        sync->setExpectedHost({}, {}); expectedHost.clear(); expectedContainer.clear();
        sync->stop(); generation = revision; queue.clear(); hosts.clear(); store->close();
        pending = false; changedFiles->stop(); container.clear(); primaryClaim.clear();
        indexDirty = true; indexRefresh.invalidate();
        namespaceSnapshot = {};
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
    QString expectedHost, expectedContainer;
    void setHosts(QStringList values, quint64 revision, QString accountHost, QString accountContainer) {
        if (generation != revision || !store) return;
        values.removeDuplicates(); values.sort();
        if (hosts == values && expectedHost == accountHost && expectedContainer == accountContainer) return;
        expectedHost = accountHost; expectedContainer = accountContainer;
        sync->setExpectedHost(accountHost, accountContainer);
        sync->stop(); hosts = values; queue.clear(); tick();
    }
    void tick() {
        if (!store || !store->isOpen() || cancellation->load() != generation) return;
        if (!pinPrimary()) { emit status(generation, true, false, "primary_host_unavailable"); return; }
        if (sync->busy()) { pending = true; return; }
        pending = false;
        if (hosts.isEmpty() && !store->bootstrapping()) {
            // The host commits local changes even with every network offline.
            // Mirrors only stage proposals here and never become authorities.
            // The authority's long model reads have their own connection/thread.
            // Replication can serve each committed batch while hashing continues.
            bool ok;
            if (store->binding().isEmpty()) {
                ok = store->publishStorageMap();
                if (ok) indexHost();
            } else {
                ok = store->scan() && store->publishStorageMap(); refreshWatches();
            }
            publishNamespace();
            emit status(generation, true, false, ok ? QString() : store->errorString());
        }
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
        // Apple kqueue consumes file descriptors for watched paths. Preserve
        // room for TLS, SQLite and payloads even when the library is very large.
        // Periodic full indexing covers paths outside this bounded watch set.
        const auto limit = watchPathLimit();
        while (!directories.isEmpty() && wanted.size() < limit && cancellation->load() == generation) {
            const auto path = directories.takeLast(); const QFileInfo directory(path);
            if (!directory.isDir() || directory.isSymLink()) continue;
            wanted.insert(path);
            if (wanted.size() >= limit) break;
            const auto children = QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
            for (const auto &entry : children) {
                if (entry.fileName().startsWith(".society-") || entry.fileName().startsWith(".iiserverhost-")) continue;
                if (entry.isDir()) directories.append(entry.absoluteFilePath());
                else if (entry.isFile()) wanted.insert(entry.absoluteFilePath());
                if (wanted.size() >= limit) break;
            }
        }
        const auto paths = watcher->directories() + watcher->files(); const QSet<QString> existing(paths.begin(), paths.end());
        const auto removed = existing - wanted, added = wanted - existing;
        if (!removed.isEmpty()) watcher->removePaths(removed.values());
        if (!added.isEmpty()) watcher->addPaths(added.values());
    }
signals:
    void namespaceChanged(quint64 generation, QJsonObject state);
    void mirrorChanged(quint64 generation, QJsonObject binding);
    void request(quint64 generation, QString id, QString peer, QJsonObject payload);
    void result(quint64 generation, QString key, QJsonObject response);
    void status(quint64 generation, bool available, bool busy, QString error);
    void synchronized(quint64 generation, QString peer);
    void progress(quint64 generation, QString path, qint64 done, qint64 total);
    void verificationProgress(quint64 generation, QString path, qint64 done, qint64 total);
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
    QSet<QThread *> providerThreads;
    void providerTask(const QString &objectPath, std::shared_ptr<ObjectProvider> provider, bool restore) {
        if (shutdown || !provider) return;
        if (!ready || providerThreads.size() >= 2) {
            emit q->providerFinished(objectPath, provider->id(), restore, false, !ready ? "sync_container_unavailable" : "provider_busy"); return;
        }
        auto task = new QThread; auto job = new ProviderWorker;
        job->moveToThread(task); providerThreads.insert(task);
        const auto revision = cancellation->load(); const auto cancelled = cancellation;
        QObject::connect(task, &QThread::started, job, [job, root = path, account = scope, objectPath, provider, restore, revision, cancelled] {
            // Independent connection on its owning thread. A slow NAS/S3 job
            // must not stop the authority's scanner or node replication worker.
            Replica store; store.setCancellation([cancelled, revision] { return cancelled->load() != revision; });
            const bool ok = cancelled->load() == revision && store.open(root, account)
                && (restore ? store.restoreObject(objectPath, *provider) : store.placeObject(objectPath, *provider));
            const auto error = store.errorString(); store.close();
            emit job->finished(ok, error); QThread::currentThread()->quit();
        });
        QObject::connect(job, &ProviderWorker::finished, q, [this, revision, objectPath, provider, restore](bool ok, const QString &error) {
            if (revision == cancellation->load()) emit q->providerFinished(objectPath, provider->id(), restore, ok, error);
        });
        QObject::connect(task, &QThread::finished, job, &QObject::deleteLater);
        QObject::connect(task, &QThread::finished, q, [this, task] { providerThreads.remove(task); });
        QObject::connect(task, &QThread::finished, task, &QObject::deleteLater);
        task->start();
    }
    Private(Controller *owner, RequestSender sender) : q(owner), send(std::move(sender)), worker(new SyncWorker) {
        worker->cancellation = cancellation; worker->inspection = inspection; worker->moveToThread(thread);
        thread->setObjectName("iiSocietySync-worker");
        QObject::connect(worker, &SyncWorker::namespaceChanged, q, [this](quint64 generation, QJsonObject state) {
            if (generation == cancellation->load()) emit q->namespaceChanged(state);
        });
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
            if (!opening && ready == available && busy == active && error == message) return;
            if (!message.isEmpty() && qEnvironmentVariableIntValue("SOCIETY_SYNC_TRACE") == 1)
                qWarning().noquote() << "Society sync error:" << message;
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
        QObject::connect(worker, &SyncWorker::verificationProgress, q, [this](quint64 generation, const QString &path, qint64 done, qint64 total) {
            if (generation == cancellation->load()) emit q->verificationProgress(path, done, total);
        });
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
        for (auto task : std::as_const(providerThreads)) {
            QObject::disconnect(task, nullptr, q, nullptr); task->wait();
        }
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
    for (auto task : std::as_const(d->providerThreads)) task->wait();
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
void Controller::setPeers(const QStringList &authorizedPeers, const QStringList &remoteHosts, const QString &accountHost, const QString &accountContainer) {
    if (d->shutdown) return;
    d->authorized = QSet<QString>(authorizedPeers.begin(), authorizedPeers.end());
    QStringList hosts; for (const auto &host : remoteHosts) if (d->authorized.contains(host)) hosts.append(host);
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, hosts, revision, accountHost, accountContainer] { w->setHosts(hosts, revision, accountHost, accountContainer); });
}
void Controller::synchronizeNow() {
    if (d->shutdown) return;
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, revision] {
        if (w->generation == revision) { w->indexDirty = true; w->tick(); }
    });
}
void Controller::setContentPolicy(Synchronizer::ContentPolicy policy) {
    if (d->shutdown) return;
    QMetaObject::invokeMethod(d->worker, [w = d->worker, policy] {
        w->policy = policy;
        if (w->sync) { w->sync->stop(); w->sync->setContentPolicy(policy); w->tick(); }
    });
}
void Controller::claimPrimaryHost(const QString &device) {
    if (d->shutdown || device.isEmpty()) return;
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, device, revision] {
        if (w->generation != revision || w->cancellation->load() != revision) return;
        w->primaryClaim = device;
        if (!w->pinPrimary()) emit w->status(revision, w->store && w->store->isOpen(), false, "primary_host_unavailable");
    });
}
void Controller::placeObject(const QString &path, std::shared_ptr<ObjectProvider> provider) {
    d->providerTask(path, std::move(provider), false);
}
void Controller::restoreObject(const QString &path, std::shared_ptr<ObjectProvider> provider) {
    d->providerTask(path, std::move(provider), true);
}
QJsonObject Controller::handle(const QString &peer, const QJsonObject &envelope) {
    const auto fail = [](const QString &error) { return QJsonObject{{"ok", false}, {"error", error}}; };
    if (!d->authorized.contains(peer) || d->scope.isEmpty() || envelope.value("scope").toString() != d->scope) return fail("sync_peer_not_authorized");
    if (!d->ready) return fail("sync_container_unavailable");
    if (envelope.value("namespaceVersion") != 1) return fail("namespace_authority_protocol_required");
    const auto token = envelope.value("token").toString();
    if (envelope.value("op") != "society.sync" || envelope.value("protocol").toInt() != 2 || QUuid(token).isNull()
        || !envelope.value("message").isObject() || QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() > 410000) return fail("invalid_sync_envelope");
    const auto messageValue = envelope.value("message").toObject();
    const auto action = messageValue.value("action").toString();
    if (action == "begin" || action == "chunk" || action == "commit" || action == "apply") {
        const auto entry = messageValue.value("entry").toObject();
        if (!entry.contains("baseRevision") || entry.contains("revision") || !Replica::validRecord(entry))
            return fail("namespace_proposal_required");
    }
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
        const auto result = !w->pinPrimary() ? QJsonObject{{"ok", false}, {"error", "primary_host_unavailable"}}
            : w->store->binding().isEmpty() ? w->store->handle(peer, message, false)
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
