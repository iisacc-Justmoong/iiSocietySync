#include "Replica.h"
#include "Synchronizer.h"
#include "ObjectProvider.h"
#include "Controller.h"
#include <SocietyDrive.h>
#include <StorageMap.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <atomic>

using namespace iiSocietySync;
static const QString scope(64, 'a');
static void put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size());
}
static QByteArray get(const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
class DelayedProvider final : public ObjectProvider {
public:
    DirectoryObjectProvider disk;
    std::atomic<bool> entered{false}, released{false};
    explicit DelayedProvider(const QString &directory) : disk("slow-nas", directory, true) {}
    QString id() const override { return disk.id(); }
    QString kind() const override { return disk.kind(); }
    bool put(const ObjectIdentity &o, const QString &file, QString *error, ProviderCancellation cancel) override {
        entered = true; QElapsedTimer elapsed; elapsed.start();
        while (!released && elapsed.elapsed() < 5000 && !(cancel && cancel())) QThread::msleep(10);
        return disk.put(o, file, error, cancel);
    }
    bool get(const ObjectIdentity &o, const QString &file, QString *error, ProviderCancellation cancel) override { return disk.get(o, file, error, cancel); }
};
class NamespaceTests : public QObject {
    Q_OBJECT
    void cycle(Replica &client, Replica &host, const QString &peer = "client") {
        Synchronizer sync(&client); QSignalSpy completed(&sync, &Synchronizer::finished);
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            sync.receive(id, {{"ok", true}, {"result", host.handle(peer, wire.value("message").toObject())}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QVERIFY2(completed[0][1].toBool(), qPrintable(completed[0][2].toString()));
    }
private slots:
    void authorityKeepsHistoryAndStableBoundedSnapshotsOffline() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/namespace-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; QVERIFY(host.open(dir.path(), scope)); put(dir.filePath("Files/note"), "first"); QVERIFY(host.scan());
        const auto first = host.objectMetadata("files/note"); const auto bound = host.namespaceState().value("sequence").toString().toLongLong();
        put(dir.filePath("Files/note"), "second"); QVERIFY(host.scan());
        const auto second = host.objectMetadata("files/note");
        QVERIFY(host.revision(second.value("revision").toString()).value("parents").toArray().contains(first.value("revision")));
        const auto bounded = host.changes(0, bound).value("entries").toArray();
        bool found = false; for (const auto &e : bounded) if (e.toObject().value("path") == "files/note") { found = true; QCOMPARE(e.toObject().value("version"), first.value("version")); }
        QVERIFY(found);
        QVERIFY(QFile::remove(dir.filePath("Files/note"))); QVERIFY(host.scan());
        QCOMPARE(host.objectMetadata("files/note").value("kind"), "deleted");
        QCOMPARE(host.revision(first.value("revision").toString()).value("object").toObject().value("hash"), first.value("hash"));
        const auto head = host.namespaceState(); host.close(); QVERIFY(host.open(dir.path(), scope)); QCOMPARE(host.namespaceState(), head);
    }
    void clientsOnlyProposeAndAllNodesReceiveTheHostDecision() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/authority-XXXXXX"), b(SYNC_TEST_DIRECTORY "/mirror-XXXXXX"), c(SYNC_TEST_DIRECTORY "/other-XXXXXX");
        for (auto dir : {&a, &b, &c}) QVERIFY(iiSocietyContainer::SocietyDrive::create(dir->path()));
        Replica host, phone, pc; QVERIFY(host.open(a.path(), scope)); QVERIFY(phone.open(b.path(), scope)); QVERIFY(pc.open(c.path(), scope));
        put(a.filePath("Files/note"), "initial"); cycle(phone, host); cycle(pc, host, "pc");
        const auto confirmed = phone.objectMetadata("files/note"); const auto cursor = phone.namespaceState().value("sequence");
        put(b.filePath("Files/note"), "offline phone edit"); QVERIFY(phone.scan());
        QCOMPARE(phone.namespaceState().value("sequence"), cursor); QCOMPARE(phone.objectMetadata("files/note"), confirmed);
        QVERIFY(!phone.record("files/note").contains("revision")); QCOMPARE(phone.record("files/note").value("baseRevision"), confirmed.value("revision"));
        put(a.filePath("Files/note"), "host truth"); cycle(phone, host); cycle(pc, host, "pc");
        QCOMPARE(get(a.filePath("Files/note")), "host truth"); QCOMPARE(get(b.filePath("Files/note")), "host truth"); QCOMPARE(get(c.filePath("Files/note")), "host truth");
        const auto conflicts = QDir(b.filePath("Files")).entryList({"note.sync-conflict-*"}, QDir::Files); QCOMPARE(conflicts.size(), 1);
        QCOMPARE(get(c.filePath("Files/" + conflicts[0])), "offline phone edit");
        for (auto replica : {&phone, &pc}) {
            QCOMPARE(replica->namespaceState().value("head"), host.namespaceState().value("head"));
            QCOMPARE(replica->objectMetadata("files/note").value("revision"), host.objectMetadata("files/note").value("revision"));
            QCOMPARE(replica->changes().value("entries").toArray().size(), 0);
        }
        QVERIFY(!phone.handle("other", {{"action", "describe"}}).value("ok").toBool());
    }
    void anEditDuringUploadAcknowledgementSurvivesTheHostRevision() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/ack-host-XXXXXX"), b(SYNC_TEST_DIRECTORY "/ack-client-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope)); cycle(client, host);
        put(b.filePath("Files/note"), "first proposal");
        Synchronizer sync(&client); QSignalSpy completed(&sync, &Synchronizer::finished); bool edited = false;
        connect(&sync, &Synchronizer::requestReady, &sync, [&](auto id, auto, auto wire) {
            const auto request = wire.value("message").toObject(); const auto response = host.handle("client", request);
            if (!edited && request.value("action") == "commit" && request.value("entry").toObject().value("path") == "files/note"
                && response.value("ok").toBool()) {
                QCOMPARE(get(a.filePath("Files/note")), "first proposal");
                put(b.filePath("Files/note"), "new edit before acknowledgement"); edited = true;
            }
            sync.receive(id, {{"ok", true}, {"result", response}});
        });
        QVERIFY(sync.start("host")); QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
        QVERIFY2(completed[0][1].toBool(), qPrintable(completed[0][2].toString())); QVERIFY(edited);
        QCOMPARE(get(a.filePath("Files/note")), "new edit before acknowledgement");
        QCOMPARE(get(b.filePath("Files/note")), "new edit before acknowledgement");
        QCOMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
        QVERIFY(client.record("files/note").contains("revision"));
        QVERIFY(QDir(b.filePath("Files")).entryList({"note.sync-conflict-*"}, QDir::Files).isEmpty());
    }
    void journalRejectsForgedGapsWrongAuthorityAndRollsBackWholePage() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/journal-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/journal-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope));
        put(a.filePath("Files/item"), "bytes"); QVERIFY(host.scan()); QVERIFY(client.bindHost("host", host.replicaId(), host.containerId()));
        const auto page = host.journal();
        QVERIFY(!client.acceptJournal("stranger", page));
        auto bad = page; auto entries = bad.value("entries").toArray(); auto last = entries.last().toObject(); last.insert("revision", QString(64, '0')); entries[entries.size() - 1] = last; bad.insert("entries", entries);
        QVERIFY(!client.acceptJournal("host", bad)); QCOMPARE(client.namespaceState().value("sequence"), "0");
        bad = page; bad.insert("authority", client.replicaId()); QVERIFY(!client.acceptJournal("host", bad));
        QVERIFY(client.acceptJournal("host", page));
        auto forged = host.record("files/item"); forged.insert("revision", QString(64, 'f'));
        QCOMPARE(client.handle("host", {{"action", "begin"}, {"entry", forged}}).value("error"), "host_revision_required");
        QVERIFY(!client.acceptJournal("host", page)); // Replay cannot append history again.
        QCOMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
    }
    void authorityResolvesAFileAgainstOfflineDirectoryChildrenWithoutLoss() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/file-authority-XXXXXX"), b(SYNC_TEST_DIRECTORY "/directory-client-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope)); cycle(client, host);
        put(a.filePath("Files/object"), "host file"); put(b.filePath("Files/object/child"), "offline child");
        cycle(client, host);
        QCOMPARE(get(a.filePath("Files/object/child")), "offline child"); QCOMPARE(get(b.filePath("Files/object/child")), "offline child");
        const auto conflicts = QDir(a.filePath("Files")).entryList({"object.sync-conflict-*"}, QDir::Files); QCOMPARE(conflicts.size(), 1);
        QCOMPARE(get(b.filePath("Files/" + conflicts[0])), "host file");
        QCOMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
    }
    void journalMigrationRetainsCurrentStateWithoutInventingOldHistory() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/migration-XXXXXX"); QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; QVERIFY(host.open(dir.path(), scope)); put(dir.filePath("Files/item"), "old journal"); QVERIFY(host.scan());
        auto old = host.record("files/item"); const auto identity = host.containerId(); host.close();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "namespace-migration-fixture"); db.setDatabaseName(dir.filePath(".society-sync/journal.sqlite")); QVERIFY(db.open());
            QSqlQuery q(db); QVERIFY(q.exec("DROP TABLE namespace_revisions")); QVERIFY(q.exec("DROP TABLE object_locations")); QVERIFY(q.exec("PRAGMA user_version=2"));
            db.close();
        }
        QSqlDatabase::removeDatabase("namespace-migration-fixture");
        QVERIFY2(host.open(dir.path(), scope), qPrintable(host.errorString())); QCOMPARE(host.containerId(), identity);
        QCOMPARE(host.objectMetadata("files/item").value("version"), old.value("version"));
        const auto r = host.revision(host.objectMetadata("files/item").value("revision").toString()); QVERIFY(r.value("parents").toArray().isEmpty());
        QCOMPARE(get(dir.filePath("Files/item")), "old journal");
    }
    void journalPagesRejectGapsAndResumeFromTheDurableCursor() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/pages-host-XXXXXX"), b(SYNC_TEST_DIRECTORY "/pages-client-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica host, client; QVERIFY(host.open(a.path(), scope)); QVERIFY(client.open(b.path(), scope));
        for (int i = 0; i < 150; ++i) put(a.filePath("Files/item-" + QString::number(i)), "item");
        QVERIFY(host.scan()); QVERIFY(client.bindHost("host", host.replicaId(), host.containerId()));
        const auto first = host.journal(); QVERIFY(first.value("more").toBool()); QCOMPARE(first.value("entries").toArray().size(), 128);
        auto bad = first; auto rows = bad.value("entries").toArray(); auto r = rows[0].toObject();
        r.remove("revision"); r.insert("sequence", "2");
        r.insert("revision", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(r).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
        rows[0] = r; bad.insert("entries", rows);
        QVERIFY(!client.acceptJournal("host", bad)); QCOMPARE(client.namespaceState().value("sequence"), "0");
        QVERIFY(client.acceptJournal("host", first)); const auto cursor = client.namespaceState().value("sequence").toString().toLongLong();
        client.close(); QVERIFY(client.open(b.path(), scope)); QCOMPARE(client.namespaceState().value("sequence").toString().toLongLong(), cursor);
        QVERIFY(client.acceptJournal("host", host.journal(cursor, first.value("through").toString().toLongLong())));
        QCOMPARE(client.namespaceState().value("head"), host.namespaceState().value("head"));
        QVERIFY(Replica::claimPrimaryHost(a.path(), scope, "host"));
        QVERIFY(!host.bindHost("other", client.replicaId(), client.containerId())); QCOMPARE(host.errorString(), "authority_cannot_become_mirror");
    }
    void providerCopiesAndRestoresWithoutChangingNamespaceOrRevision() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/provider-host-XXXXXX"), backing(SYNC_TEST_DIRECTORY "/provider-nas-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        Replica host; QVERIFY(host.open(dir.path(), scope)); const auto path = dir.filePath("Models/model.bin");
        const QByteArray bytes(1024 * 1024 + 37, 'm'); put(path, bytes); QVERIFY(host.scan());
        const auto head = host.namespaceState(); const auto metadata = host.objectMetadata("models/model.bin");
        DirectoryObjectProvider nas("nas-a", backing.path(), true);
        QVERIFY2(host.placeObject("models/model.bin", nas), qPrintable(host.errorString()));
        QCOMPARE(host.namespaceState(), head); QCOMPARE(host.objectMetadata("models/model.bin").value("locations").toArray().size(), 2);
        const ObjectIdentity object{host.containerId(), metadata.value("hash").toString(), bytes.size()};
        QCOMPARE(get(backing.filePath(object.key())), bytes);
        QVERIFY(!host.restoreObject("models/model.bin", nas)); QCOMPARE(get(path), bytes);
        QVERIFY(QFile::remove(path)); QVERIFY2(host.restoreObject("models/model.bin", nas), qPrintable(host.errorString()));
        QCOMPARE(get(path), bytes); QCOMPARE(host.namespaceState(), head);
        put(backing.filePath(object.key()), QByteArray(bytes.size(), 'x')); QVERIFY(QFile::remove(path));
        QVERIFY(!host.restoreObject("models/model.bin", nas)); QVERIFY(!QFileInfo::exists(path)); QCOMPARE(host.namespaceState(), head);
        put(path, bytes); QVERIFY(host.scan());
        DirectoryObjectProvider offline("offline", backing.filePath("unmounted"), true);
        QVERIFY(!host.placeObject("models/model.bin", offline)); QVERIFY(host.scan()); QCOMPARE(host.namespaceState(), head);
        QString error; const auto output = dir.filePath("restore.bin"); put(output, "keep");
        QVERIFY(!nas.get(object, output, &error)); QCOMPARE(get(output), "keep");
        QVERIFY(!nas.put(object, path, &error, [] { return true; })); QCOMPARE(error, "provider_cancelled");
    }
    void offlineControllerCommitsWithoutAnyPeersOrInternet() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/offline-controller-XXXXXX"); QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        int requests = 0; Controller controller([&](auto, auto) { ++requests; return QString(); });
        QSignalSpy changed(&controller, &Controller::namespaceChanged); controller.open(dir.path(), scope);
        QTRY_VERIFY(controller.available()); QTRY_VERIFY(!changed.isEmpty()); changed.clear();
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(dir.path()));
        put(dir.filePath("Files/offline"), "host offline");
        // A root/directory batch can publish a head before this file's batch.
        // Wait for the actual committed object before closing the indexer.
        QTRY_COMPARE(map.object("files/offline").value("kind"), "file");
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(changed.last()[0].toJsonObject().value("role"), "authority"); QCOMPARE(requests, 0);
        controller.closeAndWait(); Replica host; QVERIFY(host.open(dir.path(), scope)); QCOMPARE(host.objectMetadata("files/offline").value("kind"), "file");
    }
    void slowProviderDoesNotBlockLocalAuthorityCommits() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/slow-provider-host-XXXXXX"), remote(SYNC_TEST_DIRECTORY "/slow-provider-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path())); put(dir.filePath("Files/stored"), "stored bytes");
        Controller controller({}); QSignalSpy changed(&controller, &Controller::namespaceChanged), done(&controller, &Controller::providerFinished);
        controller.open(dir.path(), scope); QTRY_VERIFY(!changed.isEmpty());
        iiSocietyContainer::StorageMap map(*iiSocietyContainer::SocietyDrive::open(dir.path()));
        QTRY_COMPARE(map.object("files/stored").value("kind"), "file");
        auto provider = std::make_shared<DelayedProvider>(remote.path()); const auto release = qScopeGuard([&] { provider->released = true; });
        controller.placeObject("files/stored", provider); QTRY_VERIFY(provider->entered.load());
        const auto before = changed.last()[0].toJsonObject().value("head");
        put(dir.filePath("Files/while-nas-waits"), "local commit");
        QTRY_COMPARE_WITH_TIMEOUT(map.object("files/while-nas-waits").value("kind"), "file", 3000);
        QTRY_VERIFY(changed.last()[0].toJsonObject().value("head") != before);
        QVERIFY(done.isEmpty()); provider->released = true;
        QTRY_COMPARE(done.size(), 1); QVERIFY2(done[0][3].toBool(), qPrintable(done[0][4].toString()));
        controller.closeAndWait();
    }
    void realS3CompatibleProviderRoundTrip() {
        const auto rclone = qEnvironmentVariable("IISERVERHOST_TEST_RCLONE"); if (rclone.isEmpty()) QSKIP("Set IISERVERHOST_TEST_RCLONE for the local S3 fixture");
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/s3-provider-XXXXXX"); QVERIFY(QDir(dir.path()).mkdir("runtime")); QVERIFY(QDir(dir.path()).mkpath("buckets/bucket"));
        QVERIFY(QFile::setPermissions(dir.filePath("runtime"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer probe; QVERIFY(probe.listen(QHostAddress::LocalHost)); const auto port = probe.serverPort(); probe.close();
        iiServerHost::StorageBridgeOptions backend{rclone, {}, {}, dir.filePath("runtime")};
        iiServerHost::FileProtocolServer server(backend); iiServerHost::FileServerOptions options;
        options.protocol = iiServerHost::FileServerProtocol::S3; options.root = dir.filePath("buckets"); options.port = port;
        options.username = "fixture-key"; options.password = "fixture-secret"; options.readOnly = false;
        QVERIFY2(server.start(options), qPrintable(server.errorString()));
        // A TCP connect can precede protocol readiness (and a recycled ephemeral
        // port can self-connect). Require an actual HTTP response from S3.
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, port);
            if (!socket.waitForConnected(100) || socket.localPort() == port) return false;
            socket.write("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
            return socket.waitForReadyRead(200) && socket.readAll().startsWith("HTTP/1.");
        })(), 30000); // External backend cold start is distinct from transfer latency.
        const auto config = dir.filePath("rclone.conf");
        put(config, QString("[s3fixture]\ntype = s3\nprovider = Other\naccess_key_id = fixture-key\nsecret_access_key = fixture-secret\nendpoint = http://127.0.0.1:%1\nregion = us-east-1\nforce_path_style = true\nno_check_bucket = true\n").arg(port).toUtf8());
        QVERIFY(QFile::setPermissions(config, QFile::ReadOwner | QFile::WriteOwner)); backend.configFile = config;
        RemoteObjectProvider provider("s3-a", "s3", "s3fixture:bucket/society", backend);
        const QByteArray bytes(2 * 1024 * 1024 + 17, 's'); const auto source = dir.filePath("source"), target = dir.filePath("target"); put(source, bytes);
        ObjectIdentity object{QUuid::createUuid().toString(QUuid::WithoutBraces), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), bytes.size()};
        QString error; QVERIFY2(provider.put(object, source, &error), qPrintable(error)); QVERIFY2(provider.get(object, target, &error), qPrintable(error));
        QCOMPARE(get(target), bytes); server.stop();
    }
};
QTEST_GUILESS_MAIN(NamespaceTests)
#include "namespace.moc"
