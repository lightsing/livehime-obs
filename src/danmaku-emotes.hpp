#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>

namespace livehime {
struct DanmakuEmote {
    int start = 0;
    int length = 0;
    QString url; // Empty when the message identifies an emote without a usable image URL.
    bool standalone = false;
};

// Offsets and lengths refer to UTF-16 code units in the unchanged message body.
QList<DanmakuEmote> danmakuEmotes(const QString &body, const QJsonArray &meta, const QJsonObject &extra);
} // namespace livehime

Q_DECLARE_METATYPE(livehime::DanmakuEmote)
