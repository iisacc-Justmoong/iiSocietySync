#pragma once
#include "iiSocietySyncExport.h"
#include <StorageBridge.h>
#include <functional>

namespace iiSocietySync {
struct IISOCIETYSYNC_EXPORT ObjectIdentity {
    QString nameSpace, sha256;
    qint64 size = 0;
    QString key() const;
    bool valid() const;
};
// Called from storage workers; the callback must be thread safe (e.g. atomic).
using ProviderCancellation = std::function<bool()>;

// Providers hold bytes, never authority, account policy or a SQLite journal.
// Synchronous worker-thread API; implementations must verify size and SHA-256.
class IISOCIETYSYNC_EXPORT ObjectProvider {
public:
    virtual ~ObjectProvider() = default;
    virtual QString id() const = 0;
    virtual QString kind() const = 0;
    virtual bool put(const ObjectIdentity &, const QString &source, QString *error,
                     ProviderCancellation cancelled = {}) = 0;
    virtual bool get(const ObjectIdentity &, const QString &destination, QString *error,
                     ProviderCancellation cancelled = {}) = 0;
};

// Existing, dedicated local directory or mounted NAS share. No hard links or
// database locking are required from the storage filesystem.
class IISOCIETYSYNC_EXPORT DirectoryObjectProvider final : public ObjectProvider {
public:
    DirectoryObjectProvider(QString id, QString directory, bool nas = false);
    QString id() const override;
    QString kind() const override;
    bool put(const ObjectIdentity &, const QString &, QString *, ProviderCancellation = {}) override;
    bool get(const ObjectIdentity &, const QString &, QString *, ProviderCancellation = {}) override;
private:
    QString m_id, m_root;
    bool m_nas;
    QString path(const ObjectIdentity &, QString *error, bool create);
};

// Named rclone S3/NAS remote, using iiServerHost's bounded, credential-isolated
// bridge (including S3 multipart). The config and backend stay on the host.
class IISOCIETYSYNC_EXPORT RemoteObjectProvider final : public ObjectProvider {
public:
    RemoteObjectProvider(QString id, QString kind, QString remotePrefix, iiServerHost::StorageBridgeOptions backend);
    QString id() const override;
    QString kind() const override;
    bool put(const ObjectIdentity &, const QString &, QString *, ProviderCancellation = {}) override;
    bool get(const ObjectIdentity &, const QString &, QString *, ProviderCancellation = {}) override;
private:
    QString m_id, m_kind, m_prefix;
    iiServerHost::StorageBridgeOptions m_backend;
    bool copy(const QString &, const QString &, QString *, const ProviderCancellation &);
};
}
