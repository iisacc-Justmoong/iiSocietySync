#include "Synchronizer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace iiSocietySync {
namespace {
bool nonnegative(const QJsonValue &v, qint64 *value) { bool ok; *value = v.toString().toLongLong(&ok); return ok && *value >= 0; }
void order(QList<QJsonObject> &entries) {
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        const auto rank = [](const auto &e) { return e.value("kind") == "directory" ? 0 : e.value("kind") == "file" ? 2 : 1; };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        if (rank(a) == 2) {
            const auto sa = a.value("size").toString().toLongLong(), sb = b.value("size").toString().toLongLong();
            if (sa != sb) return sa < sb;
        }
        const auto pa = a.value("path").toString(), pb = b.value("path").toString();
        if (pa.count('/') != pb.count('/')) return rank(a) == 1 ? pa.count('/') > pb.count('/') : pa.count('/') < pb.count('/');
        return pa < pb;
    });
}
}
class Synchronizer::Private {
public:
    Synchronizer *q; Replica *store; QTimer timeout;
    bool active = false; quint64 generation = 0;
    int pollDelay = 1;
    QString peer, remoteId, remoteContainer, waiting;
    QJsonObject envelope; std::function<void(QJsonObject)> callback;
    QList<QJsonObject> entries; qsizetype index = 0;
    qint64 pulled = 0, pushed = 0, through = 0, offset = 0;
    QJsonObject current;
    bool manifests = false;
    QList<QJsonObject> outgoing;
    qint64 pushThrough = 0;
    qsizetype manifestIndex = 0;
    QString manifestId;
    std::function<void()> manifestReady;
    QElapsedTimer transferSlice;
    bool transferredInSlice = false;
    explicit Private(Synchronizer *owner, Replica *replica) : q(owner), store(replica) {
        timeout.setSingleShot(true); timeout.setInterval(120000);
        QObject::connect(&timeout, &QTimer::timeout, owner, [this] { finish(false, "sync_request_timeout"); });
    }
    void finish(bool ok, const QString &error = {}) {
        if (!active) return;
        active = false; ++generation; waiting.clear(); callback = {}; manifestReady = {}; entries.clear(); outgoing.clear(); timeout.stop();
        emit q->finished(peer, ok, error);
    }
    bool accepted(const QJsonObject &r) {
        if (r.value("ok").toBool()) return true;
        finish(false, r.value("error").toString("sync_request_failed")); return false;
    }
    void transmit() {
        if (!active) return;
        waiting = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto id = waiting; const auto revision = generation; QPointer<Synchronizer> guard(q);
        QTimer::singleShot(0, q, [guard, revision, id] {
            if (guard && guard->d->active && guard->d->generation == revision && guard->d->waiting == id)
                emit guard->requestReady(id, guard->d->peer, guard->d->envelope);
        });
    }
    void send(QJsonObject message, std::function<void(QJsonObject)> done) {
        if (!active) return;
        if (!remoteContainer.isEmpty()) message.insert("container", remoteContainer);
        if (manifests && (message.value("action") == "begin" || message.value("action") == "chunk" || message.value("action") == "commit"))
            message.insert("manifest", manifestId);
        callback = std::move(done);
        envelope = {{"op", "society.sync"}, {"protocol", 2}, {"scope", store->accountScope()},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"message", message}};
        pollDelay = 1; timeout.start(); transmit();
    }
    void describe() {
        remoteContainer.clear(); remoteId.clear();
        send({{"action", "describe"}}, [this](const auto &r) {
            if (!accepted(r)) return;
            if (r.value("protocol") != 2 || !store->bindHost(peer, r.value("replica").toString(), r.value("container").toString())) {
                finish(false, r.value("protocol") != 2 ? "host_mirror_protocol_required" : store->errorString()); return;
            }
            emit q->mirrorChanged(store->binding());
            manifests = r.value("manifestVersion").toInt() == 1;
            remoteId = r.value("replica").toString(); remoteContainer = r.value("container").toString();
            const auto state = store->peerState(peer);
            pulled = state.value("pulled").toString().toLongLong(); pushed = state.value("pushed").toString().toLongLong();
            if (!store->bootstrapping() && !store->scan()) { finish(false, store->errorString()); return; }
            page(pulled, 0);
        });
    }
    void page(qint64 after, qint64 upper) {
        send({{"action", "changes"}, {"after", QString::number(after)}, {"through", QString::number(upper)}}, [this, after, upper](const auto &r) {
            if (!accepted(r)) return;
            const auto replica = r.value("replica").toString(), container = r.value("container").toString(); qint64 next, limit;
            if (QUuid(replica).isNull() || QUuid(container).isNull() || replica == store->replicaId()
                || (!remoteId.isEmpty() && (remoteId != replica || remoteContainer != container))
                || !r.value("entries").isArray() || !nonnegative(r.value("next"), &next) || !nonnegative(r.value("through"), &limit)
                || next < after || next > limit || (upper && upper != limit) || (r.value("more").toBool() && next <= after)) {
                finish(false, "remote_container_or_manifest_changed"); return;
            }
            remoteId = replica; remoteContainer = container;
            if (!store->savePeerState(peer, remoteId, remoteContainer, pulled, pushed)) { finish(false, store->errorString()); return; }
            qint64 previous = after;
            for (const auto &item : r.value("entries").toArray()) {
                const auto e = item.toObject(); qint64 sequence;
                if (!Replica::validRecord(e) || !nonnegative(e.value("sequence"), &sequence) || sequence <= previous || sequence > next || entries.size() >= 250000) {
                    finish(false, "invalid_remote_manifest"); return;
                }
                previous = sequence; entries.append(e);
            }
            through = limit;
            if (r.value("more").toBool()) page(next, limit);
            else announceLocal([this] { order(entries); index = 0; pullNext(); });
        });
    }
    void announceLocal(std::function<void()> done) {
        outgoing.clear(); pushThrough = pushed;
        // The old independent drive is never advertised during first adoption.
        if (!store->bootstrapping()) {
            qint64 cursor = pushed, upper = 0;
            while (true) {
                const auto page = store->changes(cursor, upper); if (!accepted(page)) return;
                upper = page.value("through").toString().toLongLong(); cursor = page.value("next").toString().toLongLong();
                for (const auto &e : page.value("entries").toArray()) outgoing.append(e.toObject());
                if (outgoing.size() > 250000) { finish(false, "manifest_capacity"); return; }
                if (!page.value("more").toBool()) break;
            }
            pushThrough = upper;
        }
        if (!manifests) { done(); return; }
        QCryptographicHash digest(QCryptographicHash::Sha256);
        for (const auto &entry : outgoing) {
            digest.addData(QJsonDocument(entry).toJson(QJsonDocument::Compact)); digest.addData(QByteArrayView("\n"));
        }
        manifestId = QString::fromLatin1(digest.result().toHex()); manifestIndex = 0; manifestReady = std::move(done);
        announcePage();
    }
    void announcePage() {
        QJsonArray page; qint64 bytes = 0;
        const auto start = manifestIndex;
        while (manifestIndex < outgoing.size() && page.size() < 128) {
            const auto &entry = outgoing[manifestIndex]; const auto size = QJsonDocument(entry).toJson(QJsonDocument::Compact).size();
            if (bytes + size > 256 * 1024) break;
            page.append(entry); bytes += size; ++manifestIndex;
        }
        send({{"action", "manifest"}, {"manifest", manifestId}, {"replica", store->replicaId()},
              {"offset", QString::number(start)}, {"total", QString::number(outgoing.size())},
              {"after", QString::number(pushed)}, {"through", QString::number(pushThrough)}, {"entries", page}}, [this](const auto &r) {
            if (!accepted(r)) return;
            const bool complete = manifestIndex == outgoing.size();
            if (r.value("manifest") != manifestId || r.value("next").toString() != QString::number(manifestIndex)
                || !r.value("complete").isBool() || r.value("complete").toBool() != complete) {
                finish(false, "invalid_manifest_acknowledgement"); return;
            }
            if (!complete) { announcePage(); return; }
            const auto done = std::move(manifestReady); manifestReady = {}; if (done) done();
        });
    }
    void pullNext() {
        if (!active) return;
        if (index >= entries.size()) {
            const bool caughtUp = through == pulled && entries.isEmpty();
            pulled = through;
            if (!store->savePeerState(peer, remoteId, remoteContainer, pulled, pushed)) { finish(false, store->errorString()); return; }
            entries.clear();
            if (store->bootstrapping()) {
                // Rescan the host after the bounded snapshot. Updates made
                // during a download must arrive before the mirror is usable.
                if (!caughtUp) { page(pulled, 0); return; }
                if (!store->completeBootstrap()) { finish(false, store->errorString()); return; }
                emit q->mirrorChanged(store->binding());
            }
            if (!store->scan()) { finish(false, store->errorString()); return; }
            announceLocal([this] { entries = outgoing; through = pushThrough; index = 0; order(entries); pushNext(); }); return;
        }
        current = entries[index];
        const auto r = store->handle(peer, {{"action", "begin"}, {"entry", current}});
        if (!accepted(r)) return;
        if (r.value("complete").toBool()) { ++index; schedule([this] { pullNext(); }); return; }
        if (!nonnegative(r.value("offset"), &offset) || offset > current.value("size").toString().toLongLong()) { finish(false, "invalid_transfer_offset"); return; }
        pullChunk();
    }
    void schedule(std::function<void()> action) {
        const auto revision = generation; QPointer<Synchronizer> guard(q);
        QTimer::singleShot(0, q, [guard, revision, action = std::move(action)] { if (guard && guard->d->active && revision == guard->d->generation) action(); });
    }
    bool refreshBetweenChunks() {
        if (!transferSlice.isValid()) transferSlice.start();
        if (!transferredInSlice || !transferSlice.hasExpired(1000)) return false;
        // Keep partial payloads on disk and cursors uncommitted. A fresh bounded
        // snapshot lets new small edits/deletions precede the resumed bulk file.
        transferSlice.invalidate(); transferredInSlice = false;
        entries.clear(); outgoing.clear(); index = 0;
        if (!store->bootstrapping() && !store->scan()) { finish(false, store->errorString()); return true; }
        page(pulled, 0);
        return true;
    }
    void pullChunk() {
        const auto size = current.value("size").toString().toLongLong();
        if (offset == size) {
            if (!accepted(store->handle(peer, {{"action", "commit"}, {"entry", current}}))) return;
            ++index; schedule([this] { pullNext(); }); return;
        }
        if (refreshBetweenChunks()) return;
        send({{"action", "read"}, {"entry", current}, {"offset", QString::number(offset)}}, [this, size](const auto &r) {
            if (!accepted(r)) return;
            const auto bytes = QByteArray::fromBase64Encoding(r.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); qint64 received;
            if (!bytes || bytes.decoded.isEmpty() || bytes.decoded.size() > Replica::ChunkBytes || bytes.decoded.size() > size - offset
                || !nonnegative(r.value("offset"), &received) || received != offset || r.value("version") != current.value("version")) {
                finish(false, "invalid_download_chunk"); return;
            }
            if (!accepted(store->handle(peer, {{"action", "chunk"}, {"entry", current}, {"offset", QString::number(offset)}, {"data", r.value("data")}}))) return;
            offset += bytes.decoded.size(); transferredInSlice = true; emit q->progress(current.value("path").toString(), offset, size);
            if (active) pullChunk();
        });
    }
    void pushNext() {
        if (!active) return;
        if (index >= entries.size()) {
            pushed = through;
            const bool ok = store->savePeerState(peer, remoteId, remoteContainer, pulled, pushed);
            finish(ok, ok ? QString() : store->errorString()); return;
        }
        current = entries[index];
        send({{"action", "begin"}, {"entry", current}}, [this](const auto &r) {
            if (!accepted(r)) return;
            if (r.value("complete").toBool()) { ++index; pushNext(); return; }
            if (current.value("kind") != "file" || !nonnegative(r.value("offset"), &offset) || offset > current.value("size").toString().toLongLong()) { finish(false, "invalid_transfer_offset"); return; }
            pushChunk();
        });
    }
    void pushChunk() {
        const auto size = current.value("size").toString().toLongLong();
        if (offset == size) {
            send({{"action", "commit"}, {"entry", current}}, [this](const auto &r) { if (accepted(r)) { ++index; pushNext(); } }); return;
        }
        if (refreshBetweenChunks()) return;
        const auto r = store->handle(peer, {{"action", "read"}, {"entry", current}, {"offset", QString::number(offset)}});
        if (!accepted(r)) return;
        const auto bytes = QByteArray::fromBase64Encoding(r.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!bytes || bytes.decoded.isEmpty() || bytes.decoded.size() > size - offset) { finish(false, "invalid_local_chunk"); return; }
        const auto next = offset + bytes.decoded.size();
        send({{"action", "chunk"}, {"entry", current}, {"offset", QString::number(offset)}, {"data", r.value("data")}}, [this, next, size](const auto &reply) {
            if (!accepted(reply)) return;
            if (reply.value("offset").toString() != QString::number(next)) { finish(false, "invalid_upload_acknowledgement"); return; }
            offset = next; transferredInSlice = true; emit q->progress(current.value("path").toString(), offset, size); if (active) pushChunk();
        });
    }
};
Synchronizer::Synchronizer(Replica *replica, QObject *parent) : QObject(parent), d(std::make_unique<Private>(this, replica)) {}
Synchronizer::~Synchronizer() { stop(); }
bool Synchronizer::busy() const { return d->active; }
bool Synchronizer::start(const QString &peer) {
    if (d->active || !d->store || !d->store->isOpen() || peer.isEmpty()) return false;
    d->active = true; ++d->generation; d->peer = peer; d->entries.clear();
    d->transferSlice.invalidate(); d->transferredInSlice = false;
    d->pulled = 0; d->pushed = 0; d->describe(); return true;
}
void Synchronizer::stop() { if (d->active) d->finish(false, "cancelled"); }
void Synchronizer::receive(const QString &requestId, const QJsonObject &response) {
    if (!d->active || requestId != d->waiting) return;
    d->waiting.clear();
    if (!response.value("ok").toBool()) { d->finish(false, response.value("error").toString("transport_failed")); return; }
    if (response.value("pending").toBool()) {
        const auto revision = d->generation; QPointer<Synchronizer> guard(this);
        const auto delay = d->pollDelay; d->pollDelay = qMin(100, delay * 2);
        QTimer::singleShot(delay, this, [guard, revision] { if (guard && guard->d->active && guard->d->generation == revision) guard->d->transmit(); }); return;
    }
    if (!response.value("result").isObject()) { d->finish(false, "invalid_sync_response"); return; }
    d->timeout.stop(); const auto done = std::move(d->callback); d->callback = {}; if (done) done(response.value("result").toObject());
}
}
