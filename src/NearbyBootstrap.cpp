#include "NearbyBootstrap.h"
#include <LanPeer.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

namespace iiSocietySync {
namespace {
bool valid(const NearbyBootstrap &value) {
    static const QRegularExpression token("\\A[A-Za-z0-9_.:-]{1,128}\\z"), hash("\\A[a-f0-9]{64}\\z"), controls("[\\x00-\\x1f\\x7f]");
    const auto &r = value.record;
    const QSet<QString> fields{"v", "scope", "id", "name", "kind", "host", "nonce", "epoch", "proof", "autoHost", "primary"};
    for (auto it = r.begin(); it != r.end(); ++it)
        if (!fields.contains(it.key()) || !it.value().isString()) return false;
    if (!value.port || value.addresses.isEmpty() || value.addresses.size() > 8
        || r.value("v") != "1" || !hash.match(r.value("scope").toString()).hasMatch()
        || !token.match(r.value("id").toString()).hasMatch() || !token.match(r.value("nonce").toString()).hasMatch()
        || r.value("name").toString().isEmpty() || r.value("name").toString().size() > 128
        || r.value("name").toString().contains(controls)
        || !QStringList{"pc", "phone", "tablet"}.contains(r.value("kind").toString())
        || !QStringList{"0", "1"}.contains(r.value("host").toString())
        || QJsonDocument(r).toJson(QJsonDocument::Compact).size() > 1024) return false;
    for (const auto &address : value.addresses)
        if (!iiServerHost::LanLink::localAddress(address) || QHostAddress(address).isLoopback()) return false;
    return true;
}
}
QByteArray NearbyBootstrap::encode() const {
    if (!valid(*this)) return {};
    const auto json = QJsonDocument(QJsonObject{{"v", 1}, {"record", record},
        {"addresses", QJsonArray::fromStringList(addresses)}, {"port", port}}).toJson(QJsonDocument::Compact);
    // A digest detects GATT pages read across an advertisement refresh. It is
    // deliberately not an authentication proof.
    QByteArray bytes("SSB1"); bytes += QCryptographicHash::hash(json, QCryptographicHash::Sha256); bytes += json;
    return bytes.size() <= MaximumBytes ? bytes : QByteArray();
}
std::optional<NearbyBootstrap> NearbyBootstrap::decode(const QByteArray &bytes) {
    if (bytes.size() <= 36 || bytes.size() > MaximumBytes || !bytes.startsWith("SSB1")) return {};
    const auto json = bytes.mid(36);
    if (QCryptographicHash::hash(json, QCryptographicHash::Sha256) != bytes.mid(4, 32)) return {};
    QJsonParseError error; const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto object = document.object();
    if (object.size() != 4 || object.value("v") != 1 || !object.value("record").isObject()
        || !object.value("addresses").isArray()) return {};
    const auto port = object.value("port").toInt();
    if (port <= 0 || port > 65535 || object.value("port").toDouble() != port) return {};
    NearbyBootstrap result{object.value("record").toObject(), {}, quint16(port)};
    for (const auto &address : object.value("addresses").toArray()) {
        if (!address.isString()) return {};
        result.addresses.append(address.toString());
    }
    return valid(result) ? std::optional(result) : std::nullopt;
}
QStringList NearbyBootstrap::localAddresses() {
    QStringList result;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || flags.testFlag(QNetworkInterface::IsPointToPoint)) continue;
        for (const auto &entry : interface.addressEntries()) {
            const auto address = entry.ip().toString();
            if (!entry.ip().isLoopback() && iiServerHost::LanLink::localAddress(address) && !result.contains(address)) result.append(address);
        }
    }
    return result.mid(0, 8);
}
}
