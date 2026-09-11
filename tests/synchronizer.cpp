#include "Synchronizer.h"
#include <SocietyDrive.h>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace iiSocietySync;
static void put(const QString &path, QByteArray data) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size()); }
static QByteArray get(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
class SyncTests : public QObject {
    Q_OBJECT
    void cycle(Replica &client, Replica &host, QList<QJsonObject> *requests = nullptr) {
        Synchronizer sync(&client); QSignalSpy completed(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](const QString &id, const QString &, const QJsonObject &wire) {
            if (requests) requests->append(wire.value("message").toObject());
            QCOMPARE(wire.value("scope").toString(), QString(64, 'a'));
            const auto reply = host.handle("client", wire.value("message").toObject());
            sync.receive(id, {{"ok", true}, {"result", reply}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 20000);
        QVERIFY2(completed[0][1].toBool(), qPrintable(completed[0][2].toString()));
    }
private slots:
    void firstJoinAdoptsOneLogicalDriveAndPreservesFormerContentsPrivately() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/mirror-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/mirror-b-XXXXXX"), c(SYNC_TEST_DIRECTORY "/mirror-c-XXXXXX");
        const auto old = iiSocietyContainer::SocietyDrive::create(a.path()); QVERIFY(old);
        QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(c.path()));
        for (const auto section : iiSocietyContainer::allStoreSections()) {
            const auto name = iiSocietyContainer::storeSectionName(section);
            put(a.filePath(name + "/legacy/old"), "private old content");
            put(b.filePath(name + "/from-host/nested/item"), name.toUtf8());
        }
        Replica client, host, second;
        QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a'))); QVERIFY(second.open(c.path(), QString(64, 'a')));
        cycle(client, host); cycle(second, host);
        QCOMPARE(client.containerId(), host.containerId()); QCOMPARE(second.containerId(), host.containerId());
        QVERIFY(client.replicaId() != host.replicaId()); QVERIFY(second.replicaId() != client.replicaId());
        QVERIFY(!old->isValid()); QVERIFY(!client.bootstrapping());
        const auto binding = Replica::binding(a.path()); QVERIFY(binding.value("complete").toBool());
        QCOMPARE(binding.value("container").toString(), host.containerId());
        for (const auto section : iiSocietyContainer::allStoreSections()) {
            const auto name = iiSocietyContainer::storeSectionName(section);
            for (const auto &path : {a.path(), c.path()}) {
                QCOMPARE(get(QDir(path).filePath(name + "/from-host/nested/item")), name.toUtf8());
                QVERIFY(!QFileInfo::exists(QDir(path).filePath(name + "/legacy")));
            }
            QVERIFY(!QFileInfo::exists(b.filePath(name + "/legacy")));
            QCOMPARE(get(a.filePath(binding.value("recovery").toString() + '/' + name + "/legacy/old")), QByteArray("private old content"));
        }
        const auto deviceReplica = client.replicaId(); client.close(); QVERIFY(client.open(a.path(), QString(64, 'a')));
        QCOMPARE(client.containerId(), host.containerId()); QCOMPARE(client.replicaId(), deviceReplica);
        put(a.filePath("Files/offline/new"), "new on phone"); cycle(client, host); cycle(second, host);
        QCOMPARE(get(c.filePath("Files/offline/new")), QByteArray("new on phone"));
        QVERIFY(QFile::remove(c.filePath("Files/offline/new"))); cycle(second, host); cycle(client, host);
        QVERIFY(!QFileInfo::exists(a.filePath("Files/offline/new"))); QVERIFY(!QFileInfo::exists(b.filePath("Files/offline/new")));
    }
    void interruptedInitialDownloadResumesWithoutUploadingLegacyFiles() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/bootstrap-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/bootstrap-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(a.filePath("Files/old"), "preserve me"); put(b.filePath("Models/large"), QByteArray(900000, 'h'));
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        {
            Synchronizer sync(&client); bool interrupted = false;
            connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) { interrupted = true; sync.stop(); });
            connect(&sync, &Synchronizer::requestReady, &sync, [&](const auto &id, auto, const auto &wire) {
                const auto message = wire.value("message").toObject();
                QVERIFY(message.value("action") != "chunk");
                sync.receive(id, {{"ok", true}, {"result", host.handle("client", message)}});
            });
            QVERIFY(sync.start("host")); QTRY_VERIFY(interrupted);
        }
        QVERIFY(client.bootstrapping()); QCOMPARE(client.containerId(), host.containerId());
        const auto recovery = client.binding().value("recovery").toString();
        client.close(); QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(client.bootstrapping());
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) if (r.value("action") == "read") QVERIFY(r.value("offset").toString().toLongLong() >= Replica::ChunkBytes);
        QCOMPARE(get(a.filePath("Models/large")), QByteArray(900000, 'h'));
        QVERIFY(!QFileInfo::exists(b.filePath("Files/old")));
        QCOMPARE(get(a.filePath(recovery + "/Files/old")), QByteArray("preserve me"));
        QVERIFY(!client.bootstrapping());
    }
    void recoveryResumesAfterIdentityCommitAndOnlySomeEntriesMoved() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/adoption-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/adoption-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(a.filePath("Files/one"), "one"); put(a.filePath("Models/two"), "two");
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        client.setCancellation([&] { return !QFileInfo::exists(a.filePath("Files/one")); });
        QVERIFY(!client.bindHost("host", host.replicaId(), host.containerId()));
        QVERIFY(iiSocietyContainer::SocietyDrive::open(a.path()));
        const auto recovery = Replica::binding(a.path()).value("recovery").toString(); QVERIFY(!recovery.isEmpty());
        client.close(); client.setCancellation({}); QVERIFY2(client.open(a.path(), QString(64, 'a')), qPrintable(client.errorString()));
        cycle(client, host); QCOMPARE(client.containerId(), host.containerId());
        QCOMPARE(get(a.filePath(recovery + "/Files/one")), QByteArray("one"));
        QCOMPARE(get(a.filePath(recovery + "/Models/two")), QByteArray("two"));
        QVERIFY(QDir(a.filePath("Files")).isEmpty()); QVERIFY(QDir(a.filePath("Models")).isEmpty());
    }
    void bootstrapCatchesHostChangesBeforePublishingTheMirror() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/catchup-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/catchup-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/changing"), "before");
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        Synchronizer sync(&client); QSignalSpy completed(&sync, &Synchronizer::finished); bool changed = false;
        connect(&sync, &Synchronizer::progress, &sync, [&](auto, auto, auto) {
            if (!changed) { changed = true; put(b.filePath("Files/changing"), "after"); put(b.filePath("Models/late"), "late"); }
        });
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle("client", wire.value("message").toObject())}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE(completed.size(), 1); QVERIFY(completed[0][1].toBool());
        QCOMPARE(get(a.filePath("Files/changing")), QByteArray("after")); QCOMPARE(get(a.filePath("Models/late")), QByteArray("late"));
        QVERIFY(!client.bootstrapping());
    }
    void aPrimaryClaimIsPersistentAndCannotPromoteAMirror() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/primary-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/primary-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QString scope(64, 'a');
        QVERIFY(Replica::claimPrimaryHost(b.path(), scope, "host")); QCOMPARE(Replica::primaryHost(b.path(), scope), QString("host"));
        QVERIFY(!Replica::claimPrimaryHost(b.path(), scope, "intruder")); QVERIFY(Replica::primaryHost(b.path(), QString(64, 'b')).isEmpty());
        Replica client, host; QVERIFY(client.open(a.path(), scope)); QVERIFY(host.open(b.path(), scope)); cycle(client, host);
        QVERIFY(!Replica::claimPrimaryHost(a.path(), scope, "client"));
    }
    void emptyHostAndHostDriveReplacementNeverMergeIndependentContents() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/replace-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/replace-b-XXXXXX"), c(SYNC_TEST_DIRECTORY "/replace-c-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(c.path()));
        put(a.filePath("Files/independent"), "legacy");
        Replica client, host, replacement; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); QVERIFY(QDir(a.filePath("Files")).isEmpty());
        put(a.filePath("Files/pending"), "offline edit for old drive"); put(c.filePath("Files/new-host"), "new drive");
        QVERIFY(replacement.open(c.path(), QString(64, 'a'))); cycle(client, replacement);
        QCOMPARE(client.containerId(), replacement.containerId());
        QCOMPARE(get(a.filePath("Files/new-host")), QByteArray("new drive"));
        QVERIFY(!QFileInfo::exists(c.filePath("Files/pending")));
        QCOMPARE(get(a.filePath(client.binding().value("recovery").toString() + "/Files/pending")), QByteArray("offline edit for old drive"));
        QVERIFY(!client.bindHost("different-host", host.replicaId(), host.containerId()));
        QCOMPARE(client.errorString(), QString("different_primary_host"));
    }
    void bothDirectionsDirectoriesDeletionAndIncrementalRequests() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/sync-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/sync-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/sub/client"), QByteArray(700000, 'c')); put(b.filePath("Thinking Space/host"), "host");
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); cycle(client, host);
        QCOMPARE(get(b.filePath("Files/sub/client")), QByteArray(700000, 'c')); QCOMPARE(get(a.filePath("Thinking Space/host")), QByteArray("host"));
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) QVERIFY(r.value("action") != "read" && r.value("action") != "chunk");
        put(b.filePath("Files/sub/client"), "edited on host"); cycle(client, host); QCOMPARE(get(a.filePath("Files/sub/client")), QByteArray("edited on host"));
        QVERIFY(QDir(a.filePath("Files/sub")).removeRecursively()); cycle(client, host); cycle(client, host);
        QVERIFY(!QFileInfo::exists(b.filePath("Files/sub"))); QVERIFY(!QFileInfo::exists(a.filePath("Files/sub")));
        // A newly joined device has never created the deleted parent. Old
        // tombstones must still be acknowledged so its first cycle can finish.
        QTemporaryDir c(SYNC_TEST_DIRECTORY "/late-join-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(c.path()));
        Replica late; QVERIFY(late.open(c.path(), QString(64, 'a'))); cycle(late, host);
        QVERIFY(!QFileInfo::exists(c.filePath("Files/sub")));
        QCOMPARE(get(c.filePath("Thinking Space/host")), QByteArray("host"));
    }
    void concurrentEditsAndDeleteAgainstEditKeepBothResults() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/conflict-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/conflict-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        put(b.filePath("Files/note"), "initial");
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        cycle(client, host); cycle(client, host);
        put(a.filePath("Files/note"), "client edit"); put(b.filePath("Files/note"), "host edit");
        cycle(client, host); cycle(client, host); cycle(client, host);
        QCOMPARE(get(a.filePath("Files/note")), get(b.filePath("Files/note")));
        const auto copies = QDir(a.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files);
        QCOMPARE(copies.size(), 1); QCOMPARE(QDir(b.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files), copies);
        const QSet<QByteArray> values{get(a.filePath("Files/note")), get(a.filePath("Files/" + copies[0]))};
        QCOMPARE(values, (QSet<QByteArray>{"client edit", "host edit"}));
        QVERIFY(QFile::remove(a.filePath("Files/note"))); put(b.filePath("Files/note"), "concurrent rescue");
        cycle(client, host); cycle(client, host);
        QCOMPARE(get(a.filePath("Files/note")), QByteArray("concurrent rescue")); QCOMPARE(get(b.filePath("Files/note")), QByteArray("concurrent rescue"));
    }
    void lostAcknowledgementAndRestartDoNotResendCompletedBytes() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/restart-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/restart-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        put(a.filePath("Models/large.bin"), QByteArray(900000, 'm'));
        {
            Synchronizer sync(&client); bool interrupted = false;
            connect(&sync, &Synchronizer::requestReady, &sync, [&](const QString &id, const QString &, const QJsonObject &wire) {
                const auto message = wire.value("message").toObject(); const auto reply = host.handle("client", message);
                if (message.value("action") == "chunk") { interrupted = true; sync.stop(); return; }
                sync.receive(id, {{"ok", true}, {"result", reply}});
            });
            QVERIFY(sync.start("host")); QTRY_VERIFY(interrupted);
        }
        client.close(); host.close(); QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        QList<QJsonObject> requests; cycle(client, host, &requests);
        for (const auto &r : requests) if (r.value("action") == "chunk") QVERIFY(r.value("offset").toString().toLongLong() >= Replica::ChunkBytes);
        QCOMPARE(get(b.filePath("Models/large.bin")), QByteArray(900000, 'm'));
    }
    void directoryAndLongFilenameConflictsPreserveData() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/types-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/types-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QString name(200, 'x');
        Replica client, host; QVERIFY(client.open(a.path(), QString(64, 'a'))); QVERIFY(host.open(b.path(), QString(64, 'a')));
        cycle(client, host);
        put(a.filePath("Files/" + name), "file variant");
        put(b.filePath("Files/" + name + "/child"), "directory child");
        cycle(client, host); cycle(client, host); cycle(client, host);
        QCOMPARE(get(a.filePath("Files/" + name + "/child")), QByteArray("directory child"));
        QCOMPARE(get(b.filePath("Files/" + name + "/child")), QByteArray("directory child"));
        const auto copies = QDir(a.filePath("Files")).entryList({"*.sync-conflict-*"}, QDir::Files);
        QCOMPARE(copies.size(), 1); QVERIFY(copies[0].toUtf8().size() <= 220);
        QCOMPARE(get(a.filePath("Files/" + copies[0])), QByteArray("file variant"));
        QCOMPARE(get(b.filePath("Files/" + copies[0])), QByteArray("file variant"));
    }
};
QTEST_GUILESS_MAIN(SyncTests)
#include "synchronizer.moc"
