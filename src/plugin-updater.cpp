#include "plugin-updater.hpp"
#include "beta-diagnostics.hpp"
#include <QMetaEnum>
#include "npm-signature.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimer>
#include <memory>

namespace livehime {
namespace {
constexpr qsizetype MaxMetadata = 2 * 1024 * 1024;
QUrl metadataUrl(bool mirror) {
    const auto host = mirror ? "registry.npmmirror.com" : "registry.npmjs.org";
    return QUrl::fromEncoded(QByteArray("https://") + host + "/@lightsing%2Flivehime-obs-win32-x64");
}
QUrl packageUrl(bool mirror, const QString &version) {
    return QUrl(QStringLiteral("https://%1/@lightsing/livehime-obs-win32-x64/-/livehime-obs-win32-x64-%2.tgz")
                    .arg(mirror ? QStringLiteral("registry.npmmirror.com") : QStringLiteral("registry.npmjs.org"),
                         version));
}
} // namespace

PluginUpdater::PluginUpdater(QString currentVersion, QString cacheDirectory, QObject *parent)
    : QObject(parent), currentVersion_(std::move(currentVersion)), cacheDirectory_(std::move(cacheDirectory)) {
    update::cleanInstalledCaches(currentVersion_, cacheDirectory_);
}
PluginUpdater::~PluginUpdater() { cancelPending(); }
void PluginUpdater::cancelPending() {
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_->deleteLater();
        reply_.clear();
    }
    if (worker_) {
        disconnect(worker_, nullptr, this, nullptr);
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
    }
}
void PluginUpdater::setChannel(update::Channel channel) {
    if (channel_ == channel) return;
    channel_ = channel;
    ++generation_;
    cancelPending();
    release_.reset();
    installer_ = {};
    urls_.clear();
    setState(State::Idle, {});
}
void PluginUpdater::setState(State state, const QString &status) {
#if LIVEHIME_BETA_BUILD
    diagnostics::record("update.state", {{"state", QMetaEnum::fromType<State>().valueToKey(int(state))},
                                         {"channel", channel_ == update::Channel::Beta ? "beta" : "stable"}});
#endif
    state_ = state;
    status_ = status;
    if (state == State::Error) emit updateFailed(status);
    emit changed();
}
QNetworkReply *PluginUpdater::get(const QNetworkRequest &request) { return network_.get(request); }

