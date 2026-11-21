#include "danmaku-emotes.hpp"
#include "display-asset-cache.hpp"
#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <QStringView>

namespace livehime {
namespace {
constexpr int MaxBodyLength = 8192;
constexpr int MaxTokens = 32;
constexpr int MaxTokenLength = 128;
constexpr int MaxOptionsLength = 8192;

bool isToken(QStringView text) {
    if (text.size() < 3 || text.size() > MaxTokenLength || !text.startsWith(u'[') || !text.endsWith(u']'))
        return false;
    for (const QChar ch : text.sliced(1, text.size() - 2)) {
        if (ch == u'[' || ch == u']' || ch.category() == QChar::Other_Control ||
            ch == QChar::LineSeparator || ch == QChar::ParagraphSeparator)
            return false;
    }
    return true;
}
} // namespace

QList<DanmakuEmote> danmakuEmotes(const QString &body, const QJsonArray &meta, const QJsonObject &extra) {
    if (body.isEmpty() || body.size() > MaxBodyLength)
        return {};
    if (meta.size() > 12 && meta.at(12).toInt(-1) == 1) {
        const auto options = meta.size() > 13 ? meta.at(13) : QJsonValue{};
        QJsonObject item = options.toObject();
        if (options.isString()) {
            const auto json = options.toString();
            if (json.size() <= MaxOptionsLength)
                item = QJsonDocument::fromJson(json.toUtf8()).object();
        }
        const auto url = DisplayAssetCache::emoteUrl(item.value("url").toString());
        return {{0, static_cast<int>(body.size()), url, true}};
    }

    const auto definitions = extra.value("emots").toObject();
    if (definitions.isEmpty() || definitions.size() > MaxTokens)
        return {};
    QHash<QString, QString> urls;
    QSet<QString> conflictingNames;
    for (auto it = definitions.begin(); it != definitions.end(); ++it) {
        if (!it.value().isObject())
            continue;
        const auto item = it.value().toObject();
        const auto description = item.value("descript").toString();
        QString token = it.key();
        if (!isToken(token)) {
            if (!isToken(description))
                continue;
            token = description;
        } else if (isToken(description) && description != token) {
            conflictingNames.insert(token);
            conflictingNames.insert(description);
            continue;
        }
        const auto url = DisplayAssetCache::emoteUrl(item.value("url").toString());
        // The token is known even when its definitions disagree about the image.
        auto known = urls.find(token);
        if (known == urls.end())
            urls.insert(token, url);
        else if (known.value() != url)
            known.value().clear();
    }

    QList<DanmakuEmote> spans;
    int start = -1;
    for (int i = 0; i < body.size(); ++i) {
        if (body[i] == u'[') {
            start = i;
        } else if (body[i] == u']' && start >= 0) {
            const int length = i - start + 1;
            if (length <= MaxTokenLength) {
                const auto token = body.mid(start, length);
                const auto known = urls.constFind(token);
                if (known != urls.cend() && !conflictingNames.contains(token)) {
                    if (spans.size() == MaxTokens)
                        return {};
                    spans.append({start, length, known.value(), false});
                }
            }
            start = -1;
        }
    }
    return spans;
}
} // namespace livehime
