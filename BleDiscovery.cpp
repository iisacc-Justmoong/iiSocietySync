#include "BleDiscovery.h"
#include "NearbyBootstrap.h"
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#ifdef IISOCIETYSYNC_BLE
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothPermission>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyController>
#include <QLowEnergyServiceData>
#include <QDateTime>
#include <QQueue>
#endif

namespace iiSocietySync {
class BleDiscovery::Private {
public:
    BleDiscovery *q;
    QString state = "stopped";
    quint64 generation = 0;
    QByteArray bootstrap;
    QJsonObject identity;
    quint16 invitationPort = 0;
    bool requested = false, permissionPending = false;
#ifdef IISOCIETYSYNC_BLE
    QBluetoothDeviceDiscoveryAgent *scanner = nullptr;
    QLowEnergyController *peripheral = nullptr;
    QPointer<QLowEnergyController> central;
    QTimer scanTimer, connectionTimer;
    QQueue<QBluetoothDeviceInfo> queue;
    QHash<QString, qint64> visited;
    QByteArray incoming;
    int page = 0;
    static QBluetoothUuid serviceUuid() { return QBluetoothUuid(QStringLiteral("a91f1130-7ab5-4e10-92a0-642309ef0001")); }
    static QBluetoothUuid pageUuid(int index) {
        return QBluetoothUuid(QStringLiteral("a91f1130-7ab5-4e10-92a0-642309ef%1").arg(index + 2, 4, 16, QLatin1Char('0')));
    }
    void scan() {
        if (scanner && !scanner->isActive()) { visited.clear(); scanner->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod); }
    }
    void retireCentral() {
        connectionTimer.stop(); incoming.clear();
        if (central) { auto *old = central.data(); central = nullptr; old->disconnect(q); old->disconnectFromDevice(); old->deleteLater(); }
        const auto revision = generation;
        QTimer::singleShot(0, q, [this, revision] { if (revision == generation) connectNext(); });
    }
    void connectNext() {
        if (central || queue.isEmpty()) return;
        auto *controller = QLowEnergyController::createCentral(queue.dequeue(), q); central = controller;
        const auto revision = generation;
        QObject::connect(controller, &QLowEnergyController::connected, q, [controller] { controller->discoverServices(); });
        QObject::connect(controller, &QLowEnergyController::discoveryFinished, q, [this, controller, revision] {
            if (revision != generation || central != controller) return;
            auto *service = controller->createServiceObject(serviceUuid(), controller);
            if (!service) { retireCentral(); return; }
            QObject::connect(service, &QLowEnergyService::stateChanged, q, [this, service, controller, revision](auto state) {
                if (revision != generation || central != controller || state != QLowEnergyService::RemoteServiceDiscovered) return;
                page = 0; incoming.clear(); readPage(service);
            });
            QObject::connect(service, &QLowEnergyService::characteristicRead, q, [this, service, controller, revision](auto characteristic, auto bytes) {
                if (revision != generation || central != controller || characteristic.uuid() != pageUuid(page)) return;
                if (bytes.size() > 500 || incoming.size() + bytes.size() > NearbyBootstrap::MaximumBytes) { retireCentral(); return; }
                incoming += bytes;
                if (++page < 4) { readPage(service); return; }
                const auto record = NearbyBootstrap::decode(incoming);
                if (record && record->record.value("scope") == identity.value("scope") && record->record.value("id") != identity.value("id")) {
                    for (const auto &address : record->addresses) {
                        emit q->found("ble:" + record->record.value("id").toString(), record->record, QHostAddress(address), record->port);
                        if (revision != generation || central != controller) return;
                    }
                }
                retireCentral();
            });
            QObject::connect(service, &QLowEnergyService::errorOccurred, q, [this, controller, revision](auto) {
                if (revision == generation && central == controller) retireCentral();
            });
            service->discoverDetails(QLowEnergyService::SkipValueDiscovery);
        });
        QObject::connect(controller, &QLowEnergyController::errorOccurred, q, [this, controller, revision](auto) {
            if (revision == generation && central == controller) retireCentral();
        });
        QObject::connect(controller, &QLowEnergyController::disconnected, q, [this, controller, revision] {
            if (revision == generation && central == controller) retireCentral();
        });
        connectionTimer.start(6000); controller->connectToDevice();
    }
    void readPage(QLowEnergyService *service) {
        const auto characteristic = service->characteristic(pageUuid(page));
        if (!characteristic.isValid()) { retireCentral(); return; }
        service->readCharacteristic(characteristic);
    }
    void begin() {
        scanner = new QBluetoothDeviceDiscoveryAgent(q); scanner->setLowEnergyDiscoveryTimeout(5000);
        QObject::connect(scanner, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, q, [this](const QBluetoothDeviceInfo &device) {
            if (!device.serviceUuids().contains(serviceUuid()) || queue.size() >= 16) return;
            const auto key = device.deviceUuid().isNull() ? device.address().toString() : device.deviceUuid().toString();
            if (visited.contains(key)) return;
            visited.insert(key, QDateTime::currentMSecsSinceEpoch()); queue.enqueue(device); connectNext();
        });
        QObject::connect(scanner, &QBluetoothDeviceDiscoveryAgent::errorOccurred, q, [this](auto) { setState("unavailable: " + scanner->errorString()); });
#ifndef Q_OS_WIN
        peripheral = QLowEnergyController::createPeripheral(q);
        QLowEnergyServiceData service; service.setType(QLowEnergyServiceData::ServiceTypePrimary); service.setUuid(serviceUuid());
        for (int i = 0; i < 4; ++i) {
            QLowEnergyCharacteristicData value; value.setUuid(pageUuid(i)); value.setProperties(QLowEnergyCharacteristic::Read);
            value.setValue(bootstrap.mid(i * 500, 500)); value.setValueLength(0, 500); service.addCharacteristic(value);
        }
        if (!peripheral->addService(service, peripheral)) {
            setState("unavailable: cannot publish BLE bootstrap");
            // A machine without peripheral support can still discover a peer.
            scanTimer.start(12000); scan(); return;
        }
        QObject::connect(peripheral, &QLowEnergyController::errorOccurred, q, [this](auto) { setState("unavailable: " + peripheral->errorString()); });
        const auto advertise = [this] {
            QLowEnergyAdvertisingData advertisement; advertisement.setServices({serviceUuid()});
            peripheral->startAdvertising(QLowEnergyAdvertisingParameters(), advertisement);
        };
        QObject::connect(peripheral, &QLowEnergyController::disconnected, q, advertise);
        advertise();
#endif
        scanTimer.start(12000); scan(); setState("discovering");
    }
#endif
    explicit Private(BleDiscovery *owner) : q(owner) {
#ifdef IISOCIETYSYNC_BLE
        connectionTimer.setSingleShot(true);
        QObject::connect(&connectionTimer, &QTimer::timeout, q, [this] { retireCentral(); });
        QObject::connect(&scanTimer, &QTimer::timeout, q, [this] {
            const auto updated = NearbyBootstrap{identity, NearbyBootstrap::localAddresses(), invitationPort}.encode();
            if (updated != bootstrap) {
                // Wi-Fi may have appeared or changed address since BLE started.
                const auto record = identity; const auto port = invitationPort; q->start(record, port); return;
            }
            scan();
        });
#endif
    }
    void setState(const QString &value) { if (state != value) { state = value; emit q->statusChanged(state); } }
};
BleDiscovery::BleDiscovery(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) {}
BleDiscovery::~BleDiscovery() { stop(); }
bool BleDiscovery::supported() {
#ifdef IISOCIETYSYNC_BLE
    return true;
#else
    return false;
#endif
}
QString BleDiscovery::status() const { return d->state; }
void BleDiscovery::start(const QJsonObject &record, quint16 port) {
    stop(); d->identity = record; d->invitationPort = port; d->requested = true;
    if (qEnvironmentVariableIntValue("IISOCIETYSYNC_DISABLE_BLE") == 1) { d->setState("disabled"); return; }
    d->bootstrap = NearbyBootstrap{record, NearbyBootstrap::localAddresses(), port}.encode();
    if (d->bootstrap.isEmpty()) {
        d->setState("unavailable: no LAN bootstrap endpoint");
#ifdef IISOCIETYSYNC_BLE
        if (port && !record.isEmpty()) d->scanTimer.start(12000);
#endif
        return;
    }
#ifdef IISOCIETYSYNC_BLE
    QBluetoothPermission permission; permission.setCommunicationModes(QBluetoothPermission::Access | QBluetoothPermission::Advertise);
    if (d->permissionPending) { d->setState("permission-required"); return; }
    const auto state = qApp->checkPermission(permission);
    if (state == Qt::PermissionStatus::Granted) { d->begin(); return; }
    if (state == Qt::PermissionStatus::Denied) { d->setState("unavailable: Bluetooth permission denied"); return; }
    d->setState("permission-required"); d->permissionPending = true;
    qApp->requestPermission(permission, this, [this](const QPermission &result) {
        // Identity and HMAC updates can restart discovery while the OS prompt is
        // open. Keep one prompt and apply its result to the latest live record.
        d->permissionPending = false;
        if (!d->requested || d->bootstrap.isEmpty()) return;
        if (result.status() == Qt::PermissionStatus::Granted) d->begin();
        else d->setState("unavailable: Bluetooth permission denied");
    });
#else
    d->setState("unavailable: built without Bluetooth");
#endif
}
void BleDiscovery::stop() {
    ++d->generation; d->requested = false;
#ifdef IISOCIETYSYNC_BLE
    d->scanTimer.stop(); d->connectionTimer.stop(); d->queue.clear(); d->visited.clear();
    if (d->scanner) { d->scanner->disconnect(this); d->scanner->stop(); delete d->scanner; d->scanner = nullptr; }
    if (d->central) { auto *old = d->central.data(); d->central = nullptr; old->disconnect(this); old->disconnectFromDevice(); old->deleteLater(); }
    if (d->peripheral) { d->peripheral->disconnect(this); d->peripheral->stopAdvertising(); delete d->peripheral; d->peripheral = nullptr; }
#endif
    d->identity = {}; d->invitationPort = 0; d->bootstrap.clear(); d->setState("stopped");
}
}
