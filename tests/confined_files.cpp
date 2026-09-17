#include "ConfinedFiles.h"
#include "TestLink.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using iiSocietySync::detail::ConfinedFiles;
using iiSocietySync::detail::FileState;

class ConfinedFilesTests : public QObject {
    Q_OBJECT
private slots:
    void hashingReportsRealByteProgressAndCanStopAtAChunkBoundary() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/hash-progress-XXXXXX");
        ConfinedFiles files; QVERIFY(files.open(dir.path()));
        const QByteArray bytes(3 * 1024 * 1024 + 71, 'p');
        QFile output(dir.filePath("model")); QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(bytes), bytes.size()); output.close();
        QList<qint64> observed;
        files.hashProgress = [&](const QString &path, qint64 done, qint64 total) {
            QCOMPARE(path, QString("model")); QCOMPARE(total, bytes.size());
            if (!observed.isEmpty()) QVERIFY(done > observed.last());
            observed.append(done);
        };
        QCOMPARE(files.hash("model"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(observed.first(), 0); QCOMPARE(observed.last(), bytes.size());
        QVERIFY(observed.size() > 2);
        observed.clear();
        files.cancelled = [&] { return !observed.isEmpty() && observed.last() > 0; };
        QVERIFY(files.hash("model").isEmpty());
        QCOMPARE(files.error, QString("cancelled"));
        QVERIFY(observed.last() < bytes.size());
    }
    void sha256MatchesAcrossPaddingAndReadBoundaries() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/files-digest-XXXXXX");
        ConfinedFiles files; QVERIFY(files.open(dir.path()));
        for (const int size : {0, 1, 55, 56, 63, 64, 65, 1024 * 1024, 1024 * 1024 + 333}) {
            QByteArray bytes(size, Qt::Uninitialized);
            for (int i = 0; i < size; ++i) bytes[i] = char(i % 251);
            const auto name = QString::number(size);
            QFile output(dir.filePath(name)); QVERIFY(output.open(QIODevice::WriteOnly));
            QCOMPARE(output.write(bytes), bytes.size()); output.close();
            FileState state; QVERIFY(files.state(name, &state));
            QCOMPARE(files.hash(name, state.stamp), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        }
        QCOMPARE(files.hash("0"), QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }
    void chunksResumeAndReplacementKeepsOriginal() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/files-chunks-XXXXXX"); QVERIFY(dir.isValid());
        ConfinedFiles files; QVERIFY2(files.open(dir.path()), qPrintable(files.error));
        QVERIFY(files.mkdir("Files/nested")); QVERIFY(files.mkdir(".society-sync/transfers"));
        QVERIFY(files.append("Files/nested/item", 0, "old"));
        QVERIFY(files.append(".society-sync/transfers/incoming", 0, "abc"));
        QVERIFY(files.append(".society-sync/transfers/incoming", 0, "abc"));
        QVERIFY(!files.append(".society-sync/transfers/incoming", 0, "xyz"));
        QVERIFY(!files.append(".society-sync/transfers/incoming", 2, "cd"));
        QVERIFY(!files.append(".society-sync/transfers/incoming", 4, "e"));
        ConfinedFiles resumed; QVERIFY(resumed.open(dir.path()));
        QVERIFY(resumed.append(".society-sync/transfers/incoming", 3, "def"));
        QVERIFY2(resumed.install(".society-sync/transfers/incoming", "Files/nested/item"), qPrintable(resumed.error));
        QCOMPARE(resumed.read("Files/nested/item", 0, 100), QByteArray("abcdef"));
        const QDir recovery(dir.filePath(".society-sync/recovery"));
        const auto saved = recovery.entryList({"*.json"}, QDir::Files); QCOMPARE(saved.size(), 1);
        QFile metadata(recovery.filePath(saved[0])); QVERIFY(metadata.open(QIODevice::ReadOnly));
        const auto backup = QJsonDocument::fromJson(metadata.readAll()).object().value("backup").toString();
        QCOMPARE(resumed.read(backup, 0, 100), QByteArray("old"));
        QVERIFY(resumed.remove("Files/nested/item")); QVERIFY(resumed.remove("Files/nested/item"));
        QCOMPARE(recovery.entryList({"*.json"}, QDir::Files).size(), 2);
        QVERIFY(resumed.remove("Files/nested"));
    }
    void enumerationStampsAndCancellation() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/files-list-XXXXXX");
        ConfinedFiles files; QVERIFY(files.open(dir.path())); QVERIFY(files.mkdir("Files"));
        for (int i = 0; i < 300; ++i) QVERIFY(files.append("Files/항목-" + QString::number(i), 0, "entry"));
        bool ok = false; const auto names = files.list("Files", &ok); QVERIFY2(ok, qPrintable(files.error)); QCOMPARE(names.size(), 300);
        QVERIFY(names.contains("항목-299"));
        const QByteArray bytes(1024 * 1024 + 333, 'x'); QVERIFY(files.append("Files/large", 0, bytes));
        FileState original; QVERIFY(files.state("Files/large", &original)); QCOMPARE(original.size, bytes.size());
        QCOMPARE(files.hash("Files/large", original.stamp), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(files.read("Files/large", 1024 * 1024, 512, original.stamp), QByteArray(333, 'x'));
        QVERIFY(files.read("Files/large", 0, 1024 * 1024 + 1).isEmpty()); QVERIFY(!files.error.isEmpty());
        QVERIFY(files.append("Files/large", bytes.size(), "changed"));
        QVERIFY(files.hash("Files/large", original.stamp).isEmpty());
        files.cancelled = [] { return true; };
        QVERIFY(files.hash("Files/large").isEmpty()); QVERIFY(!files.mkdir("cancelled"));
        files.cancelled = {};
        QVERIFY(files.state("missing/child", &original)); QCOMPARE(original.kind, "deleted");
        QVERIFY(!files.remove("Files")); // Nonempty directories must survive.
    }
    void pathsCannotEscapeAndLegacyLinksCanBeDetached() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/files-boundary-XXXXXX");
        const auto root = dir.filePath("drive"); QVERIFY(QDir().mkdir(root));
        ConfinedFiles files; QVERIFY(files.open(root)); QVERIFY(files.mkdir("Files"));
        QFile outside(dir.filePath("outside")); QVERIFY(outside.open(QIODevice::WriteOnly)); outside.write("outside"); outside.close();
        QVERIFY(nativeTestLink(outside.fileName(), root + "/Files/link"));
        QVERIFY(nativeTestLink(dir.path(), root + "/Files/directory-link"));
        FileState state;
        for (const auto &path : {QString("../outside"), QString("Files/../../outside"), QString("Files/link"), QString("Files/directory-link/outside")}) {
            QVERIFY2(!files.state(path, &state), qPrintable(path));
            QVERIFY(files.read(path, 0, 10).isEmpty()); QVERIFY(!files.append(path, 0, "overwrite"));
            QVERIFY(!files.remove(path));
        }
        QVERIFY(!files.append("Files/stream:secret", 0, "data"));
        QVERIFY(!files.detach("Files/link", "Files/escaped"));
        QVERIFY2(files.detach("Files/link", ".society-sync/detached/old/link"), qPrintable(files.error));
        QVERIFY(files.detach("Files/link", ".society-sync/detached/old/link"));
        QVERIFY(files.detach("Files/directory-link", ".society-sync/detached/old/directory-link"));
        QVERIFY(outside.open(QIODevice::ReadOnly)); QCOMPARE(outside.readAll(), QByteArray("outside")); outside.close();
        QVERIFY(files.append("Files/new", 0, "new"));
        QVERIFY(!files.detach("Files/new", ".society-sync/detached/old/link"));
        QCOMPARE(files.read("Files/new", 0, 10), QByteArray("new"));
        QVERIFY(removeNativeTestLink(root + "/.society-sync/detached/old/link"));
        QVERIFY(removeNativeTestLink(root + "/.society-sync/detached/old/directory-link"));
        QVERIFY(QDir(dir.path()).rename("drive", "old")); QVERIFY(QDir().mkdir(root));
        QVERIFY(!files.intact()); QVERIFY(!files.append("Files/new", 0, "wrong root"));
    }
