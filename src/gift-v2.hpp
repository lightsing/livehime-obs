#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
namespace livehime {
// Descriptor-derived, bounded decoder. Invalid input produces no partial events.
QJsonArray decodeGiftV2Payload(const QByteArray &payload);
QJsonArray decodeGiftV2(const QJsonObject &event);
} // namespace livehime
