#include "ConfinedFiles.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <qt_windows.h>
#include <winternl.h>
#include <array>
#include <cstring>
#include <vector>

namespace iiSocietySync::detail {
namespace {
bool relative(const QString &path) {
    if (path.isEmpty() || path.size() > 4096 || !path.isValidUtf16() || QDir::isAbsolutePath(path)) return false;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.toUtf8().size() > 255
            || part.endsWith('.') || part.endsWith(' ')) return false;
        for (const auto c : part) if (c.unicode() < 32 || QStringLiteral("\\:<>\"|?*").contains(c)) return false;
        const auto base = part.section('.', 0, 0).toUpper();
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" || base == "CONOUT$") return false;
        if (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))
            && QStringLiteral("123456789¹²³").contains(base[3])) return false;
    }
    return true;
}
bool identity(HANDLE file, quint64 *volume, QByteArray *id) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) return false;
    *volume = info.VolumeSerialNumber;
    *id = QByteArray(reinterpret_cast<const char *>(info.FileId.Identifier), sizeof(info.FileId.Identifier));
    return true;
}
bool attributes(HANDLE file, DWORD *value) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info))) return false;
    *value = info.FileAttributes; return true;
}
bool snapshot(HANDLE file, FileState *state) {
    FILE_BASIC_INFO basic{}; FILE_STANDARD_INFO standard{}; quint64 volume = 0; QByteArray id;
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))
        || !identity(file, &volume, &id) || standard.DeletePending
        || (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    state->kind = standard.Directory ? "directory" : "file";
    state->size = standard.Directory ? 0 : standard.EndOfFile.QuadPart;
    state->stamp = QString("%1:%2:%3:%4:%5:%6").arg(volume).arg(QString::fromLatin1(id.toHex()))
        .arg(state->size).arg(basic.CreationTime.QuadPart).arg(basic.LastWriteTime.QuadPart).arg(basic.ChangeTime.QuadPart);
    return true;
}
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
bool seek(HANDLE file, qint64 offset) { LARGE_INTEGER value{}; value.QuadPart = offset; return SetFilePointerEx(file, value, nullptr, FILE_BEGIN); }
bool readBytes(HANDLE file, QByteArray *bytes) {
    qsizetype done = 0;
    while (done < bytes->size()) {
        DWORD count = 0;
        if (!ReadFile(file, bytes->data() + done, DWORD(qMin<qsizetype>(bytes->size() - done, 1024 * 1024)), &count, nullptr)) return false;
        if (!count) break;
        done += count;
    }
    bytes->resize(done); return true;
}
// The Windows SDK exposes NtCreateFile but leaves these documented information
// records in the WDK. Keep their ABI here so ordinary MSVC/MinGW builds do not
// need a driver kit. The enum values are the FILE_INFORMATION_CLASS contract.
struct NativeNameInformation { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; ULONG FileNameLength; WCHAR FileName[1]; };
constexpr ULONG RenameInformation = 10, LinkInformation = 11, DispositionInformation = 13;
bool setInformation(HANDLE file, void *data, ULONG bytes, ULONG kind) {
    using Function = NTSTATUS (NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    static const auto function = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    if (!function) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    IO_STATUS_BLOCK status{}; const auto result = function(file, &status, data, bytes, kind);
    if (result < 0) { SetLastError(RtlNtStatusToDosError(result)); return false; }
    return true;
}
bool nameOperation(HANDLE source, HANDLE parent, const QString &name, ULONG kind, bool replace) {
    const auto bytes = size_t(name.size()) * sizeof(WCHAR);
    std::vector<unsigned char> buffer(sizeof(NativeNameInformation) + bytes, 0);
    auto *info = reinterpret_cast<NativeNameInformation *>(buffer.data());
    info->ReplaceIfExists = replace; info->RootDirectory = parent; info->FileNameLength = ULONG(bytes);
    std::memcpy(info->FileName, name.utf16(), bytes);
    return setInformation(source, info, ULONG(buffer.size()), kind);
}
}

