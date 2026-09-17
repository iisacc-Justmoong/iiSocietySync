#include "iiSocietySync.h"
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>
#include <QTcpServer>
#include <QScopeGuard>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

using namespace iiSocietySync;
namespace {
void put(const QString &path, const QByteArray &bytes) {
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}
const QString scope(64, 'a');
}
class IndexingTests : public QObject {
    Q_OBJECT
private slots:
    void watchingALargeLibraryLeavesRoomForFilesAndSockets() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX descriptor limit regression");
#else
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/watch-budget-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        for (int i = 0; i < 300; ++i) put(root.filePath(QString("Files/item-%1").arg(i)), "small");
        struct rlimit original{}; QVERIFY(getrlimit(RLIMIT_NOFILE, &original) == 0);
        auto limited = original; limited.rlim_cur = qMin<rlim_t>(256, original.rlim_cur);
        QVERIFY(setrlimit(RLIMIT_NOFILE, &limited) == 0);
        const auto restore = qScopeGuard([&] { setrlimit(RLIMIT_NOFILE, &original); });
        Controller controller({}); controller.open(root.path(), scope); QTRY_VERIFY(controller.available());
        QFile input(root.filePath("Files/item-0"));
        QVERIFY2(input.open(QIODevice::ReadOnly), qPrintable(input.errorString()));
        QCOMPARE(input.readAll(), QByteArray("small"));
        QTcpServer socket; QVERIFY2(socket.listen(QHostAddress::LocalHost), qPrintable(socket.errorString()));
        put(root.filePath("Files/item-299"), "updated outside a small watch set");
        controller.synchronizeNow();
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(root.path()));
        const auto hash = QString::fromLatin1(QCryptographicHash::hash("updated outside a small watch set", QCryptographicHash::Sha256).toHex());
        QTRY_COMPARE(map.object("files/item-299").value("hash").toString(), hash);
        controller.closeAndWait();
