#pragma once
#include "iiSocietySyncExport.h"
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace iiSocietySync {
using RequestSender = std::function<QString(const QString &peer, const QJsonObject &payload)>;
// Owns device-to-device synchronization and its worker. The app supplies only
// an already authenticated transport and its current authorized peer set.
class IISOCIETYSYNC_EXPORT Controller final : public QObject {
    Q_OBJECT
public:
    explicit Controller(RequestSender send, QObject *parent = nullptr);
    ~Controller() override;
    void open(const QString &container, const QString &accountScope);
    void close();
    // Desktop process handoff: cancel and drain the worker before releasing a lease.
    void closeAndWait();
    // Read-only startup/foreground metadata, serialized on the replica worker.
    // Only the most recent inspection result is delivered on this object's thread.
    void inspectContainer(const QString &container, const QString &accountScope = {});
    // Permanently cancel this controller and retire its worker without blocking
    // the caller. Desktop ownership handoff must still use closeAndWait().
    void shutdownAsync();
    void setPeers(const QStringList &authorizedPeers, const QStringList &remoteHosts);
    bool available() const;
    bool busy() const;
    QString errorString() const;
    void synchronizeNow();
    QJsonObject handle(const QString &peer, const QJsonObject &envelope);
    void receive(const QString &transportRequestId, const QJsonObject &response);
signals:
    void mirrorChanged(QJsonObject binding);
    void changed();
    void synchronized(QString peer);
    void progress(QString path, qint64 completedBytes, qint64 totalBytes);
    void containerInspected(QString path, QString scope, QJsonObject binding,
        QString containerIdentifier, QString primaryHost);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
