#include "iiSocietySync.h"
#include "TestLink.h"
#include <SocietyDrive.h>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace iiSocietySync;
class ControllerTests : public QObject {
    Q_OBJECT
private slots:
    void processHandoffDrainsAnActiveFileScan() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/handoff-XXXXXX");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        QFile file(root.filePath("Models/large")); QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(1024LL * 1024 * 1024)); file.close();
        Controller sync({}); const QString scope(64, 'a'); sync.open(root.path(), scope); QTRY_VERIFY(sync.available());
        sync.setPeers({"trusted"}, {});
        const QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "changes"}, {"container", drive->identifier()}}}};
        QVERIFY(sync.handle("trusted", request).value("pending").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath(".society-sync/operation.lock")), 5000);
        sync.closeAndWait(); QVERIFY(!sync.available());
        QVERIFY(!QFileInfo::exists(root.filePath(".society-sync/operation.lock")));
        Replica successor; QVERIFY2(successor.open(root.path(), scope), qPrintable(successor.errorString()));
        QVERIFY(!sync.handle("trusted", request).value("ok").toBool());
    }
    void filesProjectionSupportsCreateReadAndPaginationWithoutExposingOtherSections() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/files-service-XXXXXX"); QVERIFY(root.isValid());
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        QFile model(root.filePath("Models/private")); QVERIFY(model.open(QIODevice::WriteOnly)); model.write("model"); model.close();
        const auto files = filesHandler(root.path());
        QVERIFY(files("peer", {{"op", "list"}, {"path", ""}}).value("entries").toArray().isEmpty());
        QVERIFY(!files("peer", {{"op", "list"}, {"path", "../Models"}}).value("ok").toBool());
        QVERIFY(!files("peer", {{"op", "stat"}, {"path", QJsonArray{}}}).value("ok").toBool());
        QVERIFY(!files("peer", {{"op", "list"}, {"cursor", "-1"}}).value("ok").toBool());
        QVERIFY(files("peer", {{"op", "mkdir"}, {"path", "folder"}}).value("ok").toBool());
        QVERIFY(!files("peer", {{"op", "mkdir"}, {"path", "folder"}}).value("ok").toBool());
        QVERIFY(!files("peer", {{"op", "write"}, {"path", "missing/child"}, {"data", "Ynl0ZXM="}}).value("ok").toBool());
        QVERIFY(files("peer", {{"op", "write"}, {"path", "folder/item"}, {"data", "Ynl0ZXM="}}).value("ok").toBool());
        QVERIFY(!files("peer", {{"op", "write"}, {"path", "folder/item"}, {"data", "bmV3"}}).value("ok").toBool());
        const auto metadata = files("peer", {{"op", "stat"}, {"path", "folder/item"}});
        QVERIFY(metadata.value("ok").toBool()); QCOMPARE(metadata.value("size").toString(), "5");
        const auto data = files("peer", {{"op", "read"}, {"path", "folder/item"}, {"version", metadata.value("version")}, {"offset", "1"}});
        QVERIFY(data.value("ok").toBool()); QCOMPARE(QByteArray::fromBase64(data.value("data").toString().toLatin1()), QByteArray("ytes"));
        QVERIFY(data.value("eof").toBool());
        QVERIFY(!files("peer", {{"op", "read"}, {"path", "folder/item"}, {"version", "old"}}).value("ok").toBool());
        QVERIFY(nativeTestLink(root.filePath("Models/private"), root.filePath("Files/link")));
        QVERIFY(!files("peer", {{"op", "read"}, {"path", "link"}}).value("ok").toBool());
        for (int i = 0; i < 260; ++i) {
            QFile file(root.filePath("Files/" + QString::number(i))); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("data");
        }
        auto page = files("peer", {{"op", "list"}, {"path", ""}}); QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("entries").toArray().size(), 256); QVERIFY(!page.value("nextCursor").toString().isEmpty());
        auto next = files("peer", {{"op", "list"}, {"path", ""}, {"cursor", page.value("nextCursor")}});
        QCOMPARE(next.value("entries").toArray().size(), 5); QVERIFY(next.value("nextCursor").toString().isEmpty());
        for (const auto &entry : page.value("entries").toArray() + next.value("entries").toArray())
            QVERIFY(entry.toObject().value("name").toString() != "link");
        QVERIFY(removeNativeTestLink(root.filePath("Files/link")));
    }
    void endpointRequiresTheAuthenticatedPeerAndRejectsTokenMutation() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/endpoint-XXXXXX"); QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        Controller sync({}); sync.open(root.path(), QString(64, 'a')); QTRY_VERIFY(sync.available());
        QJsonObject message{{"op", "society.sync"}, {"protocol", 2}, {"scope", QString(64, 'a')},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"message", QJsonObject{{"action", "changes"}, {"container", iiSocietyContainer::SocietyDrive::open(root.path())->identifier()}}}};
        QVERIFY(!sync.handle("other", message).value("ok").toBool());
        sync.setPeers({"trusted"}, {});
        auto otherScope = message; otherScope.insert("scope", QString(64, 'b'));
        QVERIFY(!sync.handle("trusted", otherScope).value("ok").toBool());
        QVERIFY(sync.handle("trusted", message).value("pending").toBool());
        QTRY_VERIFY(sync.handle("trusted", message).value("result").toObject().value("ok").toBool());
        auto changed = message; changed.insert("message", QJsonObject{{"action", "changes"}, {"container", iiSocietyContainer::SocietyDrive::open(root.path())->identifier()}, {"after", "1"}});
        QCOMPARE(sync.handle("trusted", changed).value("error").toString(), "different_request_token_reuse");
        sync.close(); QVERIFY(!sync.available()); QVERIFY(!sync.handle("trusted", message).value("ok").toBool());
    }
    void aClientSynchronizesBothDirectionsOverRealLocalTls() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/tls-host-XXXXXX"), mobile(SYNC_TEST_DIRECTORY "/tls-client-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(mobile.path()));
        QFile file(root.filePath("Models/host.bin")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray(700000, 'h')); file.close();
        QFile local(mobile.filePath("Files/mobile.txt")); QVERIFY(local.open(QIODevice::WriteOnly)); local.write("mobile edit"); local.close();
        iiServerHost::LanPeer host, client;
        Controller hosting({}), syncing([&](const auto &peer, const auto &message) { return client.request(peer, message); });
        const auto files = filesHandler(root.path());
        QVERIFY(host.startHost("desktop", "Desktop", [&](const auto &peer, const auto &message) {
            return message.value("op") == "society.sync" ? hosting.handle(peer, message) : files(peer, message);
        }, {"127.0.0.1"}, QHostAddress::LocalHost));
        QVERIFY2(client.join(host.createOffer(), "phone", "Phone"), qPrintable(client.errorString()));
        QTRY_VERIFY2(client.connected(), qPrintable(client.errorString()));
        hosting.open(root.path(), QString(64, 'a')); syncing.open(mobile.path(), QString(64, 'a'));
        QTRY_VERIFY(hosting.available() && syncing.available());
        // The product obtains these peers from account-authenticated pairing.
        // This transport test supplies an isolated synthetic authorization set.
        hosting.setPeers({"phone"}, {});
        connect(&client, &iiServerHost::LanPeer::completed, &syncing, [&](auto id, auto result, auto transport) {
            QCOMPARE(transport, "local"); syncing.receive(id, result);
        });
        QSignalSpy completed(&syncing, &Controller::synchronized);
        syncing.setPeers({"desktop"}, {"desktop"});
        QTRY_VERIFY2_WITH_TIMEOUT(completed.size() > 0, qPrintable(syncing.errorString()), 30000);
        QFile copy(mobile.filePath("Models/host.bin")); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), QByteArray(700000, 'h'));
        QCOMPARE(iiSocietyContainer::SocietyDrive::open(mobile.path())->identifier(), iiSocietyContainer::SocietyDrive::open(root.path())->identifier());
        QVERIFY(!QFileInfo::exists(root.filePath("Files/mobile.txt")));
        QVERIFY(local.open(QIODevice::WriteOnly)); local.write("mobile edit"); local.close();
        completed.clear();
        // A local filesystem edit must propagate without a UI/manual sync call.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(root.filePath("Files/mobile.txt")), 1800);
        QFile uploaded(root.filePath("Files/mobile.txt")); QVERIFY(uploaded.open(QIODevice::ReadOnly)); QCOMPARE(uploaded.readAll(), QByteArray("mobile edit"));
        uploaded.close();
        QVERIFY(local.open(QIODevice::WriteOnly | QIODevice::Truncate)); local.write("same file changed"); local.close();
        QTRY_VERIFY_WITH_TIMEOUT(([&] { QFile f(root.filePath("Files/mobile.txt")); return f.open(QIODevice::ReadOnly) && f.readAll() == "same file changed"; })(), 1800);
        QFile hostEdit(root.filePath("Files/host-edit")); QVERIFY(hostEdit.open(QIODevice::WriteOnly)); hostEdit.write("host event"); hostEdit.close();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(mobile.filePath("Files/host-edit")), 1800);
        QVERIFY(!client.hosting());
        QVERIFY(!files("phone", {{"op", "list"}, {"path", "../Models"}}).value("ok").toBool());
        syncing.close(); hosting.close(); client.stop(); host.stop();
    }
};
QTEST_GUILESS_MAIN(ControllerTests)
#include "controller.moc"
