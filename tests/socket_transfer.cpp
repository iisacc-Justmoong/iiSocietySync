#include "iiSocietySync.h"
#include <LanPeer.h>
#include <SocietyDrive.h>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QCryptographicHash>

using namespace iiSocietySync;
class SocketTransferTests : public QObject {
    Q_OBJECT
    static QByteArray digest(const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256); if (!hash.addData(&file)) return {}; return hash.result();
    }
    static void put(const QString &path, const QByteArray &bytes) {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size());
    }
private slots:
    void tlsBinaryMirrorResumeUploadAndRevocation() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/socket-client-XXXXXX"), b(SYNC_TEST_DIRECTORY "/socket-host-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        QByteArray bytes(8 * 1024 * 1024 + 17, Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = char((i * 37 + i / 251) % 253);
        put(b.filePath("Models/bulk"), bytes); const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        iiServerHost::LanPeer hostPeer, clientPeer;
        Controller host({}), client([&](const auto &peer, const auto &request) { return clientPeer.request(peer, request); });
        client.setContentPolicy(Synchronizer::ContentPolicy::FullReplica);
        connect(&clientPeer, &iiServerHost::LanPeer::completed, &client, [&](const auto &id, const auto &result, auto) { client.receive(id, result); });
        const QString scope(64, 'a'); host.open(b.path(), scope); client.open(a.path(), scope);
        QTRY_VERIFY(host.available() && client.available()); host.setPeers({"phone"}, {});
        QVERIFY(hostPeer.startHost("desktop", "Desktop", [&](const auto &peer, const auto &request) {
            return request.value("op") == "society.sync" ? host.handle(peer, request) : QJsonObject{{"ok", true}, {"entries", QJsonArray()}};
        }, {"127.0.0.1"}, QHostAddress::LocalHost));
        QVERIFY(clientPeer.join(hostPeer.createOffer(), "phone", "Phone")); QTRY_VERIFY(clientPeer.connected()); QVERIFY(clientPeer.binaryTransferEnabled());
        bool stopped = false;
        const auto stop = connect(&client, &Controller::progress, &client, [&](auto, qint64 done, auto) {
            if (!stopped && done >= Replica::ChunkBytes) { stopped = true; client.close(); }
        });
        client.setPeers({"desktop"}, {"desktop"}); QTRY_VERIFY_WITH_TIMEOUT(stopped, 10000); disconnect(stop);
        client.closeAndWait(); QVERIFY(!QFileInfo::exists(a.filePath("Models/bulk")));
        client.open(a.path(), scope); QTRY_VERIFY(client.available());
        QSignalSpy synchronized(&client, &Controller::synchronized); QElapsedTimer clock; clock.start();
        client.setPeers({"desktop"}, {"desktop"});
        QTRY_VERIFY_WITH_TIMEOUT(!synchronized.isEmpty(), 30000);
        QCOMPARE(digest(a.filePath("Models/bulk")), expected);
        qInfo("TCP/TLS binary resumed download bytes=%lld elapsed_ms=%lld sha256=%s", qlonglong(bytes.size()), clock.elapsed(), expected.toHex().constData());
        put(a.filePath("Files/upload"), bytes); synchronized.clear(); clock.restart(); client.synchronizeNow();
        QTRY_COMPARE_WITH_TIMEOUT(digest(b.filePath("Files/upload")), expected, 30000);
        qInfo("TCP/TLS binary upload bytes=%lld elapsed_ms=%lld sha256=%s", qlonglong(bytes.size()), clock.elapsed(), expected.toHex().constData());
        host.setPeers({}, {});
        const auto denied = host.handle("phone", {{"op", "society.sync"}, {"protocol", 2}, {"scope", scope}});
        QVERIFY(!denied.value("ok").toBool());
        client.closeAndWait(); host.closeAndWait();
    }
};
QTEST_GUILESS_MAIN(SocketTransferTests)
#include "socket_transfer.moc"
