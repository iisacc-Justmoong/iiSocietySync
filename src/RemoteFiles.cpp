#include "RemoteFiles.h"
#include <SharedStorage.h>
#include <QDir>
#include <QJsonArray>
#include <QSaveFile>
#include <QThread>
#include <atomic>
#ifdef Q_OS_WIN
#include "ConfinedFiles.h"
#include <QUuid>
#endif

namespace iiSocietySync {
#ifdef Q_OS_WIN
namespace {
QJsonObject fileError(const QString &error) { return {{"ok", false}, {"error", error}}; }
bool sharePath(const QString &path) {
    if (path.size() > 3500 || !path.isValidUtf16() || QDir::isAbsolutePath(path)
        || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    if (path.isEmpty()) return true;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.startsWith(".iiserverhost-") || part.startsWith(".society-")) return false;
    return true;
}
// The generic ServerHost FileShare is POSIX-only. Society's Windows Files
// projection uses the same native confinement as its full-container replica,
// without making the transport SDK depend on Society or exposing other sections.
QJsonObject windowsFiles(detail::ConfinedFiles &files, const QJsonObject &request) {
    const auto op = request.value("op").toString(), path = request.value("path").toString();
    if ((request.contains("path") && !request.value("path").isString()) || !sharePath(path)) return fileError("invalid_path");
    const auto native = QStringLiteral("Files") + (path.isEmpty() ? QString() : '/' + path);
    files.error.clear();
    if (op == "list") {
        bool valid = true; const auto offset = request.contains("cursor") ? request.value("cursor").toString().toLongLong(&valid) : 0;
        if (!valid || offset < 0 || offset > 1000000) return fileError("invalid_cursor");
        bool ok = false; const auto names = files.list(native, &ok); if (!ok) return fileError("not_directory");
        QJsonArray entries; qint64 index = 0; bool more = false;
        for (const auto &name : names) {
            detail::FileState state;
            if (!sharePath(name) || !files.state(native + '/' + name, &state) || state.kind == "deleted") continue;
            if (index++ < offset) continue;
            if (entries.size() == 256) { more = true; break; }
            entries.append(QJsonObject{{"name", name}, {"directory", state.kind == "directory"},
                {"size", QString::number(state.size)}, {"version", state.stamp}});
        }
        return {{"ok", true}, {"entries", entries}, {"nextCursor", more ? QString::number(offset + entries.size()) : QString()}};
    }
    detail::FileState state;
    if (!files.state(native, &state)) return fileError("invalid_or_unavailable_path");
    if (op == "stat" || op == "read") {
        if (state.kind != "file") return fileError("not_file");
        if (request.contains("version") && request.value("version").toString() != state.stamp) return fileError("file_changed");
        if (op == "stat") return {{"ok", true}, {"size", QString::number(state.size)}, {"version", state.stamp}};
        bool valid = true; const auto offset = request.contains("offset") ? request.value("offset").toString().toLongLong(&valid) : 0;
        if (!valid || offset < 0 || offset > state.size) return fileError("invalid_offset");
        const auto bytes = files.read(native, offset, iiServerHost::FileShare::ChunkBytes, state.stamp);
        if (!files.error.isEmpty()) return fileError("file_changed_or_unavailable");
        return {{"ok", true}, {"data", QString::fromLatin1(bytes.toBase64())}, {"offset", QString::number(offset)},
            {"size", QString::number(state.size)}, {"version", state.stamp}, {"eof", offset + bytes.size() == state.size}};
    }
    if (op == "write" || op == "mkdir") {
        if (path.isEmpty() || state.kind != "deleted") return fileError("create_failed_or_exists");
        detail::FileState parent;
        if (!files.state(native.left(native.lastIndexOf('/')), &parent) || parent.kind != "directory") return fileError("not_directory");
        if (op == "mkdir") return files.mkdir(native) ? QJsonObject{{"ok", true}} : fileError("create_failed_or_exists");
        const auto data = request.value("data");
        if (!data.isString() || data.toString().size() > ((iiServerHost::FileShare::ChunkBytes + 2) / 3) * 4) return fileError("invalid_data");
        const auto decoded = QByteArray::fromBase64Encoding(data.toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() > iiServerHost::FileShare::ChunkBytes) return fileError("invalid_data");
        const auto temporary = ".society-sync/uploads/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const bool saved = files.mkdir(".society-sync/uploads") && files.append(temporary, 0, decoded.decoded)
            && files.preserve(temporary, native); // A native no-replace hard link publishes the complete file.
        const bool removed = files.remove(temporary);
        if (!saved || !removed) return fileError("write_failed_or_exists");
        return {{"ok", true}, {"size", QString::number(decoded.decoded.size())}};
    }
    return fileError("unsupported_operation");
}
}
#endif
iiServerHost::RequestHandler filesHandler(const QString &container) {
    const auto storage = iiSocietyContainer::SharedStorage::open(container);
    if (!storage) return [](const auto &, const auto &) { return QJsonObject{{"ok", false}, {"error", "container_unavailable"}}; };
    const auto pinned = *storage;
#ifdef Q_OS_WIN
    const auto files = std::make_shared<detail::ConfinedFiles>();
    if (!files->open(pinned.drive().rootPath())) return [](const auto &, const auto &) { return fileError("container_unavailable"); };
    return [files, pinned](const auto &, const auto &message) {
        if (!pinned.drive().isReady()) return fileError("container_unavailable");
        return windowsFiles(*files, message);
    };
#else
    const auto share = std::make_shared<iiServerHost::FileShare>(pinned.filePath(iiSocietyContainer::StoreSection::Files), [pinned] { return pinned.drive().isValid(); });
    return [share](const auto &, const auto &message) { return share->handle(message); };
#endif
}
// Disk operations, chunk decoding and atomic publication are confined to this
// worker. Transport requests and UI notifications remain on the owner's thread.
class DownloadWorker final : public QObject {
    Q_OBJECT
public:
    std::shared_ptr<std::atomic_bool> cancelled;
    std::unique_ptr<QSaveFile> file;
    void open(const QString &path) {
        if (*cancelled) return;
        file = std::make_unique<QSaveFile>(path);
        file->setDirectWriteFallback(false);
        const bool ok = file->open(QIODevice::WriteOnly);
        if (!*cancelled) emit opened(ok);
    }
    void write(const QJsonObject &result, qint64 received, qint64 expected, const QString &version) {
        if (*cancelled) return;
        const auto fail = [this](const QString &error) { emit stored(0, {}, error); };
        const auto decoded = QByteArray::fromBase64Encoding(result.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        bool validOffset, validSize;
        const auto offset = result.value("offset").toString().toLongLong(&validOffset);
        const auto size = result.value("size").toString().toLongLong(&validSize);
        if (!decoded || !validOffset || !validSize || offset != received || size != expected || result.value("version").toString() != version
            || decoded.decoded.size() > iiServerHost::FileShare::ChunkBytes || decoded.decoded.size() > expected - received
            || (decoded.decoded.isEmpty() && received != expected) || !file
            || file->write(decoded.decoded) != decoded.decoded.size()) {
            fail(RemoteFiles::tr("The file changed or the transfer is incomplete.")); return;
        }
        received += decoded.decoded.size();
        if (*cancelled) return;
        if (result.value("eof").toBool()) {
            if (received != expected) { fail(RemoteFiles::tr("The transfer ended before the file was complete.")); return; }
            const auto destination = QUrl::fromLocalFile(file->fileName());
            if (!file->commit()) { fail(RemoteFiles::tr("The downloaded file could not be saved.")); return; }
            file.reset(); emit stored(received, destination, {}); return;
        }
        if (received == expected) { fail(RemoteFiles::tr("The host returned an invalid end-of-file marker.")); return; }
        emit stored(received, {}, {});
    }
signals:
    void opened(bool ok);
    void stored(qint64 received, QUrl destination, QString error);
};
class RemoteFiles::Download {
public:
    QThread *thread = new QThread;
    DownloadWorker *worker = new DownloadWorker;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    Download() {
        worker->cancelled = cancelled; worker->moveToThread(thread);
        thread->setObjectName("iiSocietySync-download");
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
    ~Download() {
        *cancelled = true;
        QMetaObject::invokeMethod(worker, [w = worker] { w->file.reset(); QThread::currentThread()->quit(); });
    }
};
RemoteFiles::RemoteFiles(RequestSender sender, QObject *parent) : QObject(parent), m_send(std::move(sender)) {}
RemoteFiles::~RemoteFiles() = default;
QString RemoteFiles::request(const QString &peer, const QJsonObject &payload) { return m_send ? m_send(peer, payload) : QString(); }
void RemoteFiles::fail(const QString &message) { m_status = message; m_request.clear(); m_download.reset(); emit stateChanged(); }
void RemoteFiles::reset() {
    m_download.reset(); m_request.clear(); m_operation.clear(); m_entries.clear();
    m_host.clear(); m_path.clear(); m_cursor.clear(); m_transport.clear(); m_status.clear();
    emit entriesChanged(); emit stateChanged();
}
void RemoteFiles::browse(const QString &host, const QString &path, const QString &cursor) {
    if (busy()) return;
    m_host = host; m_path = path; m_cursor.clear(); m_operation = "list";
    if (cursor.isEmpty()) m_entries.clear();
    QJsonObject request{{"op", "list"}, {"path", path}}; if (!cursor.isEmpty()) request.insert("cursor", cursor);
    m_request = this->request(host, request); emit entriesChanged(); emit stateChanged();
}
void RemoteFiles::download(const QString &path, const QUrl &destination) {
    if (busy() || m_host.isEmpty()) return;
    if (!destination.isLocalFile() || !QDir::isAbsolutePath(destination.toLocalFile())) { fail(tr("Choose a local destination file.")); return; }
    m_downloadPath = path; m_received = 0; m_operation = "opening";
    m_download = std::make_unique<Download>();
    const auto cancelled = m_download->cancelled;
    connect(m_download->worker, &DownloadWorker::opened, this, [this, cancelled](bool ok) {
        if (*cancelled) return;
        if (!ok) { fail(tr("The destination cannot be opened.")); return; }
        m_operation = "stat";
        m_request = request(m_host, {{"op", "stat"}, {"path", m_downloadPath}});
        if (m_request.isEmpty()) { fail(tr("The file request failed.")); return; }
        emit stateChanged();
    });
    connect(m_download->worker, &DownloadWorker::stored, this,
        [this, cancelled](qint64 received, const QUrl &destination, const QString &error) {
            if (*cancelled) return;
            if (!error.isEmpty()) { fail(error); return; }
            m_received = received;
            if (!destination.isEmpty()) {
                m_download.reset(); m_status = tr("Download complete.");
                emit stateChanged(); emit downloadFinished(destination);
            } else nextChunk();
        });
    QMetaObject::invokeMethod(m_download->worker, [w = m_download->worker, destination] { w->open(destination.toLocalFile()); });
    emit stateChanged();
}
void RemoteFiles::nextChunk() {
    m_operation = "read";
    m_request = request(m_host, {{"op", "read"}, {"path", m_downloadPath}, {"offset", QString::number(m_received)}, {"version", m_version}});
    if (m_request.isEmpty()) { fail(tr("The file request failed.")); return; }
    emit stateChanged();
}
void RemoteFiles::receive(const QString &id, const QJsonObject &result, const QString &transport) {
    if (m_request.isEmpty() || id != m_request) return;
    m_request.clear(); m_transport = transport;
    if (!result.value("ok").toBool()) { fail(result.value("error").toString(tr("The file request failed."))); return; }
    if (m_operation == "list") {
        m_entries.append(result.value("entries").toArray().toVariantList()); m_cursor = result.value("nextCursor").toString();
        m_status = tr("Files on your device."); emit entriesChanged(); emit stateChanged(); return;
    }
    if (!m_download) { fail(tr("The download was cancelled.")); return; }
    if (m_operation == "stat") {
        bool valid; m_expected = result.value("size").toString().toLongLong(&valid); m_version = result.value("version").toString();
        if (!valid || m_expected < 0 || m_version.isEmpty()) { fail(tr("The host returned invalid file metadata.")); return; }
        nextChunk(); return;
    }
    m_operation = "saving";
    QMetaObject::invokeMethod(m_download->worker, [w = m_download->worker, result, received = m_received, expected = m_expected, version = m_version] {
        w->write(result, received, expected, version);
    });
}

}

#include "RemoteFiles.moc"