#endif
    }
    void openingWaitsForABriefConcurrentIndexCommit() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/index-open-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        Replica replica; QVERIFY(replica.open(root.path(), scope)); replica.close();
        QSemaphore entered; bool locked = false;
        std::jthread writer([&] {
            QLockFile lock(root.filePath(".society-sync/operation.lock")); lock.setStaleLockTime(0);
            locked = lock.tryLock(); entered.release(); QThread::msleep(100);
        });
        entered.acquire();
        const bool opened = replica.open(root.path(), scope);
        writer.join(); QVERIFY(locked); QVERIFY2(opened, qPrintable(replica.errorString()));
    }
    void smallObjectsAreDurableAndReadableWhileLargeHashIsCancelled() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/index-cancel-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/previous"), "previous");
        Replica indexer, reader; QVERIFY(indexer.open(root.path(), scope)); QVERIFY(indexer.scan());
        QVERIFY(reader.open(root.path(), scope));
        QVERIFY(QFile::remove(root.filePath("Files/previous")));
        put(root.filePath("Files/small"), "small metadata first");
        put(root.filePath("Photos/thumbnail.jpg"), "thumbnail");
        put(root.filePath("Models/large"), QByteArray(4 * 1024 * 1024, 'm'));
        bool cancelled = false, readableDuringHash = false, smallVisible = false, previewVisible = false;
        indexer.setCancellation([&] { return cancelled; });
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) {
            if (path != "Models/large") return;
            if (!done) {
                readableDuringHash = reader.changes().value("ok").toBool();
                smallVisible = reader.record("files/small").value("kind") == "file";
                previewVisible = reader.record("photos/thumbnail.jpg").value("kind") == "file";
            } else cancelled = true;
        });
        QVERIFY(!indexer.scan()); QCOMPARE(indexer.errorString(), QString("cancelled"));
        QVERIFY2(readableDuringHash, "Hashing held the replication operation lock");
        QVERIFY2(smallVisible && previewVisible, "Small objects waited behind the large model");
        QVERIFY(reader.record("models/large").isEmpty());
        QCOMPARE(reader.record("files/previous").value("kind"), "file"); // No deletion from an unfinished pass.
        const auto revision = reader.record("files/small").value("revision");
        indexer.close(); cancelled = false; QVERIFY(indexer.open(root.path(), scope));
        QStringList rehashed;
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) { if (!done) rehashed.append(path); });
        QVERIFY2(indexer.scan(), qPrintable(indexer.errorString()));
        QCOMPARE(rehashed, QStringList{"Models/large"});
        QCOMPARE(reader.record("files/small").value("revision"), revision);
        QCOMPARE(reader.record("files/previous").value("kind"), "deleted");
    }
    void recreationDuringAHashDoesNotBecomeAFalseDeletion() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/index-recreate-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        put(root.filePath("Files/recreated"), "old");
        Replica indexer; QVERIFY(indexer.open(root.path(), scope)); QVERIFY(indexer.scan());
        QVERIFY(QFile::remove(root.filePath("Files/recreated")));
        put(root.filePath("Models/large"), QByteArray(2 * 1024 * 1024, 'm'));
        bool recreated = false;
        indexer.setVerificationProgress([&](const QString &path, qint64 done, qint64) {
            if (path == "Models/large" && !done && !recreated) {
                put(root.filePath("Files/recreated"), "revived"); recreated = true;
            }
        });
        QVERIFY(indexer.scan()); QVERIFY(recreated);
        QCOMPARE(indexer.record("files/recreated").value("kind"), "file");
        QVERIFY(indexer.scan());
        QCOMPARE(indexer.record("files/recreated").value("hash").toString(),
                 QString::fromLatin1(QCryptographicHash::hash("revived", QCryptographicHash::Sha256).toHex()));
    }
    void controllerServesCommittedMetadataBeforeLargeHashCompletes() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/index-responsive-XXXXXX");
        const auto drive = iiSocietyContainer::SocietyDrive::create(root.path()); QVERIFY(drive);
        put(root.filePath("Files/small"), "available during model indexing");
        QFile model(root.filePath("Models/large")); QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.resize(2LL * 1024 * 1024 * 1024)); model.close();
        Controller host({}); bool modelStarted = false, modelFinished = false;
        connect(&host, &Controller::changed, this, [&] {
            if (!host.errorString().isEmpty()) qInfo() << "Host indexing status:" << host.errorString();
        });
        connect(&host, &Controller::verificationProgress, this, [&](const QString &path, qint64 done, qint64 total) {
            if (path != "Models/large") return;
            modelStarted = true; if (done == total) modelFinished = true;
        });
        host.open(root.path(), scope); QTRY_VERIFY(host.available()); host.setPeers({"phone"}, {});
        host.synchronizeNow(); QTRY_VERIFY2_WITH_TIMEOUT(modelStarted, qPrintable(host.errorString()), 5000);
        const QJsonObject request{{"op", "society.sync"}, {"protocol", 2}, {"namespaceVersion", 1}, {"scope", scope},
            {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"message", QJsonObject{{"action", "changes"}, {"container", drive->identifier()}}}};
        QElapsedTimer elapsed; elapsed.start(); QJsonObject response;
        do {
            response = host.handle("phone", request).value("result").toObject();
            if (!response.isEmpty()) break;
            QTest::qWait(1);
        } while (elapsed.elapsed() < 3000);
        QVERIFY2(response.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
        QVERIFY2(!modelFinished, "Metadata response waited for all model bytes to be hashed");
        qInfo() << "Metadata response milliseconds during a 2 GiB model hash:" << elapsed.elapsed();
        bool found = false;
        for (const auto &item : response.value("entries").toArray())
            if (item.toObject().value("path") == "files/small") found = true;
        QVERIFY(found);
        host.closeAndWait();
        QVERIFY(!QFileInfo::exists(root.filePath(".society-sync/operation.lock")));
    }
};
QTEST_GUILESS_MAIN(IndexingTests)
#include "indexing.moc"
