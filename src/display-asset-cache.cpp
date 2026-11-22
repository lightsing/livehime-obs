#include "display-asset-cache.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QNetworkReply>
#include <QTimer>
#include <memory>
#include <utility>

namespace livehime {
namespace {
bool cdnHost(const QString &host) {
    return host.endsWith(".hdslb.com") || host.endsWith(".bilivideo.com");
}

QPixmap decodePreview(QByteArray &bytes, const QByteArray &mime, bool emote) {
    QByteArray format;
    if (mime == "image/png") format = "png";
    else if (mime == "image/jpeg") format = "jpeg";
    else if (mime == "image/webp") format = "webp";
    else if (emote && mime == "image/gif") format = "gif";
    else return {};

    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    const int maxSide = emote ? 2048 : 1024;
    if (size.isEmpty() || size.width() > maxSide || size.height() > maxSide)
        return {};
    const QSize bounds = emote ? QSize(160, 96) : QSize(32, 32);
    const auto target = emote && size.boundedTo(bounds) == size ? size
        : size.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    auto image = reader.read(); // The first frame is the preview for animated GIF/WebP.
    if (!image.isNull() && image.size() != target)
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return QPixmap::fromImage(std::move(image));
}
} // namespace

DisplayAssetCache::DisplayAssetCache(QObject *p) : QObject(p) {}
QPixmap DisplayAssetCache::get(const QString &url) {
    if (auto p = avatars_.object(url))
        return *p;
    QUrl u(url);
    if (u.scheme() != "https" || !u.userInfo().isEmpty() || !u.fragment().isEmpty() || u.port() != -1 ||
        !cdnHost(u.host()) || url.size() > 2048)
        return {};
    return requestPreview(url, Usage::Avatar);
}

QString DisplayAssetCache::emoteUrl(const QString &url) {
    if (url.isEmpty() || url.size() > 2048)
        return {};
    for (const QChar ch : url)
        if (ch.unicode() <= 0x20 || ch.unicode() == 0x7f || ch == QLatin1Char('\\'))
            return {};
    QUrl parsed(url.startsWith("//") ? QStringLiteral("https:") + url : url, QUrl::StrictMode);
    if (!parsed.isValid() || !cdnHost(parsed.host()) || parsed.path().isEmpty() || parsed.hasFragment() ||
        parsed.authority(QUrl::FullyEncoded).contains('@'))
        return {};
    const bool http = parsed.scheme() == "http";
    const int defaultPort = http ? 80 : 443;
    if ((!http && parsed.scheme() != "https") || parsed.port(defaultPort) != defaultPort)
        return {};
    parsed.setScheme(QStringLiteral("https"));
    parsed.setPort(-1);
    const auto normalized = parsed.toString(QUrl::FullyEncoded);
    return normalized.size() <= 2048 ? normalized : QString{};
}

QPixmap DisplayAssetCache::emote(const QString &normalizedUrl) {
    if (const auto cached = cachedEmote(normalizedUrl); !cached.isNull())
        return cached;
    const auto url = emoteUrl(normalizedUrl);
    if (url.isEmpty())
        return {};
    if (url != normalizedUrl) {
        if (auto p = emotes_.object(url))
            return *p;
    }
    return requestPreview(url, Usage::Emote);
}

QPixmap DisplayAssetCache::cachedEmote(const QString &normalizedUrl) const {
    const auto *pixmap = emotes_.object(normalizedUrl);
    return pixmap ? *pixmap : QPixmap{};
}
void DisplayAssetCache::storePreview(const QString &url, Usage usage, const QPixmap &preview) {
    if (preview.isNull())
        return;
    auto &cache = usage == Usage::Emote ? emotes_ : avatars_;
    const auto cost = (preview.width() * preview.height() * 4 + 1023) / 1024;
    cache.insert(url, new QPixmap(preview), cost);
    if (usage == Usage::Emote)
        emit emoteReady(url);
    else
        emit ready(url);
}

QPixmap DisplayAssetCache::requestPreview(const QString &url, Usage usage) {
    const Key key{url, usage};
    if (auto *until = failures_.object(key)) {
        if (!until->hasExpired())
            return {};
        failures_.remove(key);
    }
    if (!requested_.contains(key) && pending_.size() < 128) {
        requested_.insert(key);
        pending_.enqueue(key);
        pump();
    }
    return {};
}
void DisplayAssetCache::pump() {
    while (active_ < 2 && !pending_.isEmpty()) {
        const auto key = pending_.dequeue();
        const bool emote = key.second == Usage::Emote;
        const qint64 limit = emote ? 4 * 1024 * 1024 : 1024 * 1024;
        QNetworkRequest request{QUrl(key.first)};
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(8000);
        auto reply = network_.get(request);
        reply->setReadBufferSize(limit + 1);
        QTimer::singleShot(15000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
        ++active_;
        auto bytes = std::make_shared<QByteArray>();
        auto readAvailable = [reply, bytes, limit] {
            const auto count = qMin<qint64>(reply->bytesAvailable(), limit + 1 - bytes->size());
            if (count > 0)
                bytes->append(reply->read(count));
        };
        connect(reply, &QIODevice::readyRead, this, [reply, bytes, limit, readAvailable] {
            readAvailable();
            if (bytes->size() > limit)
                reply->abort();
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [reply, limit] {
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, limit, key, emote, readAvailable] {
            readAvailable();
            QPixmap preview;
            if (reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && bytes->size() <= limit) {
                const auto mime = reply->rawHeader("Content-Type").split(';').value(0).trimmed().toLower();
                preview = decodePreview(*bytes, mime, emote);
            }
            if (preview.isNull())
                failures_.insert(key, new QDeadlineTimer(30000));
            requested_.remove(key);
            reply->deleteLater();
            --active_;
            pump();
            storePreview(key.first, key.second, preview);
        });
    }
}
} // namespace livehime
