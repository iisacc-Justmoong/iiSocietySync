#pragma once
#include <QByteArray>
#include <QStringList>
#include <functional>
#include <memory>

namespace iiSocietySync::detail {
struct FileState { QString kind, stamp; qint64 size = 0; };
class ConfinedFiles final {
public:
    QString root, error;
    std::function<bool()> cancelled;
    bool open(const QString &path);
    bool intact();
    bool state(const QString &path, FileState *value);
    QStringList list(const QString &path, bool *ok);
    QByteArray read(const QString &path, qint64 offset, qint64 length, const QString &stamp = {});
    QString hash(const QString &path, const QString &stamp = {});
    bool mkdir(const QString &path);
    bool append(const QString &path, qint64 offset, const QByteArray &bytes);
    bool install(const QString &source, const QString &target);
    bool preserve(const QString &path, const QString &copy);
    // Move a legacy entry into private recovery without following it. Retrying
    // an already completed move is safe; an occupied destination is rejected.
    bool detach(const QString &path, const QString &destination);
    bool remove(const QString &path);
    bool exactPath(const QString &path);
private:
    quint64 m_device = 0, m_inode = 0;
#ifdef Q_OS_WIN
    QByteArray m_fileId;
    quint32 m_nativeError = 0;
    struct WinPath;
    std::unique_ptr<WinPath> openPath(const QString &path, quint32 access, quint32 options = 0,
        bool create = false, bool allowReparse = false, quint32 sharing = 1);
    std::unique_ptr<WinPath> parentPath(const QString &path, bool create);
#else
    int openAt(const QString &path, int flags, bool parents = false);
    int parent(const QString &path, bool create);
#endif
    bool fail(const QString &message);
    bool archive(const QString &path);
};
}
