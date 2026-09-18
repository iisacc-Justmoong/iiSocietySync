#pragma once
#include "iiSocietySyncExport.h"
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <memory>

namespace iiSocietySync {
// BLE advertises only a service UUID. Four bounded, read-only GATT pages expose
// bootstrap metadata. The consumer performs pairing over the advertised LAN.
class IISOCIETYSYNC_EXPORT BleDiscovery final : public QObject {
    Q_OBJECT
public:
    explicit BleDiscovery(QObject *parent = nullptr);
    ~BleDiscovery() override;
    void start(const QJsonObject &record, quint16 port);
    void stop();
    QString status() const;
    static bool supported();
signals:
    void found(QString service, QJsonObject record, QHostAddress address, quint16 port);
    void statusChanged(QString status);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
