#include "bili-client.hpp"
#include "beta-diagnostics.hpp"
#include <QElapsedTimer>
#include <QSet>

#include <QBuffer>
#include <QDateTime>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPointer>
#include <QSslSocket>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace livehime {
namespace {
constexpr qint64 MaxResponseBytes = 4 * 1024 * 1024;
ApiError inputError(const QString &message) { return {ApiError::Kind::InvalidInput, message}; }
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
bool containsControl(const QByteArray &value)
{
    return std::any_of(value.begin(), value.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f;
    });
}
QUrl uploadLocation(const QString &text)
{
    return protocol::coverUrl(text);
}
class NoCookiesJar final : public QNetworkCookieJar {
public:
    explicit NoCookiesJar(QObject *parent) : QNetworkCookieJar(parent) {}
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
};
} // namespace

class CookieJar final : public QNetworkCookieJar {
public:
    explicit CookieJar(QObject *parent) : QNetworkCookieJar(parent) {}
    void replace(const QList<QNetworkCookie> &cookies)
    {
        stored_.clear();
        for (const auto &cookie : cookies) if (protocol::cookieAllowed(cookie)) insert(cookie);
    }
    QList<QNetworkCookie> values() const
    {
        QList<QNetworkCookie> result;
        for (const auto &cookie : stored_) if (protocol::cookieAllowed(cookie)) result.append(cookie);
        return result;
    }
    QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const override
    {
        QList<QNetworkCookie> result;
        for (const auto &cookie : stored_)
            if (protocol::cookieMatches(cookie, url)) result.append(cookie);
        std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return a.path().size() > b.path().size();
        });
        return result;
    }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &cookies, const QUrl &url) override
    {
        if (!protocol::isApiUrl(url)) return false;
        bool changed = false;
        for (auto cookie : cookies) {
            const bool hostOnly = cookie.domain().isEmpty();
            if (hostOnly) cookie.setDomain(url.host());
            else if (!cookie.domain().startsWith('.')) cookie.setDomain('.' + cookie.domain());
            if (cookie.path().isEmpty()) {
                QString path = url.path();
                path = path.left(path.lastIndexOf('/') + 1);
                cookie.setPath(path.isEmpty() ? QStringLiteral("/") : path);
            }
            // Expired cookies must be allowed through so the base jar can delete
            // them. Validate an otherwise identical temporary session cookie.
            auto check = cookie;
            check.setExpirationDate({});
            QString domain = cookie.domain().toLower();
            if (domain.startsWith('.')) domain.remove(0, 1);
            if (!protocol::cookieAllowed(check) ||
                !(url.host() == domain || (!hostOnly && url.host().endsWith('.' + domain)))) continue;
            insert(cookie);
            changed = true;
        }
        return changed;
    }
private:
    void insert(const QNetworkCookie &cookie)
    {
        // Preserve the leading-dot distinction instead of letting a generic jar
        // normalize an imported host-only cookie into a broader domain cookie.
        stored_.removeIf([&](const QNetworkCookie &existing) {
            return existing.name() == cookie.name() &&
                   existing.domain().compare(cookie.domain(), Qt::CaseInsensitive) == 0 &&
                   existing.path() == cookie.path();
        });
        if (protocol::cookieAllowed(cookie)) stored_.append(cookie);
    }
    QList<QNetworkCookie> stored_;
};

BiliClient::BiliClient(QObject *parent) : QObject(parent), manager_(new QNetworkAccessManager(this)),
                                       publicManager_(new QNetworkAccessManager(this)),
                                       jar_(new CookieJar(manager_))
{
    manager_->setCookieJar(jar_);
    manager_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    publicManager_->setCookieJar(new NoCookiesJar(publicManager_));
    publicManager_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}
BiliClient::~BiliClient() { cancelRequests(); }
void BiliClient::cancelRequests()
{
    ++generation_;
    const auto pending = pending_;
    pending_.clear();
    for (auto *reply : pending) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}
