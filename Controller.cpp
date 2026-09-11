#include "Controller.h"
#include "Synchronizer.h"
#include <SharedStorage.h>
#include <QDateTime>
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
    quint64 generation = 0;
    std::unique_ptr<Replica> store;
    Synchronizer *sync = nullptr;
    QTimer *timer = nullptr;
    QStringList hosts, queue;
    void initialize() {
        store = std::make_unique<Replica>(); sync = new Synchronizer(store.get(), this); timer = new QTimer(this);
        timer->setInterval(5000);
        connect(timer, &QTimer::timeout, this, &SyncWorker::tick);
        connect(sync, &Synchronizer::requestReady, this, [this](auto id, auto peer, auto payload) { emit request(generation, id, peer, payload); });
        connect(sync, &Synchronizer::progress, this, [this](auto path, auto done, auto total) { emit progress(generation, path, done, total); });
        connect(sync, &Synchronizer::mirrorChanged, this, [this](auto binding) { emit mirrorChanged(generation, binding); });
        connect(sync, &Synchronizer::finished, this, [this](const QString &peer, bool ok, const QString &error) {
            emit status(generation, store->isOpen(), false, ok ? QString() : error);
            if (ok) emit synchronized(generation, peer);
            QTimer::singleShot(0, this, &SyncWorker::next);
        });
        timer->start();
    }
    void configure(QString path, QString scope, quint64 revision) {
        if (!store) initialize();
        sync->stop(); generation = revision; queue.clear(); hosts.clear(); store->close();
        const auto shared = cancellation;
        store->setCancellation([shared, revision] { return shared->load() != revision; });
        if (scope.isEmpty()) { emit status(generation, false, false, {}); return; }
        QString error; const auto storage = iiSocietyContainer::SharedStorage::open(path, &error, true);
        const bool ok = storage && store->open(storage->drive().rootPath(), scope);
        if (ok) emit mirrorChanged(generation, store->binding());
        emit status(generation, ok, false, ok ? QString() : storage ? store->errorString() : error);
    }
    void setHosts(QStringList values, quint64 revision) {
        if (generation != revision || !store) return;
        values.removeDuplicates(); values.sort();
        if (hosts == values) return;
        sync->stop(); hosts = values; queue.clear(); tick();
    }
    void tick() {
        if (!store || !store->isOpen() || sync->busy() || cancellation->load() != generation) return;
        queue = hosts; next();
    }
    void next() {
        if (!store || !store->isOpen() || sync->busy() || queue.isEmpty() || cancellation->load() != generation) return;
        const auto peer = queue.takeFirst(); emit status(generation, true, true, {}); sync->start(peer);
    }
signals:
    void mirrorChanged(quint64 generation, QJsonObject binding);
    void request(quint64 generation, QString id, QString peer, QJsonObject payload);
    void result(quint64 generation, QString key, QJsonObject response);
    void status(quint64 generation, bool available, bool busy, QString error);
    void synchronized(quint64 generation, QString peer);
    void progress(quint64 generation, QString path, qint64 done, qint64 total);
};
class Controller::Private {
public:
    struct Job { QJsonObject request, result; bool finished = false; qint64 created = 0; };
    Controller *q; RequestSender send; QThread thread; SyncWorker *worker;
    std::shared_ptr<std::atomic<quint64>> cancellation = std::make_shared<std::atomic<quint64>>(0);
    QMap<QString, Job> jobs; QHash<QString, QString> transportIds; QSet<QString> authorized;
    QString path, scope, error; bool ready = false, busy = false;
    Private(Controller *owner, RequestSender sender) : q(owner), send(std::move(sender)), worker(new SyncWorker) {
        worker->cancellation = cancellation; worker->moveToThread(&thread);
        QObject::connect(worker, &SyncWorker::mirrorChanged, q, [this](quint64 generation, const QJsonObject &binding) {
            if (generation == cancellation->load()) emit q->mirrorChanged(binding);
        });
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(worker, &SyncWorker::status, q, [this](quint64 generation, bool available, bool active, const QString &message) {
            if (generation != cancellation->load()) return;
            ready = available; busy = active; error = message; emit q->changed();
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
        thread.start();
    }
    ~Private() {
        ++*cancellation;
        QMetaObject::invokeMethod(worker, [w = worker] { if (w->sync) w->sync->stop(); if (w->store) w->store->close(); QThread::currentThread()->quit(); });
        thread.wait();
    }
};
Controller::Controller(RequestSender sender, QObject *parent) : QObject(parent), d(std::make_unique<Private>(this, std::move(sender))) {}
Controller::~Controller() { close(); }
bool Controller::available() const { return d->ready; }
bool Controller::busy() const { return d->busy; }
QString Controller::errorString() const { return d->error; }
void Controller::open(const QString &container, const QString &accountScope) {
    if (d->path == container && d->scope == accountScope && !accountScope.isEmpty()) return;
    const auto revision = ++*d->cancellation;
    d->path = container; d->scope = accountScope; d->ready = false; d->busy = false; d->error.clear();
    d->jobs.clear(); d->transportIds.clear(); d->authorized.clear();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, container, accountScope, revision] { w->configure(container, accountScope, revision); });
    emit changed();
}
void Controller::close() {
    if (d->scope.isEmpty() && d->path.isEmpty()) return;
    open({}, {});
}
void Controller::setPeers(const QStringList &authorizedPeers, const QStringList &remoteHosts) {
    d->authorized = QSet<QString>(authorizedPeers.begin(), authorizedPeers.end());
    QStringList hosts; for (const auto &host : remoteHosts) if (d->authorized.contains(host)) hosts.append(host);
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, hosts, revision] { w->setHosts(hosts, revision); });
}
void Controller::synchronizeNow() {
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
    const auto id = d->transportIds.take(transportRequestId); if (id.isEmpty()) return;
    const auto revision = d->cancellation->load();
    QMetaObject::invokeMethod(d->worker, [w = d->worker, revision, id, response] { if (w->generation == revision) w->sync->receive(id, response); });
}
}
#include "Controller.moc"
