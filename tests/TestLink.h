#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

// QFile::link creates a shortcut on Windows, which cannot exercise reparse
// confinement. Native Windows runs need Developer Mode or symlink privilege.
inline bool nativeTestLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    const auto nativeTarget = QDir::toNativeSeparators(target);
    const auto nativeLink = QDir::toNativeSeparators(link);
    const DWORD flags = (QFileInfo(target).isDir() ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0)
        | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
            reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), flags)) return false;
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(nativeLink.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFile::link(target, link);
#endif
}

inline bool removeNativeTestLink(const QString &link) {
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(link);
    const auto name = reinterpret_cast<LPCWSTR>(native.utf16());
    const DWORD attributes = GetFileAttributesW(name);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(name) : DeleteFileW(name);
#else
    return QFile::remove(link);
#endif
}