void BiliClient::setCookies(const QList<QNetworkCookie> &cookies)
{
    cancelRequests();
    validated_ = false;
    jar_->replace(cookies);
    emit cookiesChanged();
}
QList<QNetworkCookie> BiliClient::cookies() const { return jar_->values(); }
void BiliClient::logout()
{
    cancelRequests();
    validated_ = false;
    jar_->replace({});
    manager_->clearAccessCache();
    publicManager_->clearAccessCache();
    emit cookiesChanged();
}
void BiliClient::setDeviceId(const QString &deviceId)
{
    const auto value = deviceId.toUtf8();
    deviceId_ = value.size() <= 256 && !containsControl(value) ? value : QByteArray{};
}
QByteArray BiliClient::csrf() const
{
    const auto relevant = jar_->cookiesForUrl(QUrl(QStringLiteral("https://api.live.bilibili.com/")));
    for (const auto &cookie : relevant) if (cookie.name() == "bili_jct") return cookie.value();
    return {};
}
std::optional<ApiError> BiliClient::sessionError() const
{
    if (!validated_) return ApiError{ApiError::Kind::Authentication, QStringLiteral("请先扫码登录并完成身份验证。")};
    if (csrf().isEmpty()) return ApiError{ApiError::Kind::Authentication, QStringLiteral("登录会话缺少 CSRF Cookie，请重新登录。")};
    if (deviceId_.isEmpty()) return inputError(QStringLiteral("本地设备标识未准备完成。"));
    return {};
}

QNetworkReply *BiliClient::send(const protocol::Request &draft, Callback<QJsonObject> callback,
                      QHttpMultiPart *multipart, bool web, bool requireMessage, bool classicInfo)
{
    if (!protocol::isApiUrl(draft.url) || (draft.method != "GET" && draft.method != "POST")) {
        delete multipart;
        callback(inputError(QStringLiteral("请求目标不在允许的 HTTPS API 范围内。")));
        return nullptr;
    }
    if (!QSslSocket::supportsSsl()) {
        delete multipart;
        callback(protocol::transportFailure(QNetworkReply::NoError, 0, false, false, false));
        return nullptr; // No request was sent, so a mutation cannot have committed.
    }
    QNetworkRequest request(draft.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", protocol::userAgent());
    if (!deviceId_.isEmpty()) request.setRawHeader("buvid", deviceId_);
    if (!draft.clientTraceId.isEmpty()) request.setRawHeader("client_traceid", draft.clientTraceId);
    if (web) {
        request.setRawHeader("Origin", "https://live.bilibili.com");
        request.setRawHeader("Referer", "https://live.bilibili.com/");
    }
    if (draft.method == "POST" && !multipart)
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(multipart ? 60000 : 30000);
    auto *reply = draft.method == "GET" ? manager_->get(request) :
                  multipart ? manager_->post(request, multipart) : manager_->post(request, draft.body);
#if LIVEHIME_BETA_BUILD
    static const QSet<QString> operations{"nav", "startLive", "stopLive", "GetInfo", "PreLive", "getDanmuInfo",
        "sendmsg", "UpdatePreLiveInfo", "AddBlack", "AddSilentUser", "appoint", "queryContributionRank", "topListNew",
        "pkPreCheck", "join", "cancel", "getInfoById", "exit", "autoMatchSwitchStatus", "updateAutoMatchSwitch"};
    auto operation = draft.url.fileName();
    if (!operations.contains(operation)) operation = QStringLiteral("other");
    QElapsedTimer timer;
    timer.start();
    callback = [done = std::move(callback), operation, timer, reply](Result<QJsonObject> result) {
        QJsonObject fields{{"operation", operation}, {"elapsed_ms", timer.elapsed()},
                           {"http", reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()},
                           {"network_error", int(reply->error())}};
        if (const auto *error = std::get_if<ApiError>(&result)) {
            fields.insert("error_kind", int(error->kind));
            fields.insert("api_code", error->code);
            fields.insert("outcome_unknown", error->outcomeUnknown);
        }
        diagnostics::record("api.complete", fields);
        done(std::move(result));
    };
#endif
    if (multipart) multipart->setParent(reply);
    pending_.insert(reply);
    const auto generation = generation_;
    const bool mutation = draft.method == "POST" || draft.changesState;
    struct Response { QByteArray bytes; bool timedOut = false; bool oversized = false; };
    const auto state = std::make_shared<Response>();
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply, state] { state->timedOut = true; reply->abort(); });
    deadline->start(multipart ? 60000 : 30000);
    connect(reply, &QIODevice::readyRead, this, [reply, state] {
        state->bytes += reply->readAll();
        if (state->bytes.size() > MaxResponseBytes) { state->oversized = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, deadline, state, generation, mutation, requireMessage, classicInfo, callback = std::move(callback)]() mutable {
        pending_.remove(reply);
        deadline->stop();
        reply->deleteLater();
        if (generation != generation_) return;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        auto fail = [&](ApiError::Kind kind, const QString &message, bool unknown) {
            ApiError error{kind, message};
            error.httpStatus = status;
            error.networkCode = static_cast<int>(reply->error());
            error.outcomeUnknown = unknown;
            callback(error);
        };
        // Never follow a redirect carrying account cookies or repeat a POST.
        if ((status >= 300 && status < 400) || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
            fail(ApiError::Kind::Redirect, QStringLiteral("API 返回了重定向；请求已停止，请重新验证会话。"), mutation);
            return;
        }
        if (state->oversized) {
            fail(ApiError::Kind::InvalidResponse, QStringLiteral("服务器响应超过大小限制。"), mutation);
            return;
        }
        if (state->timedOut || reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            callback(protocol::transportFailure(static_cast<int>(reply->error()), status, state->timedOut,
                                                QSslSocket::supportsSsl(), mutation));
            return;
        }
        state->bytes += reply->readAll();
        if (state->bytes.size() > MaxResponseBytes) {
            fail(ApiError::Kind::InvalidResponse, QStringLiteral("服务器响应超过大小限制。"), mutation);
            return;
        }
        auto result = protocol::envelope(state->bytes, requireMessage, classicInfo);
        if (auto *error = std::get_if<ApiError>(&result)) {
            error->httpStatus = status;
            if (error->kind == ApiError::Kind::InvalidResponse) error->outcomeUnknown = mutation;
            if (error->kind == ApiError::Kind::Authentication) { validated_ = false; emit authenticationExpired(); }
        }
        // Store only this plugin's jar. There are no diagnostic payload/URL logs.
        const QPointer<BiliClient> self(this);
        emit cookiesChanged();
        if (!self || generation != generation_) return;
        callback(std::move(result));
    });
    return reply;
}

