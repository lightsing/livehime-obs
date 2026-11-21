#pragma once
#include <QByteArray>
#include <QJsonValue>

namespace livehime::protobuf {
constexpr qsizetype MaxPayload = 256 * 1024;
constexpr int MaxDepth = 8, MaxFields = 4096;

struct Reader {
    QByteArrayView bytes;
    qsizetype offset = 0;
    int &budget;
    bool next(quint32 &id, quint32 &wire) {
        quint64 tag;
        if (--budget < 0 || !varint(tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffff)
            return false;
        id = static_cast<quint32>(tag >> 3);
        wire = static_cast<quint32>(tag & 7);
        return true;
    }
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
inline QByteArray decodeBase64(const QJsonValue &value) {
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
    if (!decoded || decoded.decoded.size() > MaxPayload || decoded.decoded.toBase64() != encoded)
        return {};
    return decoded.decoded;
}
} // namespace livehime::protobuf
