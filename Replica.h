#pragma once
#include "iiSocietySyncExport.h"
#include <QJsonObject>
#include <functional>
#include <memory>

namespace iiSocietySync {
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
    // Called only after the app's authenticated host descriptor handshake.
    // First adoption archives the former independent contents, then changes
    // the logical UUID. Uploads remain disabled until completeBootstrap().
    bool bindHost(const QString &peer, const QString &replica, const QString &container);
    bool bootstrapping() const;
    bool completeBootstrap();
    QJsonObject binding() const;
    static QJsonObject binding(const QString &containerPath);
    static QString primaryHost(const QString &containerPath, const QString &scope);
    static bool claimPrimaryHost(const QString &containerPath, const QString &scope, const QString &device);
    bool scan();
    QJsonObject record(const QString &path) const;
    QJsonObject changes(qint64 after = 0, qint64 through = 0);
    QJsonObject handle(const QString &peer, const QJsonObject &request);
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
