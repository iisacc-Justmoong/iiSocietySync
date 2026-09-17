#include "Synchronizer.h"
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QImage>
using namespace iiSocietySync;
static void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size());
}
class OnDemandTests : public QObject {
    Q_OBJECT
    void cycle(Replica &client, Replica &host, QStringList &reads) {
        Synchronizer sync(&client); QVERIFY(sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst));
        QSignalSpy done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject();
            if (request.value("action") == "read") reads.append(request.value("entry").toObject().value("path").toString());
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", request)}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QVERIFY2(done[0][1].toBool(), qPrintable(done[0][2].toString()));
    }
private slots:
    void metadataIsVisibleWhileAnUnrelatedConflictWaitsForBytes() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/metadata-priority-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/metadata-priority-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        put(b.filePath("Models/new.bin"), QByteArray(800123, 'm'));
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read") { waiting = true; return; }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        QVERIFY(sync.start("host")); QTRY_VERIFY_WITH_TIMEOUT(waiting, 3000);
        QCOMPARE(map.object("models/new.bin").value("hash"), host.record("models/new.bin").value("hash"));
        QVERIFY(!map.object("models/new.bin").value("resident").toBool());
        QVERIFY(!QFileInfo::exists(a.filePath("Models/new.bin")));
        sync.stop();
        QFile pending(a.filePath("Files/backlog.bin")); QVERIFY(pending.open(QIODevice::ReadOnly));
        QCOMPARE(pending.readAll(), QByteArray("pending phone edit"));
    }
    void selectedDownloadFinishesBeforeUnrelatedConflictReconciliation() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/request-priority-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/request-priority-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QByteArray model(800123, 'm'); put(b.filePath("Models/selected.bin"), model);
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto request = map.request({"models/selected.bin"}); QVERIFY(!request.isEmpty());
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        QSignalSpy finished(&sync, &Synchronizer::finished); bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read" && message.value("entry").toObject().value("path") == "files/backlog.bin") {
                waiting = true; return;
            }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(map.requestState(request).value("state"), "ready", 3000);
        QTRY_VERIFY_WITH_TIMEOUT(waiting, 3000); QVERIFY(finished.isEmpty()); QVERIFY(sync.busy());
        QVERIFY(map.available({"models/selected.bin"}));
        QFile downloaded(a.filePath("Models/selected.bin")); QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), model); sync.stop();
    }
    void newUploadIsAcknowledgedBeforeUnrelatedConflictReconciliation() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/upload-priority-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/upload-priority-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        put(a.filePath("Files/backlog.bin"), "pending phone edit");
        put(b.filePath("Files/backlog.bin"), "host version");
        put(a.filePath("Generation History/new.png"), QByteArray(100123, 'g'));
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        QSignalSpy finished(&sync, &Synchronizer::finished); bool waiting = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto message = wire.value("message").toObject();
            if (message.value("action") == "read" && message.value("entry").toObject().value("path") == "files/backlog.bin") {
                waiting = true; return;
            }
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", message)}});
        });
        QVERIFY(sync.start("host"));
        QTRY_VERIFY_WITH_TIMEOUT(map.object("generation-history/new.png").contains("revision"), 3000);
        QCOMPARE(map.object("generation-history/new.png").value("revision"), host.record("generation-history/new.png").value("revision"));
        QTRY_VERIFY_WITH_TIMEOUT(waiting, 3000); QVERIFY(finished.isEmpty()); QVERIFY(sync.busy());
        QFile uploaded(b.filePath("Generation History/new.png")); QVERIFY(uploaded.open(QIODevice::ReadOnly));
        QCOMPARE(uploaded.readAll(), QByteArray(100123, 'g')); sync.stop();
    }
    void previewBatchesYieldToSelectedDownloads() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/preview-batch-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/preview-batch-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        for (int index = 0; index < 20; ++index) {
            QImage image(64, 64, QImage::Format_RGB32); image.fill(QColor(index * 10, 0, 0));
            QVERIFY(image.save(b.filePath(QString("Generation History/image-%1.png").arg(index))));
        }
        put(b.filePath("Models/selected.bin"), QByteArray(500000, 'm'));
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads); QVERIFY(reads.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto countPreviews = [&] {
            int count = 0;
            for (const auto &value : map.objects()) if (!map.previewPath(value.toObject()).isEmpty()) ++count;
            return count;
        };
        QCOMPARE(countPreviews(), 16);
        const auto request = map.request({"models/selected.bin"}); QVERIFY(!request.isEmpty());
        cycle(phone, host, reads);
        QCOMPARE(map.requestState(request).value("state"), "ready");
        QVERIFY(!reads.isEmpty());
        for (const auto &path : reads) QCOMPARE(path, "models/selected.bin");
        QCOMPARE(countPreviews(), 20);
        QVERIFY(!QFileInfo::exists(a.filePath("Generation History/image-0.png")));
    }
    void cancellationDuringTransferLeavesNoPublishedOriginal() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/cancel-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/cancel-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/large.bin"), QByteArray(4 * 1024 * 1024, 'm'));
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto request = map.request({"models/large.bin"}); QVERIFY(!request.isEmpty());
        Synchronizer sync(&phone); sync.setContentPolicy(Synchronizer::ContentPolicy::MetadataFirst);
        QSignalSpy done(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle("phone", wire.value("message").toObject())}});
        });
        connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) { QVERIFY(map.cancel(request)); });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QVERIFY(done.first()[1].toBool()); QCOMPARE(map.requestState(request).value("state"), "cancelled");
        QVERIFY(!QFileInfo::exists(a.filePath("Models/large.bin")));
        const auto resumed = map.request({"models/large.bin"}); cycle(phone, host, reads);
        QCOMPARE(map.requestState(resumed).value("state"), "ready"); QVERIFY(map.available({"models/large.bin"}));
    }
    void previewsAndCancellationDoNotFetchOriginals() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/preview-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/preview-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        QImage original(1024, 768, QImage::Format_RGB32); original.fill(Qt::red);
        QVERIFY(original.save(b.filePath("Generation History/large.png")));
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads); QVERIFY(reads.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto entry = map.object("generation-history/large.png");
        const QImage preview(map.previewPath(entry)); QVERIFY(!preview.isNull());
        QVERIFY(preview.width() <= 256 && preview.height() <= 256);
        QVERIFY(!QFileInfo::exists(a.filePath("Generation History/large.png")));
        const auto id = map.request({"generation-history/large.png"}); QVERIFY(map.cancel(id));
        cycle(phone, host, reads); QVERIFY(reads.isEmpty());
        QCOMPARE(map.requestState(id).value("state"), "cancelled");
        QVERIFY(!map.available({"generation-history/large.png"}));
    }
    void metadataThenSelectedModelAndGeneratedUpload() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/demand-phone-XXXXXX"), b(SYNC_TEST_DIRECTORY "/demand-host-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/Checkpoint/selected.safetensors"), QByteArray(800123, 'm'));
        put(b.filePath("Models/Checkpoint/unselected.safetensors"), QByteArray(1600555, 'x'));
        put(b.filePath("Files/Documents/readme.txt"), "host document");
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        QVERIFY(reads.isEmpty()); QVERIFY(!phone.bootstrapping());
        QVERIFY(!QFileInfo::exists(a.filePath("Models/Checkpoint/selected.safetensors")));
        QVERIFY(phone.scan()); QVERIFY(phone.changes().value("entries").toArray().isEmpty());
        auto drive = iiSocietyContainer::SocietyDrive::open(a.path()); QVERIFY(drive && drive->isReady());
        iiSocietyContainer::StorageMap map(*drive);
        QCOMPARE(map.object("models/Checkpoint/selected.safetensors").value("kind"), "file");
        const auto request = map.request({"models/Checkpoint/selected.safetensors"}); QVERIFY(!request.isEmpty());
        cycle(phone, host, reads); QVERIFY(!reads.isEmpty());
        for (const auto &path : reads) QCOMPARE(path, "models/Checkpoint/selected.safetensors");
        QCOMPARE(map.requestState(request).value("state"), "ready");
        QVERIFY(map.available({"models/Checkpoint/selected.safetensors"}));
        QVERIFY(!QFileInfo::exists(a.filePath("Models/Checkpoint/unselected.safetensors")));
        // A completed local image is a durable proposal and uploads even though
        // all unrelated host payloads remain remote-only.
        put(a.filePath("Generation History/generated.png"), "completed image fixture");
        reads.clear(); cycle(phone, host, reads);
        QVERIFY(QFileInfo::exists(b.filePath("Generation History/generated.png")));
        QVERIFY(phone.record("generation-history/generated.png").contains("revision"));
        QVERIFY(reads.isEmpty());
        // Closing/reopening does not turn remote objects into deletions.
        phone.close(); QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(phone.scan());
        QVERIFY(phone.changes().value("entries").toArray().isEmpty());
    }
    void staleRequestAndRemoteDeletion() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/demand-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/demand-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Models/model.bin"), "old");
        Replica phone, host; QVERIFY(phone.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QStringList reads; cycle(phone, host, reads);
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto id = map.request({"models/model.bin"}); QVERIFY(!id.isEmpty());
        put(b.filePath("Models/model.bin"), "new version"); cycle(phone, host, reads);
        QCOMPARE(map.requestState(id).value("state"), "failed");
        QVERIFY(!map.available({"models/model.bin"}));
        const auto next = map.request({"models/model.bin"}); cycle(phone, host, reads);
        QCOMPARE(map.requestState(next).value("state"), "ready");
        QVERIFY(QFile::remove(b.filePath("Models/model.bin"))); cycle(phone, host, reads);
        QCOMPARE(map.object("models/model.bin").value("kind"), "deleted");
        QVERIFY(!QFileInfo::exists(a.filePath("Models/model.bin")));
    }
};
QTEST_GUILESS_MAIN(OnDemandTests)
#include "on_demand.moc"
