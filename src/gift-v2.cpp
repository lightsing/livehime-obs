#include "gift-v2.hpp"
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>
#include <limits>
namespace livehime {
namespace {
constexpr qsizetype MaxPayload = 256 * 1024;
constexpr int MaxFields = 4096, MaxGifts = 128, MaxDepth = 8;
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
struct Reader {
    QByteArrayView bytes;
    qsizetype offset = 0;
    int &budget;
    bool varint(quint64 &out) {
        out = 0;
        for (int i = 0; i < 10; ++i) {
            if (offset == bytes.size())
                return false;
            const auto byte = static_cast<quint8>(bytes[offset++]);
            if (i == 9 && byte > 1)
                return false;
            out |= quint64(byte & 127) << (i * 7);
            if (!(byte & 128))
                return true;
        }
        return false;
    }
    bool slice(QByteArrayView &out) {
        quint64 length;
        if (!varint(length) || length > quint64(bytes.size() - offset))
            return false;
        out = bytes.sliced(offset, static_cast<qsizetype>(length));
        offset += static_cast<qsizetype>(length);
        return true;
    }
    bool skip(quint32 wire, quint32 id, int depth) {
        quint64 ignored;
        QByteArrayView value;
        switch (wire) {
        case 0:
            return varint(ignored);
        case 1:
            if (bytes.size() - offset < 8)
                return false;
            offset += 8;
            return true;
        case 2:
            return slice(value);
        case 5:
            if (bytes.size() - offset < 4)
                return false;
            offset += 4;
            return true;
        case 3:
            if (depth >= MaxDepth)
                return false;
            while (offset < bytes.size()) {
                quint64 tag;
                if (--budget < 0 || !varint(tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffff)
                    return false;
                if ((tag & 7) == 4)
                    return (tag >> 3) == id;
                if (!skip(tag & 7, static_cast<quint32>(tag >> 3), depth + 1))
                    return false;
            }
            return false;
        default:
            return false;
        }
    }
};
bool parse(QByteArrayView bytes, Schema schema, int depth, int &budget, QJsonObject &result) {
    if (depth > MaxDepth)
        return false;
    Reader reader{bytes, 0, budget};
    QSet<quint32> seen;
    QJsonArray gifts;
    while (reader.offset < bytes.size()) {
        quint64 tag;
        if (--budget < 0 || !reader.varint(tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffff)
            return false;
        const auto id = static_cast<quint32>(tag >> 3), wire = static_cast<quint32>(tag & 7);
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
    if (payload.isEmpty() || payload.size() > MaxPayload)
        return {};
    int budget = MaxFields;
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
    if (!value.isString())
        return {};
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > ((MaxPayload + 2) / 3) * 4 || text.size() % 4)
        return {};
    for (const auto c : text)
        if (c.unicode() > 127)
            return {};
    const auto encoded = text.toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != encoded)
        return {};
    return decodeGiftV2Payload(decoded.decoded);
}
} // namespace livehime
