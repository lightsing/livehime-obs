#pragma once
#include <QJsonObject>
#include <optional>

namespace livehime {
// Normalizes an INTERACT_WORD or INTERACT_WORD_V2 into privacy-safe fields for the current room.
std::optional<QJsonObject> roomInteraction(const QJsonObject &event, qint64 roomId);
} // namespace livehime