QNetworkReply *BiliClient::sendDanmaku(qint64 roomId, const QString &text, Callback<Done> callback)
{
    if (const auto error = sessionError()) { callback(*error); return nullptr; }
    if (roomId <= 0 || !protocol::validDanmakuText(text)) {
        callback(inputError(QStringLiteral("请选择有效房间并输入单行弹幕。")));
        return nullptr;
    }
    return send(protocol::sendDanmaku(roomId, text, csrf(), QDateTime::currentDateTimeUtc()),
                [callback = std::move(callback)](Result<QJsonObject> result) {
                    if (const auto *error = std::get_if<ApiError>(&result)) callback(*error);
                    else callback(Done{});
                }, nullptr, false, true);
}
QNetworkReply *BiliClient::moderateUser(UserModeration action, qint64 anchorId, qint64 roomId, qint64 targetUid,
                                       const QString &message, int durationHours, Callback<Done> callback)
{
    if (const auto error = sessionError()) { callback(*error); return nullptr; }
    const auto request = protocol::moderateUser(action, anchorId, roomId, targetUid, message, csrf(), now(), durationHours);
    if (request.url.isEmpty()) {
        callback(inputError(QStringLiteral("请选择当前直播间内的其他用户。")));
        return nullptr;
    }
    return send(request, [callback = std::move(callback)](Result<QJsonObject> result) {
        if (const auto *error = std::get_if<ApiError>(&result)) callback(*error);
        else callback(Done{});
    });
}
void BiliClient::classicPk(ClassicPkOp op, qint64 uid, qint64 room, qint64 pkId, bool continuous,
                           Callback<QJsonObject> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    const auto request = protocol::classicPk(op, uid, room, pkId, continuous, csrf(), now());
    send(request, std::move(callback), nullptr, false, false, op == ClassicPkOp::Info);
}