#ifdef Q_OS_WIN
    void windowsAliasesAndOpenWritersFailClosed() {
        QTemporaryDir dir(SYNC_TEST_DIRECTORY "/files-win-XXXXXX"); ConfinedFiles files;
        QVERIFY(files.open(dir.path())); QVERIFY(files.mkdir("Files"));
        for (const auto *name : {"trailing.", "space ", "CON", "NUL.txt", "LPT1", "a?b", "a|b"})
            QVERIFY(!files.append("Files/" + QString::fromLatin1(name), 0, "data"));
        QVERIFY(files.append("Files/ITEM", 0, "original")); QVERIFY(!files.exactPath("Files/item"));
        QFile writer(dir.filePath("Files/ITEM")); QVERIFY(writer.open(QIODevice::ReadWrite));
        QVERIFY(files.hash("Files/ITEM").isEmpty()); QVERIFY(!files.append("Files/ITEM", 8, "x")); writer.close();
        QVERIFY(!files.hash("Files/ITEM").isEmpty());
        bool attempted = false, renamed = false;
        // openPath checks cancellation before pinning. The callback in the hash
        // loop must run while the full directory chain is held.
        int calls = 0;
        files.cancelled = [&] {
            if (++calls > 1 && !attempted) { attempted = true; renamed = QDir().rename(dir.path(), dir.path() + "-moved"); }
            return false;
        };
        QVERIFY(!files.hash("Files/ITEM").isEmpty()); QVERIFY(attempted); QVERIFY(!renamed);
        files.cancelled = {};
    }
#endif
};
QTEST_GUILESS_MAIN(ConfinedFilesTests)
#include "confined_files.moc"
