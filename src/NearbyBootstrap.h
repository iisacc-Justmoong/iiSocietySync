#pragma once
#include "iiSocietySyncExport.h"
#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <optional>

namespace iiSocietySync {
// Untrusted discovery metadata only. Account proof and TLS pairing still decide
// whether a peer may access the container; BLE never carries file payloads.
struct IISOCIETYSYNC_EXPORT NearbyBootstrap {
    QJsonObject record;
    QStringList addresses;
    quint16 port = 0;
    static constexpr qsizetype MaximumBytes = 2000;
    QByteArray encode() const;
    static std::optional<NearbyBootstrap> decode(const QByteArray &bytes);
    static QStringList localAddresses();
};
}