QNetworkReply *BiliClient::fetchRealtime(const protocol::Request &request, Callback<QJsonObject> callback)
{
    if (const auto error = sessionError()) { callback(*error); return nullptr; }
    const auto path = request.url.path();
    if (request.method != "GET" || !(path == "/xlive/app-room/v1/index/getDanmuInfo" ||
        path == "/xlive/general-interface/v1/rank/queryContributionRank" ||
        path == "/xlive/app-room/v2/guardTab/topListNew")) {
        callback(inputError(QStringLiteral("不支持的实时只读请求。"))); return nullptr;
    }
    return send(request, std::move(callback));
}
void BiliClient::cancelRealtime(QNetworkReply *reply)
{
    if (reply && pending_.contains(reply)) reply->abort();
}
void BiliClient::validateSession(Callback<UserInfo> callback)
{
    validated_ = false;
    if (jar_->values().isEmpty()) { callback(ApiError{ApiError::Kind::Authentication, QStringLiteral("没有可验证的扫码会话。")}); return; }
    send(protocol::nav(now()),
        [this, callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) { callback(*error); return; }
        auto user = protocol::parseUser(std::get<QJsonObject>(result));
        validated_ = std::holds_alternative<UserInfo>(user);
        callback(std::move(user));
    });
}
void BiliClient::fetchRoom(qint64 uid, Callback<RoomInfo> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    if (uid <= 0) { callback(inputError(QStringLiteral("账号 UID 无效。"))); return; }
    send(protocol::room(uid, now()), [this, callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) { callback(*error); return; }
        auto room = protocol::parseRoom(std::get<QJsonObject>(result));
        if (auto *error = std::get_if<ApiError>(&room)) { callback(*error); return; }
        // GetInfo does not provide the current cover/audit/intercept contract.
        send(protocol::preLive(now()), [room = std::get<RoomInfo>(room), callback = std::move(callback)](Result<QJsonObject> pre) mutable {
            if (auto *error = std::get_if<ApiError>(&pre)) { callback(protocol::preLiveFailure(room, *error)); return; }
            auto merged = protocol::mergePreLive(room, std::get<QJsonObject>(pre));
            if (auto *error = std::get_if<ApiError>(&merged)) { callback(protocol::preLiveFailure(room, *error)); return; }
            callback(std::move(merged));
        });
    });
}
void BiliClient::anonymousGet(const QUrl &url, Callback<QByteArray> callback, qint64 maxBytes,
                              const QByteArray &accept)
{
    const QUrl areas(QStringLiteral("https://api.live.bilibili.com/room/v1/Area/getList"));
    if (url != areas && !protocol::isCoverUrl(url)) {
        callback(inputError(QStringLiteral("匿名资源请求目标不在允许范围内。"))); return;
    }
    if (!QSslSocket::supportsSsl()) {
        callback(protocol::transportFailure(QNetworkReply::NoError, 0, false, false, false)); return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Accept", accept);
    request.setRawHeader("User-Agent", protocol::userAgent());
    request.setTransferTimeout(15000);
    auto *reply = publicManager_->get(request);
    reply->setReadBufferSize(maxBytes + 1);
    pending_.insert(reply);
    const auto generation = generation_;
    struct Download { QByteArray bytes; bool timedOut = false; bool oversized = false; };
    const auto state = std::make_shared<Download>();
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply, state] { state->timedOut = true; reply->abort(); });
    deadline->start(15000);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, state, maxBytes] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maxBytes) {
            state->oversized = true; reply->abort();
        }
    });
    connect(reply, &QIODevice::readyRead, this, [reply, state, maxBytes] {
        state->bytes += reply->readAll();
        if (state->bytes.size() > maxBytes) { state->oversized = true; reply->abort(); }
    });
    const bool image = accept.startsWith("image/");
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, state, deadline, generation, maxBytes, image, callback = std::move(callback)]() mutable {
        pending_.remove(reply);
        deadline->stop();
        reply->deleteLater();
        if (generation != generation_) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto error = static_cast<int>(reply->error());
        auto fail = [&](ApiError::Kind kind, const QString &message) {
            ApiError problem{kind, message}; problem.httpStatus = status; problem.networkCode = error; callback(problem);
        };
        if ((status >= 300 && status < 400) || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
            fail(ApiError::Kind::Redirect, QStringLiteral("匿名资源返回重定向，已停止下载。")); return;
        }
        if (state->oversized) {
            fail(ApiError::Kind::InvalidResponse, QStringLiteral("资源超过允许的下载大小。")); return;
        }
        if (state->timedOut || error != QNetworkReply::NoError || status < 200 || status >= 300) {
            callback(protocol::transportFailure(error, status, state->timedOut, QSslSocket::supportsSsl(), false)); return;
        }
        state->bytes += reply->readAll();
        if (state->bytes.isEmpty() || state->bytes.size() > maxBytes) {
            fail(ApiError::Kind::InvalidResponse, QStringLiteral("资源内容为空或超过允许大小。")); return;
        }
        if (image) {
            if (!protocol::isCoverContentType(reply->header(QNetworkRequest::ContentTypeHeader).toString())) {
                fail(ApiError::Kind::InvalidResponse, QStringLiteral("封面地址未返回支持的图片格式。")); return;
            }
        }
        callback(std::move(state->bytes));
    });
}
void BiliClient::fetchAreas(Callback<QList<AreaInfo>> callback)
{
    anonymousGet(QUrl(QStringLiteral("https://api.live.bilibili.com/room/v1/Area/getList")),
        [callback = std::move(callback)](Result<QByteArray> result) mutable {
        if (const auto *error = std::get_if<ApiError>(&result)) { callback(*error); return; }
        callback(protocol::parseAreas(std::get<QByteArray>(result)));
    }, MaxResponseBytes, "application/json");
}
void BiliClient::fetchCover(const QUrl &url, Callback<QByteArray> callback)
{
    const auto normalized = protocol::coverUrl(url.toString(QUrl::FullyEncoded));
    if (normalized.isEmpty()) { callback(inputError(QStringLiteral("封面地址不是可信的哔哩哔哩图片 CDN。"))); return; }
    anonymousGet(normalized, std::move(callback), 8 * 1024 * 1024, "image/avif,image/webp,image/png,image/jpeg,image/gif");
}
void BiliClient::updateTitle(const QString &title, Callback<Done> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    if (title.trimmed().isEmpty() || title.toUtf8().size() > 300 || containsControl(title.toUtf8())) {
        callback(inputError(QStringLiteral("请输入有效标题（不超过 300 UTF-8 字节）。"))); return;
    }
    send(protocol::title(title, csrf(), now()), [callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) callback(*error); else callback(Done{});
    });
}
void BiliClient::uploadCover(const QString &filePath, Callback<QUrl> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    const QFileInfo file(filePath);
    if (!file.isFile() || file.size() <= 0 || file.size() > 30 * 1024 * 1024) {
        callback(inputError(QStringLiteral("请选择小于 30 MB 的本地图片。"))); return;
    }
    QImageReader reader(filePath);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > 80000000) {
        callback(inputError(QStringLiteral("图片尺寸无效或超过 8000 万像素。"))); return;
    }
    // Decode at a bounded size where the format supports scaled decoding.
    if (size.width() > 4096 || size.height() > 4096)
        reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
    const QImage source = reader.read();
    if (source.isNull()) { callback(inputError(QStringLiteral("无法读取图片格式。"))); return; }
    const auto scaled = source.scaled(720, 540, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QImage cropped(720, 540, QImage::Format_RGB32);
    cropped.fill(Qt::white);
    {
        QPainter painter(&cropped);
        painter.drawImage(QPoint((720 - scaled.width()) / 2, (540 - scaled.height()) / 2), scaled);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly) || !cropped.save(&buffer, "JPEG", 95)) {
        callback(inputError(QStringLiteral("无法生成 JPEG 封面。"))); return;
    }
    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto &[name, value] : protocol::Params{{"bucket", "live"}, {"dir", "new_room_cover"}}) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(QString::fromLatin1(name)));
        part.setBody(value);
        multipart->append(part);
    }
    QHttpPart image;
    image.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"file\"; filename=\"cover.jpg\""));
    image.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/jpeg"));
    image.setBody(jpeg);
    multipart->append(image);
    const QUrl url = QUrl::fromEncoded("https://api.bilibili.com/x/upload/web/image?csrf=" + protocol::escape(csrf()));
    send({"POST", url, {}, {}}, [this, callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) { callback(*error); return; }
        const auto location = uploadLocation(std::get<QJsonObject>(result).value(QStringLiteral("location")).toString());
        if (location.isEmpty()) {
            callback(ApiError{ApiError::Kind::InvalidResponse, QStringLiteral("图片上传未返回有效封面地址，尚未提交封面。")}); return;
        }
        const auto visitId = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
        send(protocol::cover(location, csrf(), visitId),
            [location, callback = std::move(callback)](Result<QJsonObject> saved) mutable {
            if (auto *error = std::get_if<ApiError>(&saved)) callback(*error); else callback(location);
        }, nullptr, true);
    }, multipart, true);
}
void BiliClient::startLive(qint64 roomId, int areaId, Callback<LiveSession> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    if (roomId <= 0 || areaId <= 0) { callback(inputError(QStringLiteral("直播间或分区 ID 无效。"))); return; }
    send(protocol::start(roomId, areaId, csrf(), QDateTime::currentDateTimeUtc()),
        [roomId, callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) { callback(*error); return; }
        callback(protocol::parseStart(roomId, std::get<QJsonObject>(result)));
    });
}
void BiliClient::stopLive(qint64 roomId, Callback<Done> callback)
{
    if (const auto error = sessionError()) { callback(*error); return; }
    if (roomId <= 0) { callback(inputError(QStringLiteral("直播间 ID 无效。"))); return; }
    send(protocol::stop(roomId, csrf(), now()), [callback = std::move(callback)](Result<QJsonObject> result) mutable {
        if (auto *error = std::get_if<ApiError>(&result)) callback(*error); else callback(Done{});
    });
}
} // namespace livehime
