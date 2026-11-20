#include "moderation-notice.hpp"
#include "protocol.hpp"

#include <QJsonArray>
#include <QStringList>
#include <algorithm>

namespace livehime {
namespace {
qint64 roomNumber(const QJsonValue &value)
{
    return value.isString() ? value.toString().toLongLong() : value.toInteger();
}

bool matchesRoom(const QJsonObject &object, qint64 roomId)
{
    for (const auto key : {"roomid", "room_id"}) {
        const auto value = object.value(key);
        if (!value.isUndefined() && roomNumber(value) != roomId)
            return false;
    }
    return true;
}

QString textField(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

void addAction(ModerationNotice &notice, const QString &text, const QString &link)
{
    if (text.isEmpty())
        return;
    const auto url = protocol::safeUserUrl(link);
    if (link.isEmpty() || !url.isEmpty())
        notice.actions.append({text, url});
}

void readDialog(const QJsonObject &dialog, const QString &prefix, ModerationNotice &notice)
{
    notice.title = textField(dialog, prefix + "title");
    QStringList lines;
    for (const auto &value : dialog.value(prefix + "message_list").toArray()) {
        const auto message = value.toObject();
        const auto type = message.value("type").toInt();
        if (type != 1 && type != 2)
            continue;
        const auto label = type == 1 ? textField(message, "label") : QString{};
        const auto content = textField(message, "content");
        if (label.isEmpty() && content.isEmpty())
            continue;
        if (label.isEmpty() || content.isEmpty() || label.endsWith(':') || label.endsWith(QChar(0xff1a)))
            lines.append(label + content);
        else
            lines.append(label + QStringLiteral("：") + content);
    }
    for (const auto &value : dialog.value(prefix + "tip_list").toArray()) {
        const auto tip = value.toObject();
        if (!tip.value("show_platform").toArray().contains(4))
            continue;
        QList<ModerationAction> fragments;
        for (const auto &part : tip.value("message_list").toArray()) {
            const auto item = part.toObject();
            const auto type = item.value("type").toInt();
            const auto content = item.value("content").toString();
            if ((type == 1 || type == 2) && !content.isEmpty()) {
                const auto link = type == 2 ? protocol::safeUserUrl(textField(item, "link_url")) : QUrl{};
                fragments.append({content, link});
            }
        }
        if (std::any_of(fragments.cbegin(), fragments.cend(), [](const ModerationAction &part) {
                return !part.text.trimmed().isEmpty();
            }))
            notice.tips.append(fragments);
    }
    notice.body = lines.join('\n');
    int buttons = 0;
    for (const auto &value : dialog.value(prefix + "button_list").toArray()) {
        const auto button = value.toObject();
        if (!button.value("show_platform").toArray().contains(4))
            continue;
        const int action = button.value("button_action").toInt();
        const auto link = textField(button, "button_link_url");
        if (action == 1)
            addAction(notice, textField(button, "button_text"), {});
        else if (action == 2 && !link.isEmpty())
            addAction(notice, textField(button, "button_text"), link);
        if (++buttons == 2)
            break;
    }
}

QString identity(const QJsonObject &event, const QString &command)
{
    const auto id = event.value("msg_id");
    if (id.isString() && !id.toString().trimmed().isEmpty())
        return command + '/' + id.toString();
    if (id.toInteger() > 0)
        return command + '/' + QString::number(id.toInteger());
    return {};
}
} // namespace

std::optional<ModerationNotice> parseModerationNotice(const QJsonObject &event, qint64 roomId)
{
    const auto command = event.value("cmd").toString();
    const bool warning = command == "WARNING";
    const bool cutOff = command == "CUT_OFF";
    const bool ecology = command == "ANCHOR_ECOLOGY_LIVING_DIALOG";
    if (!warning && !cutOff && !ecology && command != "CUT_OFF_V2")
        return std::nullopt;
    const auto data = event.value("data").toObject();
    if (roomId <= 0 || !matchesRoom(event, roomId) || !matchesRoom(data, roomId))
        return std::nullopt;

    ModerationNotice notice;
    if (warning || cutOff) {
        if (roomNumber(event.value("roomid")) != roomId || !event.value("msg").isString())
            return std::nullopt;
        notice.kind = warning ? ModerationNotice::Kind::Warning : ModerationNotice::Kind::CutOff;
        notice.title = QStringLiteral("提示");
        notice.body = textField(event, "msg");
        if (cutOff && event.value("category").toInt() == 1) {
            const auto pop = event.value("pop").toObject();
            const auto title = textField(pop, "title");
            const auto message = textField(pop, "msg");
            if (!title.isEmpty()) notice.title = title;
            if (!message.isEmpty()) notice.body = message;
            const auto link = textField(pop, "button_url");
            if (!link.isEmpty())
                addAction(notice, textField(pop, "button_text"), link);
        }
        if (notice.body.isEmpty()) {
            if (warning)
                return std::nullopt;
            notice.body = QStringLiteral("流被切断！");
        }
    } else {
        notice.kind = ecology ? ModerationNotice::Kind::Ecology : ModerationNotice::Kind::CutOffV2;
        notice.timestamp = data.value("timestamp").toInteger();
        if (!ecology && data.value("cut_off_version").toInt() != 1)
            return std::nullopt;
        const auto dialog = ecology ? data : data.value("cut_off_data").toObject();
        if (dialog.isEmpty() || !matchesRoom(dialog, roomId))
            return std::nullopt;
        readDialog(dialog, ecology ? QStringLiteral("dialog_") : QStringLiteral("cut_off_"), notice);
        if (notice.title.isEmpty() && notice.body.isEmpty() && notice.tips.isEmpty())
            return std::nullopt;
        if (notice.title.isEmpty())
            notice.title = QStringLiteral("提示");
    }
    notice.id = identity(event, command);
    return notice;
}
} // namespace livehime
