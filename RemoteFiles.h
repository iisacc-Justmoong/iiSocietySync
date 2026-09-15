#pragma once
#include "Controller.h"
#include <iiServerHost.h>
#include <QUrl>
#include <QVariantList>

namespace iiSocietySync {
// Remote drive browsing/downloads share the authenticated Society transport.
// This public file service exposes Files only. Full replication is Controller's
// separate, explicitly authorized protocol and includes all eight sections.
IISOCIETYSYNC_EXPORT iiServerHost::RequestHandler filesHandler(const QString &container);
class IISOCIETYSYNC_EXPORT RemoteFiles final : public QObject {
    Q_OBJECT
public:
    explicit RemoteFiles(RequestSender send, QObject *parent = nullptr);
    ~RemoteFiles() override;
    bool busy() const { return m_download || !m_request.isEmpty(); }
    QString status() const { return m_status; }
    QString host() const { return m_host; }
    QString path() const { return m_path; }
    QString nextCursor() const { return m_cursor; }
    QString transport() const { return m_transport; }
    QVariantList entries() const { return m_entries; }
    void browse(const QString &host, const QString &path = {}, const QString &cursor = {});
    void download(const QString &path, const QUrl &destination);
    void receive(const QString &id, const QJsonObject &result, const QString &transport);
    void reset();
signals:
    void stateChanged();
    void entriesChanged();
    void downloadFinished(QUrl file);
private:
    void fail(const QString &message);
    void nextChunk();
    QString request(const QString &peer, const QJsonObject &payload);
    RequestSender m_send;
    QString m_status, m_host, m_path, m_cursor, m_transport;
    QVariantList m_entries;
    QString m_request, m_operation, m_downloadPath, m_version;
    qint64 m_received = 0, m_expected = 0;
    class Download;
    std::unique_ptr<Download> m_download;
};
}
