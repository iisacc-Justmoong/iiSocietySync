#include "ConfinedFiles.h"
#include <QCryptographicHash>
#ifdef Q_OS_DARWIN
#include <CommonCrypto/CommonDigest.h>
#endif
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace iiSocietySync::detail {
namespace {
#ifdef Q_OS_UNIX
struct Fd { int fd; explicit Fd(int value) : fd(value) {} ~Fd() { if (fd >= 0) ::close(fd); } };
QString stamp(const struct stat &s) {
#ifdef Q_OS_DARWIN
    const auto time = s.st_mtimespec; const auto change = s.st_ctimespec;
#else
    const auto time = s.st_mtim; const auto change = s.st_ctim;
#endif
    return QString("%1:%2:%3:%4:%5:%6:%7").arg(quint64(s.st_dev)).arg(quint64(s.st_ino)).arg(s.st_size)
        .arg(time.tv_sec).arg(time.tv_nsec).arg(change.tv_sec).arg(change.tv_nsec);
}
#endif
bool relative(const QString &path) {
    if (path.size() > 4096 || !path.isValidUtf16() || QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 255) return false;
    return true;
}
}
bool ConfinedFiles::fail(const QString &message) { error = message; return false; }
#ifndef Q_OS_WIN
bool ConfinedFiles::open(const QString &path) {
    root.clear(); error.clear();
#ifdef Q_OS_UNIX
    const QFileInfo info(path); struct stat s{};
    if (!QDir::isAbsolutePath(path) || !info.isDir() || info.isSymLink() || info.canonicalFilePath() != QDir::cleanPath(path)
        || ::lstat(QFile::encodeName(path).constData(), &s) != 0 || !S_ISDIR(s.st_mode)) return fail("container_unavailable");
    root = info.canonicalFilePath(); m_device = s.st_dev; m_inode = s.st_ino; return true;
#else
    Q_UNUSED(path); return fail("native_sync_filesystem_unsupported_platform");
#endif
}
bool ConfinedFiles::intact() {
    if (cancelled && cancelled()) return fail("cancelled");
#ifdef Q_OS_UNIX
    struct stat s{};
    if (!root.isEmpty() && ::lstat(QFile::encodeName(root).constData(), &s) == 0 && S_ISDIR(s.st_mode)
        && quint64(s.st_dev) == m_device && quint64(s.st_ino) == m_inode) return true;
#endif
    return fail("container_replaced_or_unavailable");
}
int ConfinedFiles::openAt(const QString &path, int flags, bool parents) {
#ifdef Q_OS_UNIX
    if (!intact() || (!path.isEmpty() && !relative(path))) { fail("invalid_path"); return -1; }
    int fd = ::open(QFile::encodeName(root).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { fail("container_unavailable"); return -1; }
    struct stat s{};
    if (::fstat(fd, &s) || quint64(s.st_dev) != m_device || quint64(s.st_ino) != m_inode) { ::close(fd); fail("container_replaced"); return -1; }
    if (path.isEmpty()) return fd;
    const auto parts = path.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto name = QFile::encodeName(parts[i]); const bool last = i + 1 == parts.size();
        if (parents && !last) ::mkdirat(fd, name.constData(), 0700);
        const int next = ::openat(fd, name.constData(), (last ? flags : O_RDONLY | O_DIRECTORY)
            | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        ::close(fd); fd = next;
        if (fd < 0) { fail("path_unavailable_or_redirected"); return -1; }
    }
    return fd;
#else
    Q_UNUSED(path); Q_UNUSED(flags); Q_UNUSED(parents); fail("native_sync_filesystem_unsupported_platform"); return -1;
#endif
}
int ConfinedFiles::parent(const QString &path, bool create) {
#ifdef Q_OS_UNIX
    if (!relative(path)) { fail("invalid_path"); return -1; }
    const auto name = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
    if (create && !name.isEmpty() && !mkdir(name)) return -1;
    return openAt(name, O_RDONLY | O_DIRECTORY);
#else
    Q_UNUSED(path); Q_UNUSED(create); return -1;
#endif
}
bool ConfinedFiles::state(const QString &path, FileState *value) {
    *value = {};
#ifdef Q_OS_UNIX
    Fd directory(parent(path, false));
    if (directory.fd < 0) {
        if (errno == ENOENT && intact()) { error.clear(); value->kind = "deleted"; return true; }
        return false;
    }
    struct stat s{};
    if (::fstatat(directory.fd, QFile::encodeName(path.section('/', -1)).constData(), &s, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) { value->kind = "deleted"; return true; }
        return fail("stat_failed");
    }
    if (!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)) return fail("symlink_or_special_file");
    value->kind = S_ISDIR(s.st_mode) ? "directory" : "file";
    value->stamp = stamp(s); value->size = S_ISREG(s.st_mode) ? s.st_size : 0; return true;
#else
    Q_UNUSED(path); return false;
#endif
}
QStringList ConfinedFiles::list(const QString &path, bool *ok) {
    *ok = false; QStringList names;
#ifdef Q_OS_UNIX
    const int fd = openAt(path, O_RDONLY | O_DIRECTORY); if (fd < 0) return {};
    DIR *dir = ::fdopendir(fd); if (!dir) { ::close(fd); fail("directory_unavailable"); return {}; }
    errno = 0;
    while (const auto *entry = ::readdir(dir)) {
        const QByteArray name(entry->d_name); if (name == "." || name == "..") continue;
        const auto text = QFile::decodeName(name);
        if (!relative(text) || QFile::encodeName(text) != name) { ::closedir(dir); fail("unsupported_filename"); return {}; }
        names.append(text);
    }
    const bool readOk = errno == 0; ::closedir(dir);
    if (!readOk) { fail("directory_read_failed"); return {}; }
    names.sort(); *ok = true;
#else
    Q_UNUSED(path);
#endif
    return names;
}
QByteArray ConfinedFiles::read(const QString &path, qint64 offset, qint64 length, const QString &expected) {
    error.clear();
#ifdef Q_OS_UNIX
    Fd fd(openAt(path, O_RDONLY)); struct stat s{};
    if (fd.fd < 0 || ::fstat(fd.fd, &s) || !S_ISREG(s.st_mode)) { fail("not_file"); return {}; }
    const auto before = stamp(s);
    if ((!expected.isEmpty() && expected != before) || offset < 0 || offset > s.st_size || length < 0 || length > 1024 * 1024) { fail("file_changed_or_invalid_offset"); return {}; }
    QByteArray bytes(qMin(length, s.st_size - offset), Qt::Uninitialized);
    const auto count = ::pread(fd.fd, bytes.data(), bytes.size(), offset);
    if (count < 0 || ::fstat(fd.fd, &s) || stamp(s) != before) { fail("file_changed"); return {}; }
    bytes.resize(count); return bytes;
#else
    Q_UNUSED(path); Q_UNUSED(offset); Q_UNUSED(length); Q_UNUSED(expected); fail("unsupported_filesystem"); return {};
#endif
}
QString ConfinedFiles::hash(const QString &path, const QString &expected) {
    error.clear();
#ifdef Q_OS_UNIX
    Fd fd(openAt(path, O_RDONLY)); struct stat s{};
    if (fd.fd < 0 || ::fstat(fd.fd, &s) || !S_ISREG(s.st_mode)) { fail("not_file"); return {}; }
    const auto before = stamp(s);
    if (!expected.isEmpty() && expected != before) { fail("file_changed"); return {}; }
#ifdef Q_OS_DARWIN
    // CommonCrypto uses the platform's SHA implementation. The Qt distribution
    // used by our Apple builds otherwise hashes large models in software.
    CC_SHA256_CTX hash;
    if (!CC_SHA256_Init(&hash)) { fail("hash_initialization_failed"); return {}; }
#else
    QCryptographicHash hash(QCryptographicHash::Sha256);
#endif
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    qint64 processed = 0;
    if (hashProgress) hashProgress(path, 0, s.st_size);
    while (true) {
        if (cancelled && cancelled()) { fail("cancelled"); return {}; }
        const auto size = ::read(fd.fd, buffer.data(), buffer.size());
        if (size < 0) { fail("read_failed"); return {}; }
        if (!size) break;
#ifdef Q_OS_DARWIN
        if (!CC_SHA256_Update(&hash, buffer.constData(), CC_LONG(size))) { fail("hash_update_failed"); return {}; }
#else
        hash.addData(QByteArrayView(buffer.constData(), size));
#endif
        processed += size;
        if (hashProgress) hashProgress(path, processed, s.st_size);
    }
    if (::fstat(fd.fd, &s) || stamp(s) != before) { fail("file_changed"); return {}; }
#ifdef Q_OS_DARWIN
    QByteArray result(CC_SHA256_DIGEST_LENGTH, Qt::Uninitialized);
    if (!CC_SHA256_Final(reinterpret_cast<unsigned char *>(result.data()), &hash)) { fail("hash_finalization_failed"); return {}; }
    return QString::fromLatin1(result.toHex());
#else
    return QString::fromLatin1(hash.result().toHex());
#endif
#else
    Q_UNUSED(path); Q_UNUSED(expected); fail("unsupported_filesystem"); return {};
#endif
}
bool ConfinedFiles::mkdir(const QString &path) {
#ifdef Q_OS_UNIX
    if (!relative(path)) return fail("invalid_path");
    QString prefix;
    for (const auto &part : path.split('/')) {
        Fd dir(openAt(prefix, O_RDONLY | O_DIRECTORY)); if (dir.fd < 0) return false;
        if (::mkdirat(dir.fd, QFile::encodeName(part).constData(), 0700) && errno != EEXIST) return fail("mkdir_failed");
        prefix += (prefix.isEmpty() ? "" : "/") + part;
        Fd check(openAt(prefix, O_RDONLY | O_DIRECTORY)); if (check.fd < 0) return false;
    }
    return true;
#else
    Q_UNUSED(path); return false;
#endif
}
bool ConfinedFiles::append(const QString &path, qint64 offset, const QByteArray &bytes) {
#ifdef Q_OS_UNIX
    Fd fd(openAt(path, O_RDWR | O_CREAT)); struct stat s{};
    if (fd.fd < 0 || ::fstat(fd.fd, &s) || !S_ISREG(s.st_mode)) return fail("transfer_unavailable");
    if (offset < 0 || offset > s.st_size) return fail("invalid_offset");
    if (offset < s.st_size) {
        if (offset + bytes.size() > s.st_size) return fail("overlapping_chunk");
        QByteArray previous(bytes.size(), Qt::Uninitialized);
        return (::pread(fd.fd, previous.data(), previous.size(), offset) == bytes.size() && previous == bytes) || fail("different_chunk_retry");
    }
    qint64 done = 0;
    while (done < bytes.size()) {
        if (cancelled && cancelled()) return fail("cancelled");
        const auto n = ::pwrite(fd.fd, bytes.data() + done, bytes.size() - done, offset + done);
        if (n <= 0) return fail("write_failed"); done += n;
    }
    return ::fsync(fd.fd) == 0 || fail("flush_failed");
#else
    Q_UNUSED(path); Q_UNUSED(offset); Q_UNUSED(bytes); return false;
#endif
}
bool ConfinedFiles::preserve(const QString &path, const QString &copy) {
#ifdef Q_OS_UNIX
    Fd source(parent(path, false)), dest(parent(copy, true));
    if (source.fd < 0 || dest.fd < 0) return false;
    struct stat s{}; const auto name = QFile::encodeName(path.section('/', -1));
    if (::fstatat(source.fd, name.constData(), &s, AT_SYMLINK_NOFOLLOW) || !S_ISREG(s.st_mode)) return fail("not_file");
    if (::linkat(source.fd, name.constData(), dest.fd, QFile::encodeName(copy.section('/', -1)).constData(), 0)) return fail("preserve_failed");
    return ::fsync(dest.fd) == 0 || fail("flush_failed");
#else
    Q_UNUSED(path); Q_UNUSED(copy); return false;
#endif
}
bool ConfinedFiles::install(const QString &source, const QString &target) {
#ifdef Q_OS_UNIX
    if (!intact()) return false;
    Fd from(parent(source, false)), to(parent(target, true)); if (from.fd < 0 || to.fd < 0) return false;
    const auto name = QFile::encodeName(target.section('/', -1)); struct stat s{};
    if (::fstatat(to.fd, name.constData(), &s, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(s.st_mode)) return fail("target_is_not_file");
        if (!archive(target)) return false;
    } else if (errno != ENOENT) return fail("target_unavailable");
    if (!intact()) return false;
    if (::renameat(from.fd, QFile::encodeName(source.section('/', -1)).constData(), to.fd, name.constData())) return fail("replace_failed");
    return ::fsync(to.fd) == 0 || fail("flush_failed");
#else
    Q_UNUSED(source); Q_UNUSED(target); return false;
#endif
}
bool ConfinedFiles::detach(const QString &path, const QString &destination) {
#ifdef Q_OS_UNIX
    if (!destination.startsWith(".society-sync/detached/")) return fail("invalid_recovery_path");
    Fd from(parent(path, false)), to(parent(destination, true));
    if (from.fd < 0 || to.fd < 0) return false;
    const auto source = QFile::encodeName(path.section('/', -1));
    const auto target = QFile::encodeName(destination.section('/', -1));
    struct stat a{}, b{};
    const bool exists = ::fstatat(from.fd, source.constData(), &a, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) return fail("recovery_source_unavailable");
    const bool saved = ::fstatat(to.fd, target.constData(), &b, AT_SYMLINK_NOFOLLOW) == 0;
    if (!saved && errno != ENOENT) return fail("recovery_destination_unavailable");
    if (!exists) return saved || fail("recovery_entry_missing");
    if (saved) return fail("recovery_destination_occupied");
    if (!intact() || ::renameat(from.fd, source.constData(), to.fd, target.constData())) return fail("recovery_move_failed");
    return (::fsync(from.fd) == 0 && ::fsync(to.fd) == 0) || fail("flush_failed");
#else
    Q_UNUSED(path); Q_UNUSED(destination); return fail("unsupported_filesystem");
#endif
}
bool ConfinedFiles::remove(const QString &path) {
#ifdef Q_OS_UNIX
    Fd directory(parent(path, false));
    if (directory.fd < 0) {
        if (errno == ENOENT && intact()) { error.clear(); return true; }
        return false;
    }
    struct stat s{}; const auto name = QFile::encodeName(path.section('/', -1));
    if (::fstatat(directory.fd, name.constData(), &s, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT || fail("stat_failed");
    if (!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)) return fail("symlink_or_special_file");
    if (S_ISREG(s.st_mode) && !path.startsWith(".society-sync/")
        && !archive(path)) return false;
    if (!intact() || ::unlinkat(directory.fd, name.constData(), S_ISDIR(s.st_mode) ? AT_REMOVEDIR : 0)) return fail("delete_failed_or_directory_not_empty");
    return ::fsync(directory.fd) == 0 || fail("flush_failed");
#else
    Q_UNUSED(path); return false;
#endif
}
#endif
bool ConfinedFiles::archive(const QString &path) {
    const auto copy = ".society-sync/recovery/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!preserve(path, copy)) return false;
    const auto metadata = QJsonDocument(QJsonObject{{"path", path}, {"backup", copy},
        {"savedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson(QJsonDocument::Compact);
    return append(copy + ".json", 0, metadata);
}
bool ConfinedFiles::exactPath(const QString &path) {
    QString parent;
    for (const auto &part : path.split('/')) {
        bool ok; const auto names = list(parent, &ok);
        if (!ok) return false;
        if (!names.contains(part, Qt::CaseSensitive)) {
            FileState existing;
            const auto candidate = parent + (parent.isEmpty() ? "" : "/") + part;
            if (!state(candidate, &existing)) return false;
            if (existing.kind == "deleted") return true; // The remaining suffix can be created with its exact name.
            return fail("filename_normalization_collision");
        }
        parent += (parent.isEmpty() ? "" : "/") + part;
    }
    return true;
}
}
