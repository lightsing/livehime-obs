#include "gift-v2.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cstdio>
#include <limits>
using namespace livehime;
static void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static QByteArray varint(quint64 n) {
    QByteArray b;
    do {
        const auto v = n & 127;
        n >>= 7;
        b += char(v | (n ? 128 : 0));
    } while (n);
    return b;
}
static QByteArray number(int field, quint64 n) { return varint(quint64(field) << 3) + varint(n); }
static QByteArray bytes(int field, const QByteArray &b) {
    return varint((quint64(field) << 3) | 2) + varint(b.size()) + b;
}
static QByteArray gift(quint64 count = 2) {
    return number(1, 3) + bytes(2, "synthetic gift") + number(3, count) + number(7, 42) + bytes(8, "gold");
}
static QByteArray root(const QByteArray &g) { return number(11, 1) + bytes(10, g); }
static QJsonObject event(const QByteArray &payload) {
    return {{"cmd", "SEND_GIFT_V2"}, {"data", QJsonObject{{"pb", QString::fromLatin1(payload.toBase64())}}}};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto path =
        argc > 1
            ? QString::fromLocal8Bit(argv[1])
            : QFileInfo(QString::fromUtf8(__FILE__)).absoluteDir().filePath("data/gift-v2-synthetic.json");
    QFile fixtures(path);
    check(fixtures.open(QIODevice::ReadOnly), "independent fixture file");
    for (const auto &entry : QJsonDocument::fromJson(fixtures.readAll()).array()) {
        const auto fixture = entry.toObject();
        const auto result = decodeGiftV2(fixture.value("event").toObject());
        check(result.size() == fixture.value("expected_gift_count").toInt(),
              "Google protobuf fixture gift count");
        check(result[0].toObject().value("cmd") == "SEND_GIFT", "normalized command");
        check(result[0].toObject().value("data").toObject().value("coin_type") == "gold" &&
                  result[1].toObject().value("data").toObject().value("coin_type") == "silver",
              "gift currencies preserved");
        if (fixture.value("expected_anonymous").toBool()) {
            const auto serialized = QJsonDocument(result).toJson();
            check(!serialized.contains("original") && !serialized.contains("synthetic.png") &&
                      !serialized.contains("opaque-synthetic-id"),
                  "anonymous identity removed");
            for (const auto &item : result) {
                const auto data = item.toObject().value("data").toObject();
                check(data.value("is_mystery").toBool() && data.value("uid").toInteger() == 0 &&
                          data.value("face").toString().isEmpty(),
                      "anonymous safe fields");
            }
        }
    }
    const auto valid = root(gift());
    check(decodeGiftV2Payload(valid).size() == 1, "minimal gift");
    // 8.15 adds fields inside BlindGift, UserInfo and GiftItem.benefits.
    const auto blindGift = number(6, 81) + number(7, 82);
    const auto fansLightAnimation = bytes(1, "animation medal");
    const auto benefits = bytes(2, fansLightAnimation);
    const auto senderMedal = bytes(1, "sender medal");
    const auto user = number(1, 42) + bytes(2, bytes(1, "private-sender") + bytes(2, "private-face"));
    const auto updatedGift = root(gift() + bytes(38, benefits)) + bytes(9, blindGift);
    const auto existingRow = decodeGiftV2(event(valid + bytes(15, user)));
    const auto updatedRow = decodeGiftV2(event(updatedGift + bytes(15, user + bytes(3, senderMedal))));
    check(existingRow.size() == 1 && updatedRow == existingRow,
          "8.15 gift extensions retain displayed fields");
    const auto anon = bytes(9, number(5, 1));
    const auto existingAnonRow = decodeGiftV2(event(valid + bytes(15, user + anon)));
    const auto updatedAnonRow =
        decodeGiftV2(event(updatedGift + bytes(15, user + bytes(3, senderMedal) + anon)));
    check(existingAnonRow.size() == 1 && updatedAnonRow == existingAnonRow &&
              !QJsonDocument(updatedAnonRow).toJson().contains("private-"),
          "8.15 anonymous gift extensions preserve masking");
    check(decodeGiftV2Payload(bytes(10, gift())).isEmpty(), "missing switch suppresses display");
    check(decodeGiftV2Payload(number(11, 0) + bytes(10, gift())).isEmpty(),
          "false switch suppresses display");
    const quint64 exact = 9007199254740993ULL;
    check(decodeGiftV2Payload(root(gift(exact)))[0]
                  .toObject()
                  .value("data")
                  .toObject()
                  .value("num")
                  .toInteger() == qint64(exact),
          "int64 precision");
    const auto anonOnly = valid + bytes(2, "secret-name") + bytes(3, "secret-face") + number(1, 123) +
                          bytes(15, bytes(2, bytes(1, "secret-base")) + bytes(9, bytes(2, "secret-anon")));
    const auto anonymous = decodeGiftV2Payload(anonOnly);
    check(anonymous.size() == 1 && !QJsonDocument(anonymous).toJson().contains("secret"),
          "anon-only hides identity");
    const auto fullAnon = valid + bytes(15, number(1, 123) + bytes(2, bytes(1, "secret-name")) +
                                                bytes(9, number(1, 1) + number(2, 4) + number(3, 2) +
                                                             bytes(4, "secret-cipher") + number(5, 1)));
    const auto fullAnonymous = decodeGiftV2Payload(fullAnon);
    check(fullAnonymous.size() == 1 && !QJsonDocument(fullAnonymous).toJson().contains("secret"),
          "opaque anonymous fields accept wire types without identity leakage");
    auto unknown = valid + number(100, 42) + varint((101 << 3) | 1) + QByteArray(8, 'x') +
                   bytes(102, "opaque") + varint((103 << 3) | 5) + QByteArray(4, 'x');
    unknown += varint((104 << 3) | 3) + number(1, 2) + varint((104 << 3) | 4);
    check(decodeGiftV2Payload(unknown).size() == 1, "unknown fields and groups skipped");
    check(decodeGiftV2Payload(valid + char(0)).isEmpty(), "zero field rejected atomically");
    check(decodeGiftV2Payload(valid + varint((100 << 3) | 6)).isEmpty(), "invalid wire rejected");
    check(decodeGiftV2Payload(valid + bytes(10, gift()).chopped(1)).isEmpty(),
          "truncated tail produces no partial gifts");
    check(decodeGiftV2Payload(root(gift(std::numeric_limits<quint64>::max()))).isEmpty(),
          "signed quantity overflow rejected");
    check(decodeGiftV2Payload(valid + QByteArray(11, char(-128))).isEmpty(), "varint overflow");
    check(decodeGiftV2Payload(root(bytes(2, QByteArray::fromHex("c0af")))).isEmpty(), "invalid UTF8");
    check(decodeGiftV2Payload(root(bytes(2, QByteArray(16385, 'x')))).isEmpty(), "string limit");
    check(decodeGiftV2Payload(QByteArray(256 * 1024 + 1, 'x')).isEmpty(), "payload limit");
    QByteArray many = number(11, 1);
    for (int i = 0; i < 129; ++i)
        many += bytes(10, gift());
    check(decodeGiftV2Payload(many).isEmpty(), "gift count bounded");
    QByteArray fields = valid;
    for (int i = 0; i < 4096; ++i)
        fields += number(100, 1);
    check(decodeGiftV2Payload(fields).isEmpty(), "field budget");
    QByteArray deep;
    for (int i = 0; i < 10; ++i)
        deep += varint((100 << 3) | 3);
    for (int i = 0; i < 10; ++i)
        deep += varint((100 << 3) | 4);
    check(decodeGiftV2Payload(valid + deep).isEmpty(), "group depth budget");
    check(decodeGiftV2(event(valid)).size() == 1, "base64 wrapper");
    for (const QString bad :
         {QStringLiteral("AAAA "), QStringLiteral("!!!!"), QStringLiteral("AB=="), QString(400000, 'A')})
        check(decodeGiftV2({{"cmd", "SEND_GIFT_V2"}, {"data", QJsonObject{{"pb", bad}}}}).isEmpty(),
              "strict bounded Base64");
    std::puts("Gift V2 wire, privacy, bounds and independent protobuf fixtures passed");
}
