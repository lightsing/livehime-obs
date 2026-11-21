#include "gift-v2.hpp"
#include "protobuf-reader.hpp"
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>
#include <limits>
namespace livehime {
namespace {
constexpr int MaxGifts = 128;
enum class Schema { Root, Gift, User, Base, Anon, Medal, Material };
struct Field {
    const char *name = nullptr;
    bool number = false;
    bool boolean = false;
    bool message = false;
    Schema child = Schema::Root;
};
Field field(Schema schema, quint32 id) {
    if (schema == Schema::Root) {
        switch (id) {
        case 1:
            return {"uid", true};
        case 2:
            return {"uname"};
        case 3:
            return {"face"};
        case 5:
            return {"guard_level", true};
        case 8:
            return {"medal_info", false, false, true, Schema::Medal};
        case 10:
            return {"gift_list", false, false, true, Schema::Gift};
        case 11:
            return {"switch", true, true};
        case 15:
            return {"sender_uinfo", false, false, true, Schema::User};
        }
    } else if (schema == Schema::Gift) {
        switch (id) {
        case 1:
            return {"giftId", true};
        case 2:
            return {"giftName"};
        case 3:
            return {"num", true};
        case 5:
            return {"price", true};
        case 7:
            return {"total_coin", true};
        case 8:
            return {"coin_type"};
        case 9:
            return {"tid"};
        case 10:
            return {"timestamp", true};
        case 11:
            return {"super_batch_gift_num", true};
        case 12:
            return {"batch_combo_id"};
        case 14:
            return {"combo_total_coin", true};
        case 18:
            return {"action"};
        case 35:
            return {"gift_info", false, false, true, Schema::Material};
        }
    } else if (schema == Schema::User) {
        if (id == 1)
            return {"uid", true};
        if (id == 2)
            return {"base", false, false, true, Schema::Base};
        if (id == 9)
            return {"anon", false, false, true, Schema::Anon};
    } else if (schema == Schema::Base) {
        if (id == 1)
            return {"name"};
        if (id == 2)
            return {"face"};
        if (id == 3)
            return {"is_mystery", true, true};
    } else if (schema == Schema::Anon) {
        if (id == 5)
            return {"masked", true, true};
        // Identity and ciphertext fields remain opaque; their presence is masked by the parent.
    } else if (schema == Schema::Medal) {
        switch (id) {
        case 1:
            return {"target_id", true};
        case 2:
            return {"special"};
        case 3:
            return {"anchor_uname"};
        case 4:
            return {"anchor_roomid", true};
        case 5:
            return {"medal_level", true};
        case 6:
            return {"medal_name"};
        case 7:
            return {"medal_color", true};
        case 8:
            return {"medal_color_start", true};
        case 9:
            return {"medal_color_end", true};
        case 10:
            return {"medal_color_border", true};
        case 11:
            return {"is_lighted", true};
        case 12:
            return {"guard_level", true};
        }
    } else if (schema == Schema::Material) {
        if (id == 1)
            return {"img_basic"};
        if (id == 2)
            return {"webp"};
        if (id == 3)
            return {"effect_id", true};
        if (id == 4)
            return {"has_imaged_gift", true};
        if (id == 5)
            return {"gif"};
    }
    return {};
}
bool parse(QByteArrayView bytes, Schema schema, int depth, int &budget, QJsonObject &result) {
    if (depth > protobuf::MaxDepth)
        return false;
    protobuf::Reader reader{bytes, 0, budget};
    QSet<quint32> seen;
    QJsonArray gifts;
    while (reader.offset < bytes.size()) {
        quint32 id, wire;
        if (!reader.next(id, wire))
            return false;
        const auto spec = field(schema, id);
        if (!spec.name) {
            if (!reader.skip(wire, id, depth))
                return false;
            continue;
        }
        const bool repeated = schema == Schema::Root && id == 10;
        if (!repeated && seen.contains(id))
            return false;
        seen.insert(id);
        QJsonValue value;
        if (spec.number) {
            quint64 number;
            if (wire != 0 || !reader.varint(number) || number > quint64(std::numeric_limits<qint64>::max()))
                return false;
            if (spec.boolean)
                value = number != 0;
            else
                value = QJsonValue(static_cast<qint64>(number));
        } else {
            QByteArrayView raw;
            if (wire != 2 || !reader.slice(raw))
                return false;
            if (spec.message) {
                QJsonObject child;
                if (!parse(raw, spec.child, depth + 1, budget, child))
                    return false;
                if (spec.child == Schema::Anon && !raw.isEmpty())
                    child = {{"masked", true}};
                value = child;
            } else {
                if (raw.size() > 16 * 1024)
                    return false;
                QStringDecoder decoder(QStringDecoder::Utf8);
                const QString text = decoder(raw);
                if (decoder.hasError())
                    return false;
                value = schema == Schema::Anon ? QJsonValue(true) : QJsonValue(text);
            }
        }
        if (repeated) {
            if (gifts.size() >= MaxGifts)
                return false;
            gifts.append(value);
        } else
            result.insert(QLatin1String(spec.name), value);
    }
    if (schema == Schema::Root)
        result.insert("gift_list", gifts);
    return true;
}
} // namespace
QJsonArray decodeGiftV2Payload(const QByteArray &payload) {
    if (payload.isEmpty() || payload.size() > protobuf::MaxPayload)
        return {};
    int budget = protobuf::MaxFields;
    QJsonObject root;
    if (!parse(payload, Schema::Root, 0, budget, root) || !root.value("switch").toBool())
        return {};
    auto user = root.value("sender_uinfo").toObject();
    auto base = user.value("base").toObject();
    const bool anonymous = base.value("is_mystery").toBool() || !user.value("anon").toObject().isEmpty();
    QString name = base.value("name").toString(root.value("uname").toString());
    QString face = base.value("face").toString(root.value("face").toString());
    qint64 uid = user.value("uid").toInteger(root.value("uid").toInteger());
    if (anonymous) {
        name = QStringLiteral("神秘用户");
        face.clear();
        uid = 0;
        user = {{"uid", 0},
                {"base", QJsonObject{{"name", name}, {"face", ""}, {"is_mystery", true}}},
                {"anon", QJsonObject{{"masked", true}}}};
    } else {
        base.insert("name", name);
        base.insert("face", face);
        user.insert("base", base);
        user.insert("uid", uid);
    }
    QJsonArray result;
    qsizetype outputBytes = 0;
    for (const auto &item : root.value("gift_list").toArray()) {
        auto data = item.toObject();
        if (data.value("giftName").toString().isEmpty() || data.value("num").toInteger() <= 0)
            continue;
        data.insert("uid", uid);
        data.insert("uname", name);
        data.insert("face", face);
        data.insert("sender_uinfo", user);
        data.insert("is_mystery", anonymous);
        data.insert("guard_level", root.value("guard_level"));
        if (!anonymous && root.contains("medal_info"))
            data.insert("medal_info", root.value("medal_info"));
        QJsonObject event{{"cmd", "SEND_GIFT"}, {"data", data}};
        const auto size = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
        outputBytes += size;
        if (size > 256 * 1024 || outputBytes > 1024 * 1024)
            return {};
        result.append(event);
    }
    return result;
}
QJsonArray decodeGiftV2(const QJsonObject &event) {
    if (event.value("cmd").toString() != "SEND_GIFT_V2")
        return {};
    const auto value = event.value("data").toObject().value("pb");
    return decodeGiftV2Payload(protobuf::decodeBase64(value));
}
} // namespace livehime
