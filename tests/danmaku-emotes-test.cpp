#include "danmaku-emotes.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QVariant>
#include <cstdio>
#include <cstdlib>

using namespace livehime;

static void check(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString rabbit = QStringLiteral("[兔兔]");
    const QString star = QStringLiteral("[星]");
    const QString rabbitUrl = QStringLiteral("https://i0.hdslb.com/bfs/live/rabbit.png");
    const QString starUrl = QStringLiteral("https://i2.hdslb.com/bfs/live/star.webp");
    const QJsonObject rabbitItem{{"descript", rabbit}, {"url", rabbitUrl}, {"emoticon_unique", "room_123_42"}};
    const QJsonObject starItem{{"descript", star}, {"url", starUrl}};
    const QJsonObject inlineExtra{{"emots", QJsonObject{{rabbit, rabbitItem}, {"room_123_7", starItem}}}};
    const QString body = QStringLiteral("🌟[兔兔]+[兔兔][星][未匹配]room_123_7");
    const auto spans = danmakuEmotes(body, {}, inlineExtra);
    check(spans.size() == 3, "match all declared tokens and leave unknown labels and internal IDs as text");
    check(spans[0].start == 2 && spans[0].length == rabbit.size() && spans[0].url == rabbitUrl &&
          spans[1].start == 7 && spans[1].length == rabbit.size() && spans[1].url == rabbitUrl &&
          spans[2].start == 11 && spans[2].length == star.size() && spans[2].url == starUrl &&
          !spans[0].standalone && !spans[1].standalone && !spans[2].standalone,
          "inline ranges are UTF-16 offsets in the original message, including repeated and adjacent tokens");
    check(QVariant::fromValue(spans).value<QList<DanmakuEmote>>().size() == spans.size(),
          "model roles can carry parsed emote spans");
    check(danmakuEmotes(QStringLiteral("兔兔 room_123_42"), {}, inlineExtra).isEmpty(),
          "ordinary words and emoticon IDs do not trigger downloads");
    check(danmakuEmotes(rabbit, {}, {}).isEmpty(), "bracketed text alone is not an emote");

    QJsonArray meta;
    while (meta.size() < 14) meta.append(QJsonValue::Null);
    meta[12] = 1;
    meta[13] = QJsonObject{{"url", starUrl}, {"is_dynamic", 1}, {"height", 96}, {"width", 96}};
    const QString entireBody = QStringLiteral("房间专属表情");
    const auto standalone = danmakuEmotes(entireBody, meta, {});
    check(standalone.size() == 1 && standalone.first().standalone && standalone.first().start == 0 &&
          standalone.first().length == entireBody.size() && standalone.first().url == starUrl,
          "info[0][12] type one and info[0][13].url describe a full-message emote");
    check(danmakuEmotes({}, meta, {}).isEmpty(), "empty bodies have no zero-length ranges");
    const QString optionsJson = QString::fromUtf8(QJsonDocument(meta.at(13).toObject()).toJson(QJsonDocument::Compact));
    meta[13] = optionsJson;
    const auto serialized = danmakuEmotes(entireBody, meta, {});
    check(serialized.size() == 1 && serialized.first().standalone && serialized.first().start == 0 &&
          serialized.first().length == entireBody.size() && serialized.first().url == starUrl,
          "serialized standalone options provide the same image and range as an object");
    auto hasStandaloneWithoutImage = [&] {
        const auto parsed = danmakuEmotes(entireBody, meta, inlineExtra);
        return parsed.size() == 1 && parsed.first().start == 0 && parsed.first().length == entireBody.size() &&
               parsed.first().standalone && parsed.first().url.isEmpty();
    };
    meta[13] = QStringLiteral("[]");
    check(hasStandaloneWithoutImage(), "the standalone marker survives non-object serialized options");
    meta[13] = optionsJson.chopped(1);
    check(hasStandaloneWithoutImage(), "the standalone marker survives incomplete serialized options");
    meta[13] = QString(8192, u' ') + optionsJson;
    check(hasStandaloneWithoutImage(), "oversized serialized options are skipped while retaining the standalone marker");
    check(danmakuEmotes(QString(8193, u'x'), meta, {}).isEmpty(), "standalone messages still respect the body length limit");
    meta[13] = optionsJson;
    meta[12] = 0;
    check(danmakuEmotes(entireBody, meta, {}).isEmpty(), "a URL at index 13 alone does not make a standalone emote");
    meta[12] = 1;
    meta[13] = QJsonObject{{"url", "https://untrusted.invalid/picture.png"}};
    check(hasStandaloneWithoutImage(), "rejected image URLs leave an identifiable standalone emote");
    meta[13] = QJsonObject{{"height", 96}};
    check(hasStandaloneWithoutImage(), "standalone image options may omit the URL");
    meta.removeLast();
    check(hasStandaloneWithoutImage(), "truncated options retain the explicit standalone marker");
    check(danmakuEmotes(rabbit, {}, QJsonObject{{"emots", QJsonArray{rabbitItem}}}).isEmpty(),
          "malformed inline metadata is ignored");

    auto conflicting = rabbitItem;
    conflicting.insert("url", starUrl);
    const QJsonObject mixed{{rabbit, rabbitItem}, {"id_duplicate", conflicting}, {star, starItem}};
    const auto mixedSpans = danmakuEmotes(rabbit + star, {}, QJsonObject{{"emots", mixed}});
    check(mixedSpans.size() == 2 && mixedSpans[0].start == 0 && mixedSpans[0].length == rabbit.size() &&
          mixedSpans[0].url.isEmpty() && !mixedSpans[0].standalone &&
          mixedSpans[1].start == rabbit.size() && mixedSpans[1].url == starUrl,
          "a conflicting image address preserves the known token and other valid images");
    auto wrongName = rabbitItem;
    wrongName.insert("descript", star);
    check(danmakuEmotes(rabbit + star, {}, QJsonObject{{"emots", QJsonObject{{rabbit, wrongName}, {star, starItem}}}}).isEmpty(),
          "contradictory bracketed names are not guessed");
    const QJsonObject duplicate{{rabbit, rabbitItem}, {"id_duplicate", rabbitItem}};
    check(danmakuEmotes(rabbit, {}, QJsonObject{{"emots", duplicate}}).size() == 1,
          "identical definitions for one token produce one span");
    const QJsonObject unavailable{{rabbit, QJsonObject{{"url", "file:///not-an-emote.png"}}},
                                  {"room_123_7", QJsonObject{{"descript", star}}},
                                  {QStringLiteral("[杂项]"), 123}};
    const auto noImages = danmakuEmotes(body + QStringLiteral("[杂项]"), {}, QJsonObject{{"emots", unavailable}});
    check(noImages.size() == spans.size(), "known inline tokens survive rejected or missing URLs, non-objects do not");
    for (int i = 0; i < spans.size(); ++i) {
        check(noImages[i].start == spans[i].start && noImages[i].length == spans[i].length &&
              !noImages[i].standalone && noImages[i].url.isEmpty(),
              "inline fallback preserves each exact token range without a downloadable URL");
    }
    check(danmakuEmotes(rabbit.repeated(32), {}, inlineExtra).size() == 32 &&
          danmakuEmotes(rabbit.repeated(33), {}, inlineExtra).isEmpty(),
          "too many inline matches leave the entire message intact");
    QJsonObject tooMany;
    for (int i = 0; i < 33; ++i) tooMany.insert(QString::number(i), rabbitItem);
    check(danmakuEmotes(rabbit, {}, QJsonObject{{"emots", tooMany}}).isEmpty() &&
          danmakuEmotes(QString(8192, u'x') + rabbit, {}, inlineExtra).isEmpty(),
          "oversized maps and messages fall back to text");
    return 0;
}