// Every ancestor remains open without delete/write sharing until the operation
// finishes. All child opens and namespace mutations are relative to those
// handles; no checked absolute pathname is reopened for an operation.
struct ConfinedFiles::WinPath {
    std::vector<HANDLE> handles;
    ~WinPath() { for (auto i = handles.rbegin(); i != handles.rend(); ++i) CloseHandle(*i); }
    HANDLE get() const { return handles.empty() ? INVALID_HANDLE_VALUE : handles.back(); }
    bool child(const QString &name, DWORD access, DWORD options, bool create, bool reparse, DWORD share) {
        UNICODE_STRING text{}; text.Buffer = const_cast<PWSTR>(reinterpret_cast<PCWSTR>(name.utf16()));
        text.Length = USHORT(name.size() * sizeof(WCHAR)); text.MaximumLength = text.Length;
        OBJECT_ATTRIBUTES object{}; object.Length = sizeof(object); object.RootDirectory = get();
        object.ObjectName = &text; object.Attributes = OBJ_CASE_INSENSITIVE;
        HANDLE file = INVALID_HANDLE_VALUE; IO_STATUS_BLOCK status{};
        const auto result = NtCreateFile(&file, access | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object, &status, nullptr,
            FILE_ATTRIBUTE_NORMAL, share, create ? FILE_OPEN_IF : FILE_OPEN,
            options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
        if (result < 0) { SetLastError(RtlNtStatusToDosError(result)); return false; }
        DWORD attr = 0;
        if (!attributes(file, &attr) || (!reparse && (attr & FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(file); SetLastError(ERROR_CANT_ACCESS_FILE); return false;
        }
        handles.push_back(file); return true;
    }
    bool absoluteRoot(const QString &path) {
        // UNC/network roots and DOS device namespaces are never sync stores.
        if (path.size() < 3 || !path[0].isLetter() || path[1] != ':' || path[2] != '/'
            || (path.size() > 3 && !relative(path.mid(3)))) { SetLastError(ERROR_INVALID_NAME); return false; }
        const auto drive = QDir::toNativeSeparators(path.left(3));
        const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        const auto native = QStringLiteral("\\\\?\\") + drive;
        const auto file = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        handles.push_back(file);
        for (const auto &name : path.mid(3).split('/', Qt::SkipEmptyParts))
            if (!child(name, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE, false, false, FILE_SHARE_READ)) return false;
        return true;
    }
};

bool ConfinedFiles::open(const QString &path) {
    root.clear(); error.clear(); m_nativeError = 0; m_fileId.clear();
    const auto clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const QFileInfo info(clean);
    if (!QDir::isAbsolutePath(path) || !info.isDir() || info.isSymLink() || info.isJunction()
        || info.canonicalFilePath().compare(clean, Qt::CaseInsensitive) != 0) return fail("container_unavailable");
    WinPath handle;
    if (!handle.absoluteRoot(clean) || !identity(handle.get(), &m_device, &m_fileId)) return fail("container_unavailable_or_redirected");
    root = clean;
    // Some providers omit capability flags. Exercise the same confined native
    // hard-link operation that protects existing data, before accepting a store.
    const auto probe = ".society-sync/capability-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool usable = mkdir(".society-sync") && append(probe, 0, "probe")
        && preserve(probe, probe + ".link") && read(probe + ".link", 0, 5) == "probe";
    const bool sourceRemoved = remove(probe), copyRemoved = remove(probe + ".link");
    if (!usable || !sourceRemoved || !copyRemoved) { root.clear(); return fail("sync_requires_local_hardlink_storage"); }
    return true;
}
std::unique_ptr<ConfinedFiles::WinPath> ConfinedFiles::openPath(const QString &path, quint32 access,
        quint32 options, bool create, bool allowReparse, quint32 sharing) {
    m_nativeError = 0;
    if (cancelled && cancelled()) { fail("cancelled"); return {}; }
    if (root.isEmpty() || (!path.isEmpty() && !relative(path))) { fail("invalid_path"); return {}; }
    auto result = std::make_unique<WinPath>(); quint64 volume = 0; QByteArray id;
    if (!result->absoluteRoot(root) || !identity(result->get(), &volume, &id) || volume != m_device || id != m_fileId) {
        // An unavailable root is not a missing child, even for state/remove.
        m_nativeError = ERROR_CANT_ACCESS_FILE; fail("container_replaced_or_unavailable"); return {};
    }
    if (path.isEmpty()) return result;
    const auto parts = path.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        if (!result->child(parts[i], last ? access : FILE_LIST_DIRECTORY, last ? options : FILE_DIRECTORY_FILE,
                last && create, last && allowReparse, last ? sharing : FILE_SHARE_READ)) {
            m_nativeError = GetLastError(); fail("path_unavailable_or_redirected"); return {};
        }
    }
    return result;
}
bool ConfinedFiles::intact() { return bool(openPath({}, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE)); }
std::unique_ptr<ConfinedFiles::WinPath> ConfinedFiles::parentPath(const QString &path, bool create) {
    if (!relative(path)) { m_nativeError = ERROR_INVALID_NAME; fail("invalid_path"); return {}; }
    const auto parent = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
    if (create && !parent.isEmpty() && !mkdir(parent)) return {};
    return openPath(parent, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE);
}
bool ConfinedFiles::state(const QString &path, FileState *value) {
    *value = {}; auto file = openPath(path, FILE_READ_ATTRIBUTES, 0, false, false, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (!file) {
        if (missing(m_nativeError)) { error.clear(); value->kind = "deleted"; return true; }
        return false;
    }
    return snapshot(file->get(), value) || fail("stat_failed");
}
QStringList ConfinedFiles::list(const QString &path, bool *ok) {
    *ok = false; auto directory = openPath(path, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE); if (!directory) return {};
    QStringList names; alignas(8) std::array<unsigned char, 65536> buffer{};
    while (GetFileInformationByHandleEx(directory->get(), FileIdBothDirectoryInfo, buffer.data(), DWORD(buffer.size()))) {
        size_t offset = 0;
        while (true) {
            constexpr size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (offset + header > buffer.size()) { fail("directory_read_failed"); return {}; }
            const auto *entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(buffer.data() + offset);
            if ((entry->FileNameLength % 2) || entry->FileNameLength > buffer.size() - offset - header) { fail("directory_read_failed"); return {}; }
            const auto name = QString::fromWCharArray(entry->FileName, entry->FileNameLength / sizeof(WCHAR));
            if (name != "." && name != "..") {
                if (!relative(name)) { fail("unsupported_filename"); return {}; }
                names.append(name);
            }
            if (!entry->NextEntryOffset) break;
            if (entry->NextEntryOffset < header || entry->NextEntryOffset % 8) { fail("directory_read_failed"); return {}; }
            offset += entry->NextEntryOffset;
        }
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) { fail("directory_read_failed"); return {}; }
    names.sort(); *ok = true; return names;
}
QByteArray ConfinedFiles::read(const QString &path, qint64 offset, qint64 length, const QString &expected) {
    error.clear(); auto file = openPath(path, GENERIC_READ, FILE_NON_DIRECTORY_FILE); FileState before, after;
    if (!file || !snapshot(file->get(), &before)) { fail("not_file"); return {}; }
    if ((!expected.isEmpty() && expected != before.stamp) || offset < 0 || offset > before.size || length < 0 || length > 1024 * 1024) {
        fail("file_changed_or_invalid_offset"); return {};
    }
    QByteArray bytes(qMin(length, before.size - offset), Qt::Uninitialized);
    if (!seek(file->get(), offset) || !readBytes(file->get(), &bytes) || !snapshot(file->get(), &after) || after.stamp != before.stamp) {
        fail("file_changed"); return {};
    }
    return bytes;
}
QString ConfinedFiles::hash(const QString &path, const QString &expected) {
    error.clear(); auto file = openPath(path, GENERIC_READ, FILE_NON_DIRECTORY_FILE); FileState before, after;
    if (!file || !snapshot(file->get(), &before)) { fail("not_file"); return {}; }
    if (!expected.isEmpty() && expected != before.stamp) { fail("file_changed"); return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 processed = 0;
    if (hashProgress) hashProgress(path, 0, before.size);
    while (true) {
        if (cancelled && cancelled()) { fail("cancelled"); return {}; }
        QByteArray bytes(1024 * 1024, Qt::Uninitialized);
        if (!readBytes(file->get(), &bytes)) { fail("read_failed"); return {}; }
        if (bytes.isEmpty()) break; hash.addData(bytes);
        processed += bytes.size();
        if (hashProgress) hashProgress(path, processed, before.size);
    }
    if (!snapshot(file->get(), &after) || after.stamp != before.stamp) { fail("file_changed"); return {}; }
    return QString::fromLatin1(hash.result().toHex());
}
bool ConfinedFiles::mkdir(const QString &path) {
    if (!relative(path)) return fail("invalid_path");
    QString prefix;
    for (const auto &part : path.split('/')) {
        prefix += (prefix.isEmpty() ? "" : "/") + part;
        if (!openPath(prefix, FILE_LIST_DIRECTORY, FILE_DIRECTORY_FILE | FILE_WRITE_THROUGH, true)) return false;
    }
    return true;
}
bool ConfinedFiles::append(const QString &path, qint64 offset, const QByteArray &bytes) {
    auto file = openPath(path, GENERIC_READ | GENERIC_WRITE, FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, true);
    FileState before; if (!file || !snapshot(file->get(), &before)) return fail("transfer_unavailable");
    if (offset < 0 || offset > before.size || !seek(file->get(), offset)) return fail("invalid_offset");
    if (offset < before.size) {
        if (bytes.size() > before.size - offset) return fail("overlapping_chunk");
        QByteArray previous(bytes.size(), Qt::Uninitialized);
        return (readBytes(file->get(), &previous) && previous == bytes) || fail("different_chunk_retry");
    }
    qsizetype done = 0;
    while (done < bytes.size()) {
        if (cancelled && cancelled()) return fail("cancelled");
        DWORD count = 0;
        if (!WriteFile(file->get(), bytes.constData() + done, DWORD(qMin<qsizetype>(bytes.size() - done, 1024 * 1024)), &count, nullptr) || !count) return fail("write_failed");
        done += count;
    }
    return FlushFileBuffers(file->get()) || fail("flush_failed");
}
bool ConfinedFiles::preserve(const QString &path, const QString &copy) {
    auto source = openPath(path, GENERIC_READ, FILE_NON_DIRECTORY_FILE, false, false, FILE_SHARE_READ | FILE_SHARE_DELETE);
    auto destination = parentPath(copy, true); if (!source || !destination) return false;
    return nameOperation(source->get(), destination->get(), copy.section('/', -1), LinkInformation, false) || fail("preserve_failed");
}
bool ConfinedFiles::install(const QString &source, const QString &target) {
    auto from = openPath(source, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH);
    auto to = parentPath(target, true); if (!from || !to) return false;
    FileState existing; if (!state(target, &existing)) return false;
    if (existing.kind != "deleted" && (existing.kind != "file" || !archive(target))) return fail("target_is_not_file_or_preserve_failed");
    if (!FlushFileBuffers(from->get())) return fail("flush_failed");
    return nameOperation(from->get(), to->get(), target.section('/', -1), RenameInformation, true) || fail("replace_failed");
}
bool ConfinedFiles::detach(const QString &path, const QString &destination) {
    if (!destination.startsWith(".society-sync/detached/")) return fail("invalid_recovery_path");
    auto to = parentPath(destination, true); if (!to) return false;
    auto source = openPath(path, DELETE, 0, false, true); const bool sourceMissing = !source && missing(m_nativeError);
    if (!source && !sourceMissing) return false;
    auto saved = openPath(destination, FILE_READ_ATTRIBUTES, 0, false, true);
    const bool savedMissing = !saved && missing(m_nativeError);
    if (!saved && !savedMissing) return false;
    if (!source) return bool(saved) || fail("recovery_entry_missing");
    if (saved) return fail("recovery_destination_occupied");
    return nameOperation(source->get(), to->get(), destination.section('/', -1), RenameInformation, false) || fail("recovery_move_failed");
}
bool ConfinedFiles::remove(const QString &path) {
    auto file = openPath(path, DELETE | FILE_READ_ATTRIBUTES, 0, false, false, FILE_SHARE_READ | FILE_SHARE_DELETE);
    if (!file) { if (missing(m_nativeError)) { error.clear(); return true; } return false; }
    FileState value; if (!snapshot(file->get(), &value)) return fail("stat_failed");
    if (value.kind == "file" && !path.startsWith(".society-sync/") && !archive(path)) return false;
    BOOLEAN disposition = true;
    return setInformation(file->get(), &disposition, sizeof(disposition), DispositionInformation)
        || fail("delete_failed_or_directory_not_empty");
}
}
