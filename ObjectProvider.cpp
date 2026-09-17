#include "ObjectProvider.h"
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QUuid>

namespace iiSocietySync {
namespace {
bool fail(QString *error, const QString &code) { if (error) *error = code; return false; }
bool validId(const QString &id) { return QRegularExpression("\\A[a-zA-Z0-9_-]{1,64}\\z").match(id).hasMatch() && id != "local"; }
bool privateRuntime(const QString &path) {
    const QFileInfo info(path);
    if (!info.isAbsolute() || !info.isDir() || info.isSymLink() || info.isJunction() || info.canonicalFilePath() != QDir::cleanPath(path)) return false;
#ifdef Q_OS_UNIX
    if (info.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) return false;
#endif
    return true;
}
bool verifiedCopy(const ObjectIdentity &object, const QString &from, const QString &to, QString *error, const ProviderCancellation &cancelled) {
    if (error) error->clear();
    const QFileInfo source(from), target(to);
    if (!object.valid() || !source.isAbsolute() || !source.isFile() || source.isSymLink() || source.isJunction()
        || !target.isAbsolute() || target.isSymLink() || target.isJunction() || source.size() != object.size)
        return fail(error, "invalid_provider_object");
    QFile input(from); QSaveFile output(to); output.setDirectWriteFallback(false);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)
        || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) return fail(error, "provider_file_unavailable");
    QCryptographicHash digest(QCryptographicHash::Sha256); qint64 size = 0;
    while (!input.atEnd()) {
        if (cancelled && cancelled()) return fail(error, "provider_cancelled");
        const auto bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() || size > object.size - bytes.size() || output.write(bytes) != bytes.size()) return fail(error, "provider_io_failed");
        size += bytes.size(); digest.addData(bytes);
    }
    if (input.error() != QFile::NoError || size != object.size || QString::fromLatin1(digest.result().toHex()) != object.sha256)
        return fail(error, "provider_hash_mismatch");
    if (cancelled && cancelled()) return fail(error, "provider_cancelled");
    return output.commit() || fail(error, "provider_commit_failed");
}
}
bool ObjectIdentity::valid() const {
    return !QUuid(nameSpace).isNull() && QUuid(nameSpace).toString(QUuid::WithoutBraces) == nameSpace
        && QRegularExpression("\\A[a-f0-9]{64}\\z").match(sha256).hasMatch() && size >= 0;
}
QString ObjectIdentity::key() const { return valid() ? "objects/" + nameSpace + '/' + sha256.left(2) + '/' + sha256 : QString(); }
DirectoryObjectProvider::DirectoryObjectProvider(QString id, QString directory, bool nas)
    : m_id(std::move(id)), m_root(QDir::cleanPath(directory)), m_nas(nas) {}
