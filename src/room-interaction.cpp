#include "room-interaction.hpp"
#include "protobuf-reader.hpp"
#include <QStringDecoder>
#include <limits>

namespace livehime {
namespace {
enum class Part { Event, User, Base, Wealth, FansMedal, UserMedal, GroupMedal };
struct Field {
    const char *name = nullptr;
    enum Type { Number, Text, Message, Mask } type = Number;
    Part child = Part::Event;
};
// InteractWord and user_dagw.UserInfo use different layouts for their fan medals.
Field field(Part part, quint32 id) {
    switch (part) {
    case Part::Event:
        switch (id) {
        case 1: return {"uid"};
        case 2: return {"uname", Field::Text};
        case 3: return {"uname_color", Field::Text};
        case 5: return {"msg_type"};
        case 6: return {"roomid"};
        case 7: return {"timestamp"};
        case 9: return {"fans_medal", Field::Message, Part::FansMedal};
        case 18: return {"tail_text", Field::Text};
        case 20: return {"group_medal", Field::Message, Part::GroupMedal};
        case 21: return {"is_mystery"};
        case 22: return {"uinfo", Field::Message, Part::User};
        }
        break;
    case Part::User:
        switch (id) {
        case 1: return {"uid"};
        case 2: return {"base", Field::Message, Part::Base};
        case 3: return {"medal", Field::Message, Part::UserMedal};
        case 4: return {"wealth", Field::Message, Part::Wealth};
        case 9: return {"anon", Field::Mask};
        }
        break;
    case Part::Base:
        if (id == 1) return {"name", Field::Text};
        if (id == 4) return {"is_mystery"};
        if (id == 8) return {"name_color_str", Field::Text};
        break;
    case Part::Wealth:
        if (id == 1) return {"level"};
        break;
    case Part::FansMedal:
        switch (id) {
        case 2: return {"medal_level"};
        case 3: return {"medal_name", Field::Text};
        case 4: return {"medal_color"};
        case 5: return {"medal_color_start"};
        }
        break;
    case Part::UserMedal:
        switch (id) {
        case 1: return {"name", Field::Text};
        case 2: return {"level"};
        case 3: return {"color_start"};
        case 6: return {"color"};
        case 8: return {"typ"};
        case 15: return {"v2_medal_color_start", Field::Text};
        }
        break;
    case Part::GroupMedal:
        if (id == 2) return {"name", Field::Text};
        break;
    }
    return {};
}
bool parseFields(QByteArrayView bytes, Part part, QJsonObject &out, int &budget, int depth = 0) {
    protobuf::Reader reader{bytes, 0, budget};
    while (reader.offset < bytes.size()) {
        quint32 id, wire;
        if (!reader.next(id, wire)) return false;
        const auto spec = field(part, id);
        if (!spec.name) {
            if (!reader.skip(wire, id, depth)) return false;
            continue;
        }
        const QLatin1String key(spec.name);
        if (out.contains(key)) return false;
        if (spec.type == Field::Number) {
            quint64 value;
            if (wire != 0 || !reader.varint(value) || value > quint64(std::numeric_limits<qint64>::max()))
                return false;
            out.insert(key, static_cast<qint64>(value));
            continue;
        }
        QByteArrayView raw;
        if (wire != 2 || !reader.slice(raw)) return false;
        if (spec.type == Field::Mask) {
            out.insert(key, raw.isEmpty() ? QJsonObject{} : QJsonObject{{"masked", true}});
        } else if (spec.type == Field::Message) {
            QJsonObject child;
            if (!parseFields(raw, spec.child, child, budget, depth + 1)) return false;
            out.insert(key, child);
        } else {
            if (raw.size() > 16 * 1024) return false;
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString text = decoder(raw);
            if (decoder.hasError()) return false;
            out.insert(key, text.left(120));
        }
    }
    return true;
}
bool marked(const QJsonValue &v) {
    return v.toBool() || v.toInteger() != 0 || v.toString() == "1" || v.toString() == "true";
}
qint64 integer(const QJsonValue &v) { return v.isString() ? v.toString().toLongLong() : v.toInteger(); }
bool differentRoom(const QJsonObject &object, qint64 roomId) {
    for (auto key : {"roomid", "room_id"}) {
        const auto room = integer(object.value(QLatin1String(key)));
        if (room && room != roomId) return true;
    }
    return false;
}
qint64 rgb(const QJsonValue &value) {
    const auto text = value.toString();
    const auto color = text.size() == 7 && text.startsWith('#') ? text.sliced(1).toLongLong(nullptr, 16) : integer(value);
    return color > 0 && color <= 0xffffff ? color : 0;
}
QJsonObject fanBadge(const QJsonObject &m, bool modern) {
    const auto name = m.value(modern ? "name" : "medal_name").toString().trimmed().left(64);
    const auto level = integer(m.value(modern ? "level" : "medal_level"));
    if (name.isEmpty() || level <= 0) return {};
    auto color = modern ? rgb(m.value("v2_medal_color_start")) : 0;
    if (!color) color = rgb(m.value(modern ? "color_start" : "medal_color_start"));
    if (!color) color = rgb(m.value(modern ? "color" : "medal_color"));
    return {{"medal_name", name}, {"medal_level", level}, {"medal_color_start", color}};
}
} // namespace
std::optional<QJsonObject> roomInteraction(const QJsonObject &event, qint64 roomId) {
    const auto cmd = event.value("cmd").toString().section(':', 0, 0);
    const auto envelope = event.value("data").toObject();
    auto data = envelope;
    if (differentRoom(event, roomId) || differentRoom(envelope, roomId)) return {};
    if (cmd == "INTERACT_WORD_V2") {
        const auto payload = protobuf::decodeBase64(envelope.value("pb"));
        int budget = protobuf::MaxFields;
        data = {};
        if (payload.isEmpty() || !parseFields(payload, Part::Event, data, budget) || differentRoom(data, roomId)) return {};
    } else if (cmd != "INTERACT_WORD") return {};
    const auto type = integer(data.value("msg_type"));
    if (type <= 0) return {};
    auto anonymous = marked(data.value("is_mystery")) || !data.value("anon").toObject().isEmpty() ||
                     marked(envelope.value("is_mystery")) || !envelope.value("anon").toObject().isEmpty();
    qint64 uid = integer(data.value("uid")), glory = 0;
    bool conflicting = false;
    QString name, nameColor;
    QJsonObject medal;
    QString group = data.value("group_medal").toObject().value("name").toString();
    for (auto key : {"uinfo", "sender_uinfo", "user", "user_info"}) {
        const auto user = data.value(QLatin1String(key)).toObject();
        const auto base = user.value("base").toObject();
        const auto nested = integer(user.value("uid"));
        if (nested > 0) {
            if (uid > 0 && uid != nested) conflicting = true;
            uid = nested;
        }
        anonymous |= marked(base.value("is_mystery")) || marked(user.value("is_mystery")) || !user.value("anon").toObject().isEmpty();
        if (name.isEmpty()) name = base.value("name").toString();
        if (nameColor.isEmpty()) nameColor = base.value("name_color_str").toString();
        if (glory <= 0) glory = integer(user.value("wealth").toObject().value("level"));
        const auto modern = user.value("medal").toObject();
        if (integer(modern.value("typ")) == 1) group = modern.value("name").toString(group);
        else if (medal.isEmpty()) medal = fanBadge(modern, true);
    }
    if (name.isEmpty()) name = data.value("uname").toString(data.value("name").toString(data.value("username").toString()));
    if (nameColor.isEmpty()) nameColor = data.value("uname_color").toString();
    if (glory <= 0) glory = integer(data.value("wealth_level"));
    if (medal.isEmpty()) medal = fanBadge(data.value("fans_medal").toObject(), false);
    auto tail = data.value("tail_text").toString().trimmed();
    if (tail.isEmpty()) tail = group.trimmed();
    QJsonObject normalized{{"msg_type", type}, {"timestamp", integer(data.value("timestamp"))}, {"is_mystery", anonymous},
                           {"uname", anonymous ? QStringLiteral("神秘用户") : name.isEmpty() ? QStringLiteral("观众") : name.left(120)}};
    if (!anonymous) {
        if (uid > 0 && !conflicting) normalized.insert("uid", uid);
        if (glory > 0) normalized.insert("wealth_level", glory);
        if (!medal.isEmpty()) normalized.insert("fans_medal", medal);
        if (const auto color = rgb(nameColor))
            normalized.insert("uname_color", QStringLiteral("#%1").arg(color, 6, 16, QLatin1Char('0')));
        if (!tail.isEmpty()) normalized.insert("tail_text", tail.left(64));
    }
    for (auto key : {"tid", "id_str"}) {
        const auto id = envelope.value(QLatin1String(key)).toString();
        if (!id.isEmpty() && id.size() <= 128) normalized.insert(QLatin1String(key), id);
    }
    return normalized;
}
} // namespace livehime