void PluginUpdater::request(const QUrl &url, qsizetype limit, bool officialMetadata, int redirects,
                            Received received) {
    if (!update::trustedUrl(url) || (officialMetadata && url.host() != "registry.npmjs.org") || redirects > 4) {
        received({}, QStringLiteral("更新地址无效"));
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    req.setRawHeader("Accept", limit == MaxMetadata ? "application/vnd.npm.install-v1+json" : "application/octet-stream");
    req.setTransferTimeout(limit == MaxMetadata ? 5000 : 15000);
    auto *reply = get(req);
    reply_ = reply;
    reply->setReadBufferSize(128 * 1024);
    auto data = std::make_shared<QByteArray>();
    auto error = std::make_shared<QString>();
    const auto consume = [reply, limit, data, error] {
        if (!error->isEmpty() || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid())
            return;
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit ||
            reply->bytesAvailable() > limit - data->size()) {
            *error = QStringLiteral("更新响应超过大小限制");
            reply->abort();
            return;
        }
        while (reply->bytesAvailable() > 0) {
            const auto chunk = reply->read(qMin<qint64>(64 * 1024, limit - data->size() + 1));
            if (chunk.size() > limit - data->size()) {
                *error = QStringLiteral("更新响应超过大小限制");
                reply->abort();
                return;
            }
            if (chunk.isEmpty())
                break;
            data->append(chunk);
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    connect(reply, &QNetworkReply::metaDataChanged, this, consume);
    QTimer::singleShot(limit == MaxMetadata ? 8000 : 90000, reply, [reply, error] {
        if (!reply->isFinished()) {
            *error = QStringLiteral("更新请求超时");
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, limit, officialMetadata, redirects, received = std::move(received),
             consume, data, error]() mutable {
        consume();
        reply_ = nullptr;
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (error->isEmpty() && reply->error() == QNetworkReply::NoError &&
            code >= 300 && code < 400 && !location.isEmpty()) {
            request(reply->url().resolved(location), limit, officialMetadata, redirects + 1, std::move(received));
            return;
        }
        if (error->isEmpty() && (code != 200 || reply->error() != QNetworkReply::NoError))
            *error = QStringLiteral("无法连接更新源");
        received(std::move(*data), *error);
    });
}

void PluginUpdater::check() {
    if (state_ == State::Checking || state_ == State::Downloading)
        return;
    release_.reset();
    installer_ = {};
    urls_.clear();
    if (!update::validVersion(currentVersion_)) {
        setState(State::Error, QStringLiteral("当前版本号不支持更新检查"));
        return;
    }
    setState(State::Checking, QStringLiteral("正在检查更新…"));
    request(metadataUrl(true), MaxMetadata, false, 0, [this](QByteArray body, const QString &error) {
        const auto mirror = error.isEmpty() ? update::readMetadata(body, channel_) : std::nullopt;
        bool signedMirror = false;
        if (mirror) {
            const auto versions = QJsonDocument::fromJson(body).object().value("versions").toObject();
            const auto dist = versions.value(mirror->version).toObject().value("dist").toObject();
            signedMirror = trustedMirrorSignature(QLatin1String(update::PackageName), mirror->version, dist);
        }
        if (channel_ == update::Channel::Stable && signedMirror && update::newer(mirror->version, currentVersion_)) {
            selectRelease(mirror, true, {});
            return;
        }
        request(metadataUrl(false), MaxMetadata, true, 0,
                [this, mirror, signedMirror](QByteArray originBody, const QString &originError) {
            const auto official = originError.isEmpty() ? update::readMetadata(originBody, channel_) : std::nullopt;
            selectRelease(mirror, signedMirror, official);
        });
    });
}

void PluginUpdater::selectRelease(const std::optional<update::Release> &mirror, bool signedMirror,
                                 const std::optional<update::Release> &official) {
    release_ = official ? official : (signedMirror ? mirror : std::nullopt);
    if (!release_) {
        setState(State::Error, QStringLiteral("无法取得可信的更新信息，请重试"));
        return;
    }
    const bool returnToStable = channel_ == update::Channel::Stable && !update::stableVersion(currentVersion_);
    if (returnToStable && !official && update::newer(currentVersion_, release_->version)) {
        setState(State::Error, QStringLiteral("切回较低版本需向官方源确认正式版，请稍后重试"));
        return;
    }
    if (!returnToStable && !update::newer(release_->version, currentVersion_)) {
        setState(State::UpToDate, official ? QStringLiteral("已是最新版本") :
                                           QStringLiteral("镜像暂无更新，官方源暂不可用"));
        return;
    }
    auto addUrl = [this](QUrl url) {
        if (update::trustedUrl(url) && !urls_.contains(url))
            urls_.append(url);
    };
    if (mirror && mirror->version == release_->version && mirror->sha512 == release_->sha512) {
        if (mirror->tarball.host() == "registry.npmjs.org")
            addUrl(packageUrl(true, release_->version));
        addUrl(mirror->tarball);
    }
    if (official)
        addUrl(official->tarball);
    addUrl(packageUrl(false, release_->version));
    const auto cached = QFileInfo(update::archivePath(*release_, cacheDirectory_));
    if (QDir(cacheDirectory_).isAbsolute() && !QFileInfo(cacheDirectory_).isSymLink() && cached.isFile() &&
        !cached.isSymLink() && cached.size() > 0 && cached.size() <= update::MaxArchiveBytes) {
        setState(State::Downloading, QStringLiteral("正在验证已缓存的更新…"));
        verify({}, -1);
        return;
    }
    const auto message = returnToStable ? QStringLiteral("可切回正式版 %1") : QStringLiteral("发现新版本 %1");
    setState(State::Available, message.arg(release_->version));
}
void PluginUpdater::download() {
    if (state_ != State::Available || !release_)
        return;
    setState(State::Downloading, QStringLiteral("正在下载 %1…").arg(release_->version));
    tryDownload(0);
}
void PluginUpdater::tryDownload(int index) {
    request(urls_.at(index), update::MaxArchiveBytes, false, 0, [this, index](QByteArray body, const QString &error) {
        if (!error.isEmpty())
            downloadFailed(index, error);
        else {
            setState(State::Downloading, QStringLiteral("正在验证下载的更新…"));
            verify(std::move(body), index);
        }
    });
}
void PluginUpdater::downloadFailed(int index, const QString &error) {
    if (index >= 0 && index + 1 < urls_.size()) {
        setState(State::Downloading, QStringLiteral("正在从备用更新源下载…"));
        tryDownload(index + 1);
        return;
    }
    const auto detail = error + QStringLiteral("，可重新下载");
    emit updateFailed(detail);
    setState(State::Available, detail);
}
void PluginUpdater::verify(QByteArray archive, int index) {
    auto result = std::make_shared<update::CachedInstaller>();
    worker_ = QThread::create([archive = std::move(archive), release = *release_, directory = cacheDirectory_,
                               cached = index < 0, result]() mutable {
        if (cached) {
            QFile file(update::archivePath(release, directory));
            if (file.open(QIODevice::ReadOnly))
                archive = file.read(update::MaxArchiveBytes + 1);
        }
        *result = update::cacheInstaller(archive, release, directory, !cached);
    });
    connect(worker_, &QThread::finished, this, [this, index, result, generation = generation_] {
        if (generation != generation_) return;
        worker_->wait();
        delete worker_;
        worker_ = nullptr;
        if (!result->error.isEmpty()) {
            downloadFailed(index, result->error);
            return;
        }
        installer_ = *result;
        setState(State::Ready, QStringLiteral("版本 %1 已下载并验证").arg(release_->version));
    }, Qt::QueuedConnection);
    worker_->start();
}
} // namespace livehime