QString DirectoryObjectProvider::id() const { return m_id; }
QString DirectoryObjectProvider::kind() const { return m_nas ? "nas" : "local"; }
QString DirectoryObjectProvider::path(const ObjectIdentity &object, QString *error, bool create) {
    const QFileInfo root(m_root);
    if (!validId(m_id) || !object.valid() || !root.isAbsolute() || !root.isDir()
        || root.isSymLink() || root.isJunction() || root.canonicalFilePath() != m_root) { fail(error, "provider_root_unavailable"); return {}; }
    auto current = m_root; const auto parts = object.key().split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current += '/' + parts[i]; QFileInfo entry(current);
        if (entry.isSymLink() || entry.isJunction()) { fail(error, "provider_path_redirected"); return {}; }
        if (i + 1 < parts.size()) {
            if (!entry.exists() && create && !QDir().mkdir(current)) { fail(error, "provider_directory_failed"); return {}; }
            if (!QFileInfo(current).isDir()) { fail(error, "provider_object_unavailable"); return {}; }
        }
    }
    return current;
}
bool DirectoryObjectProvider::put(const ObjectIdentity &o, const QString &source, QString *error, ProviderCancellation cancelled) {
    const auto target = path(o, error, true); return !target.isEmpty() && verifiedCopy(o, source, target, error, cancelled);
}
bool DirectoryObjectProvider::get(const ObjectIdentity &o, const QString &target, QString *error, ProviderCancellation cancelled) {
    const auto source = path(o, error, false); return !source.isEmpty() && verifiedCopy(o, source, target, error, cancelled);
}
RemoteObjectProvider::RemoteObjectProvider(QString id, QString kind, QString prefix, iiServerHost::StorageBridgeOptions backend)
    : m_id(std::move(id)), m_kind(std::move(kind)), m_prefix(std::move(prefix)), m_backend(std::move(backend)) {
    while (m_prefix.endsWith('/')) m_prefix.chop(1);
}
QString RemoteObjectProvider::id() const { return m_id; }
QString RemoteObjectProvider::kind() const { return m_kind; }
bool RemoteObjectProvider::copy(const QString &source, const QString &target, QString *error, const ProviderCancellation &cancelled) {
    if (!validId(m_id) || (m_kind != "s3" && m_kind != "nas")
        || !QRegularExpression("\\A[A-Za-z0-9_][A-Za-z0-9_-]*:[^\\x00-\\x1f]*\\z").match(m_prefix).hasMatch())
        return fail(error, "invalid_remote_provider");
    if (cancelled && cancelled()) return fail(error, "provider_cancelled");
    iiServerHost::StorageResult result;
    // A separate event loop prevents re-entering Replica while its worker waits
    // for the existing asynchronous bridge. No shell or ambient cloud secrets.
    auto thread = QThread::create([&] {
        QEventLoop loop; iiServerHost::StorageBridge bridge(m_backend); QTimer timer;
        QObject::connect(&bridge, &iiServerHost::StorageBridge::finished, &loop, [&](auto, auto value) { result = value; loop.quit(); });
        iiServerHost::StorageRequest request;
        request.operation = iiServerHost::StorageOperation::CopyFile; request.source = source; request.destination = target;
        request.overwrite = true;
        const auto id = bridge.start(request);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (cancelled && cancelled()) bridge.cancel(id); });
        timer.start(50); loop.exec();
    });
    thread->start(); thread->wait(); delete thread;
    return result.ok || fail(error, result.cancelled ? "provider_cancelled" : result.error);
}
bool RemoteObjectProvider::put(const ObjectIdentity &o, const QString &source, QString *error, ProviderCancellation cancelled) {
    if (!privateRuntime(m_backend.runtimeDirectory)) return fail(error, "private_runtime_directory_required");
    // Freeze the exact bytes before remote I/O; editing the projected file must
    // never put different content under a committed hash. Remove staging on exit.
    QTemporaryFile frozen(QDir(m_backend.runtimeDirectory).filePath("society-object-XXXXXX"));
    QTemporaryFile verified(QDir(m_backend.runtimeDirectory).filePath("society-verify-XXXXXX"));
    if (!frozen.open() || !verified.open()) return fail(error, "provider_staging_unavailable");
    const auto frozenPath = frozen.fileName(), verifiedPath = verified.fileName(); frozen.close(); verified.close();
    if (!verifiedCopy(o, source, frozenPath, error, cancelled) || !copy(frozenPath, m_prefix + '/' + o.key(), error, cancelled)) return false;
    // A provider location becomes visible only after reading and checking the
    // stored object. This also covers NAS backends without native SHA-256 hashes.
    return get(o, verifiedPath, error, cancelled);
}
bool RemoteObjectProvider::get(const ObjectIdentity &o, const QString &target, QString *error, ProviderCancellation cancelled) {
    if (!o.valid()) return fail(error, "invalid_provider_object");
    if (!privateRuntime(m_backend.runtimeDirectory)) return fail(error, "private_runtime_directory_required");
    QTemporaryFile incoming(QDir(m_backend.runtimeDirectory).filePath("society-download-XXXXXX"));
    if (!incoming.open()) return fail(error, "provider_staging_unavailable");
    const auto path = incoming.fileName(); incoming.close();
    return copy(m_prefix + '/' + o.key(), path, error, cancelled) && verifiedCopy(o, path, target, error, cancelled);
}
}
