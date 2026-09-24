#pragma once
#include "iiSocietySyncExport.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <functional>
#include <memory>

namespace iiSocietySync {
class ObjectProvider;
// One local container replica, used on one thread. Its SQLite journal is local
// bookkeeping, never a file to replicate. Call handle only on an authenticated
// Society channel; Controller supplies that boundary and runs I/O off the UI.
class IISOCIETYSYNC_EXPORT Replica final {
public:
    Replica();
    ~Replica();
    Replica(const Replica &) = delete;
    Replica &operator=(const Replica &) = delete;
    bool open(const QString &container, const QString &accountScope);
    void close();
    bool isOpen() const;
    QString errorString() const;
    QString containerId() const;
    QString replicaId() const;
    QString accountScope() const;
    void setCancellation(std::function<bool()> cancelled);
    void setVerificationProgress(std::function<void(const QString &, qint64, qint64)> progress);
    // Called only after the app's authenticated host descriptor handshake.
    // First adoption archives the former independent contents, then changes
    // the logical UUID. Uploads remain disabled until completeBootstrap().
    bool bindHost(const QString &peer, const QString &replica, const QString &container, bool replaceHost = false);
    bool bootstrapping() const;
    bool completeBootstrap();
    QJsonObject binding() const;
    static QJsonObject binding(const QString &containerPath);
    static QString primaryHost(const QString &containerPath, const QString &scope);
    static bool claimPrimaryHost(const QString &containerPath, const QString &scope, const QString &device);
    // Small verified batches commit independently. Cancellation retains completed
    // hashes; deletion reconciliation runs only after the full walk succeeds.
    bool scan();
    bool publishStorageMap();
    bool acceptMetadata(const QString &peer, const QJsonObject &record);
    bool resident(const QString &path) const;
    QStringList requestedPaths();
    void completeRequests();
    QJsonArray missingPhotoIdentities() const;
    QJsonArray missingPreviews() const;
    bool savePreview(const QJsonObject &entry, const QJsonObject &response);
    // Confirmed namespace metadata is distinct from the device's working files.
    // Only the authority appends revisions; a mirror imports its host's journal.
    QJsonObject namespaceState() const;
    QJsonObject objectMetadata(const QString &path) const;
    QJsonObject revision(const QString &id) const;
    QJsonObject journal(qint64 after = 0, qint64 through = 0);
    bool acceptJournal(const QString &peer, const QJsonObject &page);
    // Add verified backing copies without changing logical object identity.
    // Restore only a missing projection; never overwrite a pending local edit.
    bool placeObject(const QString &path, ObjectProvider &provider);
    bool restoreObject(const QString &path, ObjectProvider &provider);
    QJsonObject record(const QString &path) const;
    QJsonObject changes(qint64 after = 0, qint64 through = 0);
    QJsonObject handle(const QString &peer, const QJsonObject &request);
    // A controller with a background indexer serves the latest committed snapshot
    // without starting an expensive scan on the replication worker.
    QJsonObject handle(const QString &peer, const QJsonObject &request, bool refreshIndex);
    QJsonObject peerState(const QString &peer) const;
    bool savePeerState(const QString &peer, const QString &replica, const QString &container,
                       qint64 pulled, qint64 pushed);
    static bool validRecord(const QJsonObject &record);
    static constexpr qint64 ChunkBytes = 256 * 1024;
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
