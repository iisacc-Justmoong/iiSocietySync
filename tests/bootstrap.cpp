#include "NearbyBootstrap.h"
#include "BleDiscovery.h"
#include <QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
using namespace iiSocietySync;
class BootstrapTests : public QObject {
    Q_OBJECT
    NearbyBootstrap record() const {
        return {{{"v", "1"}, {"scope", QString(64, 'a')}, {"id", "desktop"}, {"nonce", "nonce"},
            {"name", QString::fromUtf8("작업실")}, {"kind", "pc"}, {"host", "1"}}, {"192.168.1.2", "10.0.0.1"}, 12345};
    }
private slots:
    void pagesRoundTripAndDetectMixedRefresh() {
        const auto original = record(); const auto bytes = original.encode(); QVERIFY(!bytes.isEmpty());
        QByteArray joined;
        for (int page = 0; page < 4; ++page) joined += bytes.mid(page * 500, 500);
        const auto decoded = NearbyBootstrap::decode(joined); QVERIFY(decoded);
        QCOMPARE(decoded->record, original.record); QCOMPARE(decoded->addresses, original.addresses); QCOMPARE(decoded->port, original.port);
        joined[joined.size() - 10] ^= 1; QVERIFY(!NearbyBootstrap::decode(joined));
        QVERIFY(!NearbyBootstrap::decode(bytes + ' ')); QVERIFY(!NearbyBootstrap::decode(bytes.left(32)));
    }
    void rejectsCredentialsFilesAndPublicEndpoints() {
        for (const auto &field : {"cookie", "secret", "keys", "data", "files", "password"}) {
            auto value = record(); value.record.insert(field, "must never be advertised"); QVERIFY(value.encode().isEmpty());
        }
        for (const auto &address : {"8.8.8.8", "127.0.0.1", "169.254.169.254", "iisacc.com", "0.0.0.0"}) {
            auto value = record(); value.addresses = {address}; QVERIFY(value.encode().isEmpty());
        }
        auto value = record(); value.port = 0; QVERIFY(value.encode().isEmpty());
        value = record(); value.record.insert("name", QString(129, 'x')); QVERIFY(value.encode().isEmpty());
        QVERIFY(!NearbyBootstrap::decode(QByteArray(2001, 'x')));
    }
    void digestDoesNotAuthorizeUntrustedMetadata() {
        auto value = record(); value.record.insert("proof", QString(64, '0'));
        const auto decoded = NearbyBootstrap::decode(value.encode()); QVERIFY(decoded);
        QCOMPARE(decoded->record.value("proof").toString(), QString(64, '0')); // Consumer must verify HMAC.
    }
    void stopIsIdempotentWithoutStartingRadio() {
        BleDiscovery discovery; QCOMPARE(discovery.status(), "stopped"); discovery.stop(); discovery.stop();
        discovery.start({}, 0); QVERIFY(discovery.status().startsWith("unavailable"));
        discovery.stop(); QCOMPARE(discovery.status(), "stopped");
    }
};
QTEST_GUILESS_MAIN(BootstrapTests)
#include "bootstrap.moc"
