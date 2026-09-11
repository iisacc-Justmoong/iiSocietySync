#pragma once
#include "Replica.h"
#include <QObject>

namespace iiSocietySync {
// Bidirectional pull/push over a supplied authenticated request/reply channel.
// Changes and checkpoints survive restarts; sockets and login policy do not.
class IISOCIETYSYNC_EXPORT Synchronizer final : public QObject {
    Q_OBJECT
public:
    explicit Synchronizer(Replica *replica, QObject *parent = nullptr);
    ~Synchronizer() override;
    bool start(const QString &peer);
    void stop();
    bool busy() const;
    void receive(const QString &requestId, const QJsonObject &response);
signals:
    void mirrorChanged(QJsonObject binding);
    void requestReady(QString id, QString peer, QJsonObject payload);
    void progress(QString path, qint64 completedBytes, qint64 totalBytes);
    void finished(QString peer, bool success, QString error);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
