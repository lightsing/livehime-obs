#include "display-asset-cache.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QNetworkReply>
#include <memory>
namespace livehime {
DisplayAssetCache::DisplayAssetCache(QObject *p) : QObject(p) {}
QPixmap DisplayAssetCache::get(const QString &url) {
    if (auto p = cache_.object(url))
        return *p;
    QUrl u(url);
    const auto host = u.host().toLower();
    const bool cdn = host.endsWith(".hdslb.com") || host.endsWith(".bilivideo.com");
    if (u.scheme() != "https" || !u.userInfo().isEmpty() || !u.fragment().isEmpty() || u.port() != -1 ||
        !cdn || url.size() > 2048)
        return {};
    if (!known_.contains(url) && pending_.size() < 128 && known_.size() < 2048) {
        known_.insert(url);
        pending_.enqueue(url);
        pump();
    }
    return {};
}
void DisplayAssetCache::pump() {
    while (active_ < 2 && !pending_.isEmpty()) {
        auto url = pending_.dequeue();
        QNetworkRequest request{QUrl(url)};
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(8000);
        auto reply = network_.get(request);
        reply->setReadBufferSize(1024 * 1024 + 1);
        ++active_;
        auto bytes = std::make_shared<QByteArray>();
        connect(reply, &QIODevice::readyRead, this, [reply, bytes] {
            bytes->append(reply->readAll());
            if (bytes->size() > 1024 * 1024)
                reply->abort();
        });
        connect(reply, &QNetworkReply::metaDataChanged, this, [reply] {
            if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > 1024 * 1024)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, url] {
            bytes->append(reply->readAll());
            const auto contentType = reply->header(QNetworkRequest::ContentTypeHeader)
                                         .toString()
                                         .section(';', 0, 0)
                                         .trimmed()
                                         .toLower();
            const bool imageType =
                contentType == "image/png" || contentType == "image/jpeg" || contentType == "image/webp";
            if (reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && imageType &&
                bytes->size() <= 1024 * 1024) {
                QBuffer buffer(bytes.get());
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                auto size = reader.size();
                if (size.isValid() && size.width() <= 1024 && size.height() <= 1024) {
                    auto image = reader.read();
                    if (!image.isNull()) {
                        auto pixmap = QPixmap::fromImage(
                            image.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                        cache_.insert(url, new QPixmap(pixmap),
                                      qMax(1, int(pixmap.width() * pixmap.height() * 4 / 1024)));
                        emit ready();
                    }
                }
            }
            reply->deleteLater();
            --active_;
            pump();
        });
    }
}
} // namespace livehime
