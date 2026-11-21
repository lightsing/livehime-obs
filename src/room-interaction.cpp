#include "room-interaction.hpp"
#include "protobuf-reader.hpp"
#include <QStringDecoder>
#include <limits>
namespace livehime {
namespace {
enum class Part { Event, User, Base };
struct Interaction {
    QString name, baseName;
    qint64 type = 0, timestamp = 0, room = 0;
    bool anonymous = false;
};
bool parseFields(QByteArrayView bytes, Part part, Interaction &event, int &budget) {
    protobuf::Reader reader{bytes, 0, budget};
    while (reader.offset < bytes.size()) {
        quint32 id, wire;
        if (!reader.next(id, wire))
            return false;
        if ((part == Part::Event && (id == 5 || id == 6 || id == 7 || id == 21)) ||
            (part == Part::Base && id == 4)) {
            quint64 value;
            if (wire != 0 || !reader.varint(value) || value > quint64(std::numeric_limits<qint64>::max()))
                return false;
            if (part == Part::Base || id == 21)
                event.anonymous |= value != 0;
            else if (id == 5)
                event.type = static_cast<qint64>(value);
            else if (id == 6)
                event.room = static_cast<qint64>(value);
            else
                event.timestamp = static_cast<qint64>(value);
        } else if ((part == Part::Event && id == 2) || (part == Part::Base && id == 1)) {
            QByteArrayView name;
            if (wire != 2 || !reader.slice(name) || name.size() > 16 * 1024)
                return false;
            QStringDecoder decoder(QStringDecoder::Utf8);
            QString text = decoder(name);
            if (decoder.hasError())
                return false;
            (part == Part::Base ? event.baseName : event.name) = text.left(120);
        } else if ((part == Part::Event && id == 22) || (part == Part::User && (id == 2 || id == 9))) {
            QByteArrayView child;
            if (wire != 2 || !reader.slice(child))
                return false;
            if (part == Part::User && id == 9)
                event.anonymous |= !child.isEmpty();
            else if (!parseFields(child, part == Part::Event ? Part::User : Part::Base, event, budget))
                return false;
        } else if (!reader.skip(wire, id, static_cast<int>(part)))
            return false;
    }
    return true;
}
bool marked(const QJsonValue &v) { return v.toBool() || v.toInteger() != 0; }
qint64 integer(const QJsonValue &v) { return v.isString() ? v.toString().toLongLong() : v.toInteger(); }
bool differentRoom(const QJsonObject &object, qint64 roomId) {
    for (auto key : {"roomid", "room_id"}) {
        const auto room = integer(object.value(QLatin1String(key)));
        if (room && room != roomId)
            return true;
    }
    return false;
}
} // namespace
std::optional<QJsonObject> roomInteraction(const QJsonObject &event, qint64 roomId) {
    const auto cmd = event.value("cmd").toString().section(':', 0, 0);
    const auto data = event.value("data").toObject();
    if (differentRoom(event, roomId) || differentRoom(data, roomId))
        return {};
    Interaction interaction;
    if (cmd == "INTERACT_WORD_V2") {
        const auto payload = protobuf::decodeBase64(data.value("pb"));
        int budget = protobuf::MaxFields;
        if (payload.isEmpty() || !parseFields(payload, Part::Event, interaction, budget) ||
            (interaction.room && interaction.room != roomId))
            return {};
    } else if (cmd == "INTERACT_WORD") {
        QJsonObject user;
        for (auto key : {"uinfo", "sender_uinfo", "user", "user_info"}) {
            user = data.value(QLatin1String(key)).toObject();
            if (!user.isEmpty())
                break;
        }
        const auto base = user.value("base").toObject();
        interaction.type = integer(data.value("msg_type"));
        interaction.timestamp = integer(data.value("timestamp"));
        interaction.name = data.value("uname").toString(
            data.value("name").toString(data.value("username").toString())).left(120);
        interaction.baseName = base.value("name").toString().left(120);
        interaction.anonymous = marked(base.value("is_mystery")) || marked(user.value("is_mystery")) ||
                                !user.value("anon").toObject().isEmpty();
    } else
        return {};
    interaction.anonymous |= marked(data.value("is_mystery")) || !data.value("anon").toObject().isEmpty();
    if (interaction.type <= 0)
        return {};
    auto name = interaction.baseName.isEmpty() ? interaction.name : interaction.baseName;
    if (interaction.anonymous)
        name = QStringLiteral("神秘用户");
    else if (name.isEmpty())
        name = QStringLiteral("观众");
    QJsonObject normalized{{"msg_type", interaction.type}, {"timestamp", interaction.timestamp},
                           {"uname", name}, {"is_mystery", interaction.anonymous}};
    for (auto key : {"tid", "id_str"}) {
        const auto id = data.value(QLatin1String(key)).toString();
        if (!id.isEmpty() && id.size() <= 128)
            normalized.insert(QLatin1String(key), id);
    }
    return normalized;
}
} // namespace livehime
