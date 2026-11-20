#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUrl>
#include <optional>

namespace livehime {
struct ModerationAction {
    QString text;
    QUrl url;
};

struct ModerationNotice {
    enum class Kind { Warning, CutOff, Ecology, CutOffV2 };
    Kind kind = Kind::Warning;
    QString title;
    QString body;
    QList<ModerationAction> actions;
    QString id;
    QList<QList<ModerationAction>> tips;
    qint64 timestamp = 0;
};

std::optional<ModerationNotice> parseModerationNotice(const QJsonObject &event, qint64 roomId);
} // namespace livehime
