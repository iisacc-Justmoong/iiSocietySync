#include "Replica.h"
#include "TestLink.h"
#include <SocietyDrive.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace iiSocietySync;
static const QString scope(64, 'a');
static void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(data), data.size());
}
static QByteArray readFile(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
class ReplicaTests : public QObject {
    Q_OBJECT
private slots:
    void manifestHashAndOrderingAreCheckedWithoutWritingFileBytes() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/manifest-source-XXXXXX"), b(SYNC_TEST_DIRECTORY "/manifest-host-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/one"), "first"); writeFile(a.filePath("Files/two"), "second");
        Replica sender, host; QVERIFY(sender.open(a.path(), scope)); QVERIFY(host.open(b.path(), scope)); QVERIFY(sender.scan());
        const auto changes = sender.changes(); const auto entries = changes.value("entries").toArray(); QCOMPARE(entries.size(), 2);
        QByteArray records; for (const auto &entry : entries) records += QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact) + '\n';
        const auto id = QString::fromLatin1(QCryptographicHash::hash(records, QCryptographicHash::Sha256).toHex());
        QJsonObject page{{"action", "manifest"}, {"container", host.containerId()}, {"replica", sender.replicaId()}, {"manifest", id},
            {"offset", "0"}, {"total", "2"}, {"after", "0"}, {"through", changes.value("through")}, {"entries", QJsonArray{entries[0]}}};
        auto response = host.handle("sender", page); QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("complete").toBool());
        QCOMPARE(host.handle("sender", {{"action", "begin"}, {"manifest", id}, {"entry", entries[0]}}).value("error"), "manifest_required_before_transfer");
        auto outOfOrder = page; outOfOrder.insert("offset", "2"); outOfOrder.insert("entries", QJsonArray{});
        QVERIFY(!host.handle("sender", outOfOrder).value("ok").toBool());
        page.insert("offset", "1"); page.insert("entries", QJsonArray{entries[1]});
        response = host.handle("sender", page); QVERIFY(response.value("ok").toBool()); QVERIFY(response.value("complete").toBool());
        QVERIFY(QDir(b.filePath("Files")).isEmpty()); QVERIFY(host.record("files/one").isEmpty());
        QCOMPARE(host.changes().value("through").toString(), "0");
        page.insert("offset", "0"); page.insert("entries", entries); page.insert("manifest", QString(64, 'f'));
        QCOMPARE(host.handle("sender", page).value("error"), "manifest_hash_mismatch");
        QVERIFY(QDir(b.filePath("Files")).isEmpty());
    }
    void sectionInventoryAndLocalBoundary() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/replica-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        for (const auto section : iiSocietyContainer::allStoreSections())
            writeFile(dir.filePath(iiSocietyContainer::storeSectionName(section) + "/one.txt"), "data");
        writeFile(dir.filePath("private-cookie.json"), "must not cross devices");
        Replica replica; QVERIFY2(replica.open(dir.path(), scope), qPrintable(replica.errorString()));
        QVERIFY2(replica.scan(), qPrintable(replica.errorString()));
        const auto manifest = replica.changes(); QVERIFY(manifest.value("ok").toBool());
        QCOMPARE(manifest.value("entries").toArray().size(), 8);
        for (const auto &value : manifest.value("entries").toArray()) QVERIFY(Replica::validRecord(value.toObject()));
        QVERIFY(replica.record("private-cookie.json").isEmpty());
        const auto id = replica.replicaId(); replica.close();
        QVERIFY(replica.open(dir.path(), scope)); QCOMPARE(replica.replicaId(), id);
        QVERIFY(!replica.open(dir.path(), QString(64, 'b')));
    }
    void chunksResumeAndBadDataCannotReplaceExistingFiles() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/send-XXXXXX"), b(SYNC_TEST_DIRECTORY "/receive-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        const QByteArray bytes(700000, 'x'); writeFile(a.filePath("Files/large"), bytes);
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope)); QVERIFY(source.scan());
        const auto entry = source.record("files/large");
        const QJsonObject begin{{"action", "begin"}, {"entry", entry}};
        auto result = target.handle("sender", begin); QVERIFY(result.value("ok").toBool());
        auto chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", "0"}});
        QVERIFY(chunk.value("ok").toBool());
        QJsonObject put{{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", chunk.value("data")}};
        QVERIFY(target.handle("sender", put).value("ok").toBool());
        QVERIFY(target.handle("sender", put).value("ok").toBool()); // Identical retry is idempotent.
        put.insert("data", "!!!!"); QVERIFY(!target.handle("sender", put).value("ok").toBool());
        target.close(); QVERIFY(target.open(b.path(), scope));
        result = target.handle("sender", begin); QCOMPARE(result.value("offset").toString(), QString::number(Replica::ChunkBytes));
        QVERIFY(!QFileInfo::exists(b.filePath("Files/large")));
        for (qint64 offset = Replica::ChunkBytes; offset < bytes.size(); offset += Replica::ChunkBytes) {
            chunk = source.handle("receiver", {{"action", "read"}, {"entry", entry}, {"offset", QString::number(offset)}});
            QVERIFY(chunk.value("ok").toBool());
            QVERIFY(target.handle("sender", {{"action", "chunk"}, {"entry", entry}, {"offset", QString::number(offset)}, {"data", chunk.value("data")}}).value("ok").toBool());
        }
        QVERIFY2(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), qPrintable(target.errorString()));
        QCOMPARE(readFile(b.filePath("Files/large")), bytes);
        QVERIFY(target.handle("sender", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool());
        auto bad = entry; bad.insert("path", "files/../../outside"); QVERIFY(!Replica::validRecord(bad));
        QVERIFY(!target.handle("sender", {{"action", "begin"}, {"entry", bad}}).value("ok").toBool());
    }
    void deletionIsPersistentAndTheSameClockHasOneValue() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/delete-XXXXXX"); QVERIFY(iiSocietyContainer::SocietyDrive::create(dir.path()));
        writeFile(dir.filePath("Files/item"), "before"); Replica replica; QVERIFY(replica.open(dir.path(), scope)); QVERIFY(replica.scan());
        auto entry = replica.record("files/item");
        auto forged = entry; forged.insert("hash", QString(64, 'e'));
        QVERIFY(!replica.handle("sender", {{"action", "begin"}, {"entry", forged}}).value("ok").toBool());
        QVERIFY(QFile::remove(dir.filePath("Files/item"))); QVERIFY(replica.scan());
        QCOMPARE(replica.record("files/item").value("kind").toString(), "deleted");
        replica.close(); QVERIFY(replica.open(dir.path(), scope));
        QCOMPARE(replica.record("files/item").value("kind").toString(), "deleted");
    }
    void corruptCompleteTransferPreservesDestinationAndCanRetry() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/hash-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/hash-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope));
        auto transfer = [&](const QByteArray &data, bool corrupt) {
            writeFile(a.filePath("Files/item"), data); QVERIFY(source.scan());
            const auto entry = source.record("files/item");
            QVERIFY(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
            const auto bytes = corrupt ? QByteArray(data.size(), '?') : data;
            QVERIFY(target.handle("source", {{"action", "chunk"}, {"entry", entry}, {"offset", "0"}, {"data", QString::fromLatin1(bytes.toBase64())}}).value("ok").toBool());
            QCOMPARE(target.handle("source", {{"action", "commit"}, {"entry", entry}}).value("ok").toBool(), !corrupt);
        };
        transfer("original", false);
        transfer("replacement", true);
        QCOMPARE(readFile(b.filePath("Files/item")), QByteArray("original"));
        const auto entry = source.record("files/item");
        QCOMPARE(target.handle("source", {{"action", "begin"}, {"entry", entry}}).value("offset").toString(), "0");
        transfer("replacement", false);
        QCOMPARE(readFile(b.filePath("Files/item")), QByteArray("replacement"));
        const QDir recovery(b.filePath(".society-sync/recovery"));
        const auto metadataFiles = recovery.entryList({"*.json"}, QDir::Files); QCOMPARE(metadataFiles.size(), 1);
        const auto metadata = QJsonDocument::fromJson(readFile(recovery.filePath(metadataFiles[0]))).object();
        QCOMPARE(metadata.value("path").toString(), "Files/item");
        QCOMPARE(readFile(b.filePath(metadata.value("backup").toString())), QByteArray("original"));
        QVERIFY(target.scan()); QCOMPARE(target.changes().value("entries").toArray().size(), 1);
    }
    void redirectedFilesAndReplacedContainerFailClosed() {
        QTemporaryDir parent(SYNC_TEST_DIRECTORY "/boundary-XXXXXX");
        const auto root = parent.filePath("drive");
        QVERIFY(QDir().mkdir(root));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        writeFile(root + "/Files/item", "local"); writeFile(parent.filePath("outside"), "outside");
        Replica replica; QVERIFY(replica.open(root, scope)); QVERIFY(replica.scan());
        const auto entry = replica.record("files/item");
        QVERIFY(QFile::remove(root + "/Files/item")); QVERIFY(nativeTestLink(parent.filePath("outside"), root + "/Files/item"));
        QVERIFY(!replica.scan());
        QVERIFY(!replica.handle("peer", {{"action", "read"}, {"entry", entry}, {"offset", "0"}}).value("ok").toBool());
        QVERIFY(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        QCOMPARE(readFile(parent.filePath("outside")), QByteArray("outside"));
        QCOMPARE(replica.record("files/item").value("version"), entry.value("version"));
        QVERIFY(QDir(parent.path()).rename("drive", "old"));
        QVERIFY(QDir().mkdir(root));
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        QVERIFY(!replica.scan());
        QVERIFY(!replica.handle("peer", {{"action", "begin"}, {"entry", entry}}).value("ok").toBool());
        QVERIFY(!QFileInfo::exists(root + "/Files/item"));
    }
    void redirectedJournalCannotWriteLocksOutsideContainer() {
        QTemporaryDir parent(SYNC_TEST_DIRECTORY "/journal-XXXXXX");
        const auto root = parent.filePath("drive"); QVERIFY(QDir().mkdir(root)); QVERIFY(iiSocietyContainer::SocietyDrive::create(root));
        Replica replica; QVERIFY(replica.open(root, scope));
        QVERIFY(QDir(root).rename(".society-sync", "saved-state"));
        QVERIFY(QDir().mkdir(parent.filePath("outside")));
        QVERIFY(nativeTestLink(parent.filePath("outside"), root + "/.society-sync"));
        QVERIFY(!replica.scan());
        QCOMPARE(QDir(parent.filePath("outside")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        replica.close();
        QVERIFY(removeNativeTestLink(root + "/.society-sync"));
    }
    void filenameAliasesCannotOverwriteDistinctNames() {
        QTemporaryDir a(SYNC_TEST_DIRECTORY "/case-a-XXXXXX"), b(SYNC_TEST_DIRECTORY "/case-b-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(a.path())); QVERIFY(iiSocietyContainer::SocietyDrive::create(b.path()));
        writeFile(a.filePath("Files/item"), "incoming"); writeFile(b.filePath("Files/ITEM"), "original");
        if (!QFileInfo::exists(b.filePath("Files/item"))) QSKIP("This filesystem distinguishes filename case.");
        Replica source, target; QVERIFY(source.open(a.path(), scope)); QVERIFY(target.open(b.path(), scope)); QVERIFY(source.scan());
        const auto result = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/item")}});
        QVERIFY(!result.value("ok").toBool()); QCOMPARE(result.value("error").toString(), "filename_normalization_collision");
        QCOMPARE(readFile(b.filePath("Files/ITEM")), QByteArray("original"));
        writeFile(a.filePath("Files/sub/new"), "child"); QVERIFY(QDir().mkdir(b.filePath("Files/SUB"))); QVERIFY(source.scan());
        const auto child = target.handle("source", {{"action", "begin"}, {"entry", source.record("files/sub/new")}});
        QCOMPARE(child.value("error").toString(), "filename_normalization_collision");
        QVERIFY(!QFileInfo::exists(b.filePath("Files/SUB/new")));
    }
    void manifestPagesKeepAStableCursorWhileNewChangesWaitForNextCycle() {
        QTemporaryDir root(SYNC_TEST_DIRECTORY "/pages-XXXXXX");
        QVERIFY(iiSocietyContainer::SocietyDrive::create(root.path()));
        for (int i = 0; i < 260; ++i) writeFile(root.filePath("Files/" + QString::number(i)), "data");
        Replica replica; QVERIFY(replica.open(root.path(), scope)); QVERIFY(replica.scan());
        auto page = replica.changes(); QCOMPARE(page.value("entries").toArray().size(), 128);
        const auto upper = page.value("through").toString().toLongLong();
        writeFile(root.filePath("Files/259"), "changed after page one"); QVERIFY(replica.scan());
        QSet<QString> paths;
        while (true) {
            QVERIFY(page.value("ok").toBool());
            QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() < 300000);
            for (const auto &entry : page.value("entries").toArray()) {
                const auto path = entry.toObject().value("path").toString(); QVERIFY(!paths.contains(path)); paths.insert(path);
            }
            if (!page.value("more").toBool()) break;
            page = replica.changes(page.value("next").toString().toLongLong(), upper);
        }
        QCOMPARE(page.value("next").toString().toLongLong(), upper);
        page = replica.changes(upper); QCOMPARE(page.value("entries").toArray().size(), 1);
        QCOMPARE(page.value("entries").toArray()[0].toObject().value("path").toString(), "files/259");
        paths.insert("files/259"); QCOMPARE(paths.size(), 260);
    }
};
QTEST_GUILESS_MAIN(ReplicaTests)
#include "replica.moc"
