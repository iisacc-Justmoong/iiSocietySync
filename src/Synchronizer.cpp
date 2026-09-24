#include "Synchronizer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QDateTime>
#include <QMap>
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
    ContentPolicy policy = ContentPolicy::FullReplica;
    QStringList requested, photoIdentities;
    QJsonArray previews;
    qsizetype previewIndex = 0;
    bool previewsDone = false;
    bool metadataPublished = false;
    bool earlyPush = false;
    struct Request {
        QJsonObject envelope;
        std::function<void(QJsonObject)> callback;
        int pollDelay = 1;
        qint64 deadline = 0;
    };
    QString peer, remoteId, remoteContainer, expectedHost, expectedContainer;
    QHash<QString, std::shared_ptr<Request>> requests;
    int configuredWindow = 4, window = 1, pendingChunks = 0;
    QMap<qint64, QJsonObject> receivedChunks;
    QMap<qint64, qint64> acknowledgedChunks;
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
        timeout.setInterval(250);
        QObject::connect(&timeout, &QTimer::timeout, owner, [this] {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            for (const auto &request : std::as_const(requests))
                if (request->deadline <= now) { finish(false, "sync_request_timeout"); return; }
        });
    }
    void finish(bool ok, const QString &error = {}) {
        if (!active) return;
        if (ok && !store->publishStorageMap()) { finish(false, store->errorString()); return; }
        if (ok) store->completeRequests();
        if (ok && policy == ContentPolicy::MetadataFirst && !previewsDone) {
            previewsDone = true; previews = store->missingPreviews(); previewIndex = 0; previewNext(); return;
        }
        active = false; earlyPush = false; ++generation; requests.clear(); pendingChunks = 0; receivedChunks.clear(); acknowledgedChunks.clear();
        manifestReady = {}; entries.clear(); outgoing.clear(); timeout.stop();
        emit q->finished(peer, ok, error);
    }
    void previewNext() {
        if (previewIndex >= previews.size()) { finish(true); return; }
        const auto entry = previews[previewIndex++].toObject();
        send({{"action", "preview"}, {"entry", entry}}, [this, entry](const auto &response) {
            // A preview is optional; a concurrent edit must not prevent metadata
            // or completed uploads from becoming visible.
            if (response.value("ok").toBool()) store->savePreview(entry, response);
            schedule([this] { previewNext(); });
        });
    }
    bool accepted(const QJsonObject &r) {
        if (r.value("ok").toBool()) return true;
        finish(false, r.value("error").toString("sync_request_failed")); return false;
    }
    void transmit(const QString &id) {
        if (!active) return;
        const auto revision = generation; QPointer<Synchronizer> guard(q);
        QTimer::singleShot(0, q, [guard, revision, id] {
            if (guard && guard->d->active && guard->d->generation == revision && guard->d->requests.contains(id))
                emit guard->requestReady(id, guard->d->peer, guard->d->requests.value(id)->envelope);
        });
    }
    void send(QJsonObject message, std::function<void(QJsonObject)> done) {
        if (!active) return;
        if (!remoteContainer.isEmpty()) message.insert("container", remoteContainer);
        if (manifests && (message.value("action") == "begin" || message.value("action") == "chunk" || message.value("action") == "commit"))
            message.insert("manifest", manifestId);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto request = std::make_shared<Request>(); request->callback = std::move(done);
        request->envelope = {{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", store->accountScope()},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"message", message}};
        request->deadline = QDateTime::currentMSecsSinceEpoch() + 120000;
        requests.insert(id, request); transmit(id);
    }
    void describe() {
        remoteContainer.clear(); remoteId.clear(); earlyPush = false;
        send({{"action", "describe"}}, [this](const auto &r) {
            if (!accepted(r)) return;
            const auto authority = r.value("namespace").toObject();
            if (r.value("namespaceVersion") != 1 || authority.value("role") != "authority"
                || authority.value("namespace") != r.value("container") || authority.value("authority") != r.value("replica")) {
                finish(false, "namespace_authority_protocol_required"); return;
            }
            if ((!expectedHost.isEmpty() || !expectedContainer.isEmpty())
                && (peer != expectedHost || r.value("container").toString() != expectedContainer)) {
                finish(false, "account_host_mismatch"); return;
            }
            if (r.value("protocol") != 2 || !store->bindHost(peer, r.value("replica").toString(), r.value("container").toString(),
                !expectedHost.isEmpty() && !expectedContainer.isEmpty())) {
                finish(false, r.value("protocol") != 2 ? "host_mirror_protocol_required" : store->errorString()); return;
            }
            emit q->mirrorChanged(store->binding());
            if (!store->bootstrapping()) emit q->hostValidated(peer);
            manifests = r.value("manifestVersion").toInt() == 1;
            window = qMin(configuredWindow, qBound(1, r.value("transferWindow").toInt(1), 4));
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
            else journalPage(0);
        });
    }
    void journalPage(qint64 upper) {
        const auto after = store->namespaceState().value("sequence").toString();
        send({{"action", "journal"}, {"after", after}, {"through", QString::number(upper)}}, [this, upper](const auto &r) {
            if (!accepted(r)) return;
            if ((upper && r.value("through").toString() != QString::number(upper)) || !store->acceptJournal(peer, r)) {
                finish(false, store->errorString().isEmpty() ? "invalid_namespace_page" : store->errorString()); return;
            }
            if (r.value("more").toBool()) { journalPage(r.value("through").toString().toLongLong()); return; }
            requested = store->requestedPaths();
            if (policy == ContentPolicy::MetadataFirst) {
                for (const auto &path : requested) {
                    if (store->resident(path)) continue;
                    if (std::any_of(entries.cbegin(), entries.cend(), [&](const auto &e) { return e.value("path") == path; })) continue;
                    auto e = store->objectMetadata(path); e.remove("locations");
                    if (e.value("kind") == "file") entries.append(e);
                }
            }
            photoIdentities.clear();
            if (policy == ContentPolicy::MetadataFirst && !store->bootstrapping()) {
                for (const auto &value : store->missingPhotoIdentities()) {
                    const auto entry = value.toObject(); const auto path = entry.value("path").toString();
                    photoIdentities.append(path);
                    if (std::none_of(entries.cbegin(), entries.cend(), [&](const auto &e) { return e.value("path") == path; }))
                        entries.append(entry);
                }
            }
            announceLocal([this] { orderDownloads(); index = 0; pullNext(); });
        });
    }
    bool metadataOnly(const QJsonObject &entry) const {
        if (policy != ContentPolicy::MetadataFirst || entry.value("kind") != "file") return false;
        const auto path = entry.value("path").toString();
        const auto local = store->record(path);
        const auto bytes = entry.value("size").toString().toLongLong();
        // A large gallery must not gate first connection. Its small identities
        // hydrate in bounded later rounds; model indexes remain immediately usable.
        const bool identity = photoIdentities.contains(path)
            || (path.startsWith("models/") && path.endsWith("/model_index.json") && bytes <= 1024 * 1024);
        const bool pending = !local.isEmpty() && !local.contains("revision");
        return !identity && !requested.contains(path) && !pending && local.value("kind") != "directory";
    }
    bool hasLocalPayload(const QJsonObject &entry) const {
        if (entry.value("kind") != "file") return false;
        const auto path = entry.value("path").toString();
        const auto local = store->record(path);
        return local.value("kind") == "file" && local.value("hash") == entry.value("hash")
            && local.value("size") == entry.value("size") && store->resident(path);
    }
    void orderDownloads() {
        order(entries); metadataPublished = false;
        if (policy != ContentPolicy::MetadataFirst) return;
        QHash<QString, int> priorities;
        for (const auto &entry : entries) {
            const auto kind = entry.value("kind").toString(), path = entry.value("path").toString();
            priorities.insert(path, kind == "directory" ? 0 : kind != "file" ? 1
                : metadataOnly(entry) ? 2 : hasLocalPayload(entry) ? 3 : requested.contains(path) ? 4 : 5);
        }
        // Keep structural ordering, then expose remote-only objects before
        // fetching identities or reconciling older local edits. Explicit pulls
        // must not queue behind an unrelated gallery's conflict payloads.
        std::stable_sort(entries.begin(), entries.end(), [&](const auto &a, const auto &b) {
            return priorities.value(a.value("path").toString()) < priorities.value(b.value("path").toString());
        });
    }
    bool publishMetadata() {
        if (policy != ContentPolicy::MetadataFirst || metadataPublished) return true;
        if (!store->publishStorageMap()) { finish(false, store->errorString()); return false; }
        metadataPublished = true; return true;
    }
    bool completeSelected() {
        if (policy != ContentPolicy::MetadataFirst || !requested.contains(current.value("path").toString())) return true;
        if (!store->publishStorageMap()) { finish(false, store->errorString()); return false; }
        // The requested original has already passed commit's size/hash check.
        // Its consumer need not wait for unrelated uploads or conflict copies.
        store->completeRequests(); return true;
    }
    bool beginNewUploads() {
        if (policy != ContentPolicy::MetadataFirst || store->bootstrapping()) return false;
        QList<QJsonObject> fresh;
        for (const auto &entry : outgoing) {
            const auto kind = entry.value("kind").toString();
            if ((kind == "directory" || kind == "file") && store->objectMetadata(entry.value("path").toString()).isEmpty())
                fresh.append(entry);
        }
        if (fresh.isEmpty()) return false;
        order(fresh);
        std::stable_sort(fresh.begin(), fresh.end(), [](const auto &a, const auto &b) {
            const bool directoryA = a.value("kind") == "directory", directoryB = b.value("kind") == "directory";
            if (directoryA != directoryB) return directoryA;
            // A newly completed local output precedes older unsubmitted files.
            return !directoryA && a.value("sequence").toString().toLongLong() > b.value("sequence").toString().toLongLong();
        });
        // The complete outgoing manifest was already acknowledged. Send its
        // new objects without advancing either cursor past pending conflicts.
        earlyPush = true; entries = std::move(fresh); index = 0; pushNext(); return true;
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
            if (!publishMetadata()) return;
            if (beginNewUploads()) return;
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
                emit q->hostValidated(peer);
            }
            if (!store->scan()) { finish(false, store->errorString()); return; }
            announceLocal([this] { entries = outgoing; through = pushThrough; index = 0; order(entries); pushNext(); }); return;
        }
        current = entries[index];
        if (metadataOnly(current)) {
            if (!store->acceptMetadata(peer, current)) { finish(false, store->errorString()); return; }
            ++index; schedule([this] { pullNext(); }); return;
        }
        const bool localPayload = policy == ContentPolicy::MetadataFirst && hasLocalPayload(current);
        if (current.value("kind") == "file" && !localPayload) {
            if (!publishMetadata()) return;
            if (!requested.contains(current.value("path").toString()) && beginNewUploads()) return;
        }
        const auto r = store->handle(peer, {{"action", "begin"}, {"entry", current}});
        if (!accepted(r)) return;
        if (r.value("complete").toBool()) {
            if (!completeSelected()) return;
            ++index; schedule([this] { pullNext(); }); return;
        }
        if (!publishMetadata()) return;
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
        entries.clear(); outgoing.clear(); index = 0; earlyPush = false;
        if (!store->bootstrapping() && !store->scan()) { finish(false, store->errorString()); return true; }
        page(pulled, 0);
        return true;
    }
    void pullChunk() {
        const auto path = current.value("path").toString();
        if (policy == ContentPolicy::MetadataFirst && requested.contains(path)
            && !store->requestedPaths().contains(path)) {
            // Cancellation takes effect after the current bounded window. A
            // completed private part must not become an original after cancel.
            ++index; schedule([this] { pullNext(); }); return;
        }
        const auto size = current.value("size").toString().toLongLong();
        if (offset == size) {
            if (!accepted(store->handle(peer, {{"action", "commit"}, {"entry", current}}))) return;
            if (!completeSelected()) return;
            ++index; schedule([this] { pullNext(); }); return;
        }
        if (refreshBetweenChunks()) return;
        const auto start = offset;
        pendingChunks = int(qMin<qint64>(window, (size - start + Replica::ChunkBytes - 1) / Replica::ChunkBytes));
        for (int i = 0; i < pendingChunks; ++i) {
            const auto position = start + i * Replica::ChunkBytes;
            send({{"action", "read"}, {"entry", current}, {"offset", QString::number(position)}}, [this, size, position](const auto &r) {
                if (!accepted(r)) return;
                const auto bytes = QByteArray::fromBase64Encoding(r.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); qint64 received;
                if (!bytes || bytes.decoded.size() != qMin(Replica::ChunkBytes, size - position)
                    || !nonnegative(r.value("offset"), &received) || received != position || r.value("version") != current.value("version")) {
                    finish(false, "invalid_download_chunk"); return;
                }
                receivedChunks.insert(position, r); --pendingChunks;
                while (active && receivedChunks.contains(offset)) {
                    const auto chunk = receivedChunks.take(offset);
                    if (!accepted(store->handle(peer, {{"action", "chunk"}, {"entry", current}, {"offset", QString::number(offset)}, {"data", chunk.value("data")}}))) return;
                    offset += qMin(Replica::ChunkBytes, size - offset); transferredInSlice = true;
                    emit q->progress(current.value("path").toString(), offset, size);
                }
                if (active && !pendingChunks) schedule([this] { pullChunk(); });
            });
        }
    }
    void pushNext() {
        if (!active) return;
        if (index >= entries.size()) {
            if (earlyPush) {
                earlyPush = false; entries.clear();
                // Receive the authority's acknowledgement before returning to
                // unrelated reconciliation. The old pull cursor stays intact.
                page(pulled, 0); return;
            }
            pushed = through;
            const bool ok = store->savePeerState(peer, remoteId, remoteContainer, pulled, pushed);
            if (!ok) { finish(false, store->errorString()); return; }
            // An upload is only a proposal. Completion includes the authority's
            // resulting revision, including its conflict decision, back on disk.
            if (!entries.isEmpty()) { entries.clear(); page(pulled, 0); return; }
            finish(true); return;
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
        const auto start = offset;
        pendingChunks = int(qMin<qint64>(window, (size - start + Replica::ChunkBytes - 1) / Replica::ChunkBytes));
        for (int i = 0; i < pendingChunks; ++i) {
            const auto position = start + i * Replica::ChunkBytes;
            const auto r = store->handle(peer, {{"action", "read"}, {"entry", current}, {"offset", QString::number(position)}});
            if (!accepted(r)) return;
            const auto bytes = QByteArray::fromBase64Encoding(r.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes || bytes.decoded.size() != qMin(Replica::ChunkBytes, size - position)) { finish(false, "invalid_local_chunk"); return; }
            const auto next = position + bytes.decoded.size();
            send({{"action", "chunk"}, {"entry", current}, {"offset", QString::number(position)}, {"data", r.value("data")}}, [this, position, next, size](const auto &reply) {
                if (!accepted(reply)) return;
                if (reply.value("offset").toString() != QString::number(next)) { finish(false, "invalid_upload_acknowledgement"); return; }
                acknowledgedChunks.insert(position, next); --pendingChunks;
                while (active && acknowledgedChunks.contains(offset)) {
                    offset = acknowledgedChunks.take(offset); transferredInSlice = true;
                    emit q->progress(current.value("path").toString(), offset, size);
                }
                if (active && !pendingChunks) schedule([this] { pushChunk(); });
            });
        }
    }
};
Synchronizer::Synchronizer(Replica *replica, QObject *parent) : QObject(parent), d(std::make_unique<Private>(this, replica)) {}
Synchronizer::~Synchronizer() { stop(); }
bool Synchronizer::busy() const { return d->active; }
bool Synchronizer::setContentPolicy(ContentPolicy policy) {
    if (d->active) return false;
    d->policy = policy; return true;
}
bool Synchronizer::setTransferWindow(int chunks) {
    if (d->active || chunks < 1 || chunks > 4) return false;
    d->configuredWindow = chunks; return true;
}
bool Synchronizer::start(const QString &peer) {
    if (d->active || !d->store || !d->store->isOpen() || peer.isEmpty()) return false;
    d->active = true; ++d->generation; d->peer = peer; d->entries.clear();
    d->transferSlice.invalidate(); d->transferredInSlice = false;
    d->previewsDone = false; d->previews = {}; d->previewIndex = 0;
    d->pulled = 0; d->pushed = 0; d->timeout.start(); d->describe(); return true;
}
void Synchronizer::setExpectedHost(QString host, QString container) {
    stop(); d->expectedHost = std::move(host); d->expectedContainer = std::move(container);
}
void Synchronizer::stop() { if (d->active) d->finish(false, "cancelled"); }
void Synchronizer::receive(const QString &requestId, const QJsonObject &response) {
    if (!d->active || !d->requests.contains(requestId)) return;
    const auto request = d->requests.value(requestId);
    if (!response.value("ok").toBool()) { d->finish(false, response.value("error").toString("transport_failed")); return; }
    if (response.value("pending").toBool()) {
        const auto revision = d->generation; QPointer<Synchronizer> guard(this);
        const auto delay = request->pollDelay; request->pollDelay = qMin(100, delay * 2);
        QTimer::singleShot(delay, this, [guard, revision, requestId] { if (guard && guard->d->active && guard->d->generation == revision) guard->d->transmit(requestId); }); return;
    }
    if (!response.value("result").isObject()) { d->finish(false, "invalid_sync_response"); return; }
    d->requests.remove(requestId);
    const auto done = std::move(request->callback); if (done) done(response.value("result").toObject());
}
}
