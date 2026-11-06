#include "protocol.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace livehime::protocol {
namespace {
constexpr auto LiveApi = "https://api.live.bilibili.com";
constexpr auto TitlePath = "/xlive/app-blink/v1/preLive/UpdatePreLiveInfo";
ApiError invalid(const QString &message) { return {ApiError::Kind::InvalidResponse, message}; }
Params signedParams(Params p)
{
    const auto digest = signature(p);
    p.emplace("sign", digest);
    return p;
}
void addCsrf(Params &p, const QByteArray &csrf)
{
    p["csrf"] = csrf;
    p["csrf_token"] = csrf;
}
QUrl withQuery(const QString &path, const Params &p)
{
    // The serializer already performed escaping. Do not encode it a second time.
    return QUrl::fromEncoded(path.toUtf8() + '?' + serialize(p), QUrl::StrictMode);
}
qint64 integer(const QJsonValue &value, qint64 fallback = 0)
{
    if (value.isString()) {
        bool ok = false;
        const auto result = value.toString().toLongLong(&ok);
        return ok ? result : fallback;
    }
    if (!value.isDouble()) return fallback;
    const double d = value.toDouble();
    // JSON double integers above 2^53 cannot identify a room/account safely.
    return std::isfinite(d) && std::floor(d) == d && std::abs(d) <= 9007199254740991.0
               ? static_cast<qint64>(d) : fallback;
}
bool truth(const QJsonValue &value) { return value.isBool() ? value.toBool() : integer(value) != 0; }
QString displayText(const QJsonValue &value, const QString &fallback)
{
    QString text = value.toString().left(300);
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
    // Arbitrary server messages can contain credential-bearing URLs. The validated
    // user-action URL travels separately and is never formatted into an error log.
    text.replace(QRegularExpression(QStringLiteral("https?://\\S+")), QStringLiteral("[链接]"));
    return text.isEmpty() ? fallback : text;
}
bool unsafeBytes(const QByteArray &value)
{
    return std::any_of(value.begin(), value.end(), [](char c) {
        const auto b = static_cast<unsigned char>(c);
        return b < 0x20 || b == 0x7f;
    });
}
Result<LiveSession> rtmp(qint64 roomId, const QJsonObject &item)
{
    const QString address = item.value(QStringLiteral("addr")).toString();
    const QString key = item.value(QStringLiteral("code")).toString();
    const QUrl url(address, QUrl::StrictMode);
    if (address.isEmpty() || key.isEmpty() || unsafeBytes(address.toUtf8()) ||
        unsafeBytes(key.toUtf8()) || !url.isValid() || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() ||
        (url.scheme() != QStringLiteral("rtmp") && url.scheme() != QStringLiteral("rtmps"))) {
        return invalid(QStringLiteral("开播响应没有完整、有效的普通 RTMP 地址和流密钥。"));
    }
    return LiveSession{roomId, address, key};
}
} // namespace

bool ByteLess::operator()(const QByteArray &a, const QByteArray &b) const
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
    });
}
QByteArray escape(const QByteArray &bytes)
{
    constexpr char Hex[] = "0123456789ABCDEF";
    constexpr auto Safe = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!'()*-._~";
    const QByteArray safe(Safe);
    QByteArray result;
    result.reserve(bytes.size() * 3);
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (safe.contains(c)) result += c;
        else { result += '%'; result += Hex[b >> 4]; result += Hex[b & 15]; }
    }
    return result;
}
QByteArray serialize(const Params &params)
{
    QByteArray result;
    bool first = true;
    for (const auto &[key, value] : params) {
        if (!first) result += '&';
        first = false;
        if (!key.isEmpty()) result += escape(key) + '=';
        result += escape(value);
    }
    return result;
}
QByteArray signature(const Params &params)
{
    if (params.contains("sign")) throw std::invalid_argument("signature requires unsigned parameters");
    return QCryptographicHash::hash(serialize(params) + "af125a0d5279fd576c1b4418a3e8276d",
                                    QCryptographicHash::Md5).toHex();
}
Params common(qint64 timestampSeconds)
{
    return {{"access_key", ""}, {"appkey", "aae92bc66f3edfab"}, {"build", "10819"},
            {"platform", "pc_link"}, {"ts", QByteArray::number(timestampSeconds)},
            {"version", "7.64.0.10819"}};
}
Request nav(qint64 timestampSeconds)
{
    // AccountRequestCommonParams (0x2021b00) differs from live-room common().
    const Params p{{"appkey", "aae92bc66f3edfab"}, {"platform", "pc_link"},
                   {"ts", QByteArray::number(timestampSeconds)}};
    return {"GET", withQuery(QStringLiteral("https://api.bilibili.com/x/web-interface/nav"), signedParams(p)),
            {}, serialize(p)};
}
Request title(const QString &value, const QByteArray &csrf, qint64 now)
{
    const auto unsignedParams = common(now);
    auto body = signedParams(unsignedParams);
    auto url = withQuery(QString::fromLatin1(LiveApi) + QString::fromLatin1(TitlePath), body);
    body["mobi_app"] = "pc_link";
    if (!value.isEmpty()) body["title"] = value.toUtf8();
    addCsrf(body, csrf);
    return {"POST", url, serialize(body), serialize(unsignedParams)};
}
Request start(qint64 roomId, int areaId, const QByteArray &csrf, qint64 now)
{
    auto p = common(now);
    p["room_id"] = QByteArray::number(roomId);
    p["area_v2"] = QByteArray::number(areaId);
    p["type"] = "2";
    addCsrf(p, csrf);
    return {"POST", QUrl(QString::fromLatin1(LiveApi) + "/room/v1/Room/startLive"),
            serialize(signedParams(p)), serialize(p)};
}
Request stop(qint64 roomId, const QByteArray &csrf, qint64 now)
{
    auto p = common(now);
    p["room_id"] = QByteArray::number(roomId);
    auto body = signedParams(p);
    addCsrf(body, csrf);
    return {"POST", QUrl(QString::fromLatin1(LiveApi) + "/room/v1/Room/stopLive"),
            serialize(body), serialize(p)};
}
Request room(qint64 uid, qint64 now)
{
    auto p = common(now);
    p["uId"] = QByteArray::number(uid);
    return {"GET", withQuery(QString::fromLatin1(LiveApi) + "/xlive/app-blink/v1/room/GetInfo",
                              signedParams(p)), {}, serialize(p)};
}
Request preLive(qint64 now)
{
    const auto p = common(now);
    auto query = signedParams(p);
    query["mobi_app"] = "pc_link";
    for (auto field : {"cover", "area", "title", "schedule", "coverVertical"}) query[field] = "true";
    query["liveDirectionType"] = "0";
    return {"GET", withQuery(QString::fromLatin1(LiveApi) + "/xlive/app-blink/v1/preLive/PreLive", query),
            {}, serialize(p)};
}
Request cover(const QUrl &location, const QByteArray &csrf, const QByteArray &visitId)
{
    // Standalone web context: no native sign. This is the main 4:3 cover contract.
    Params p{{"platform", "web"}, {"mobi_app", "web"}, {"build", "1"},
             {"cover", location.toEncoded()}, {"coverVertical", ""},
             {"liveDirectionType", "1"}, {"aiCoverTaskId", ""}, {"visit_id", visitId}};
    addCsrf(p, csrf);
    return {"POST", QUrl(QString::fromLatin1(LiveApi) + QString::fromLatin1(TitlePath)), serialize(p), {}};
}
QByteArray userAgent()
{
    return "LiveHime/7.64.0.10819 os/Windows pc_app/livehime build/10819 osVer/10.0_x86_64";
}
bool isBilibiliHost(const QString &host)
{
    const auto h = host.toLower();
    return h == QStringLiteral("bilibili.com") || h.endsWith(QStringLiteral(".bilibili.com"));
}
bool isApiUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") && url.userInfo().isEmpty() &&
           !url.hasFragment() && (url.port(-1) == -1 || url.port(-1) == 443) &&
           (url.host() == QStringLiteral("api.bilibili.com") ||
            url.host() == QStringLiteral("api.live.bilibili.com"));
}
QUrl coverUrl(const QString &value)
{
    if (unsafeBytes(value.toUtf8())) return {};
    QUrl url(value.startsWith(QStringLiteral("//")) ? QStringLiteral("https:") + value : value,
             QUrl::StrictMode);
    const auto host = url.host().toLower();
    const bool ownedCdn = host == QStringLiteral("hdslb.com") || host.endsWith(QStringLiteral(".hdslb.com"));
    if (!url.isValid() || !ownedCdn || !url.userInfo().isEmpty() || url.hasFragment() || url.path().isEmpty()) return {};
    if (url.scheme() == QStringLiteral("http")) {
        if (url.port(-1) != -1 && url.port(-1) != 80) return {};
        url.setScheme(QStringLiteral("https"));
        url.setPort(-1);
    } else if (url.scheme() != QStringLiteral("https") || (url.port(-1) != -1 && url.port(-1) != 443)) return {};
    return url;
}
bool isCoverUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("https") && !coverUrl(url.toString(QUrl::FullyEncoded)).isEmpty();
}
bool isCoverContentType(const QString &contentType)
{
    const auto mime = contentType.section(';', 0, 0).trimmed().toLower();
    const QStringList allowed{QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
        QStringLiteral("image/webp"), QStringLiteral("image/gif"), QStringLiteral("image/avif"), QStringLiteral("image/bmp")};
    return allowed.contains(mime);
}
bool cookieAllowed(const QNetworkCookie &cookie)
{
    QString domain = cookie.domain().toLower();
    if (domain.startsWith('.')) domain.remove(0, 1);
    if (!isBilibiliHost(domain) || domain.contains('/') || domain.contains(':') ||
        cookie.name().isEmpty() || unsafeBytes(cookie.name()) || unsafeBytes(cookie.value()) ||
        cookie.name().contains('=') || cookie.name().contains(';') || cookie.name().contains(' ') ||
        cookie.value().contains(';') || !cookie.path().startsWith('/')) return false;
    if (cookie.name().startsWith("__Secure-") && !cookie.isSecure()) return false;
    if (cookie.name().startsWith("__Host-") &&
        (!cookie.isSecure() || cookie.path() != QStringLiteral("/") || cookie.domain().startsWith('.'))) return false;
    return cookie.isSessionCookie() || cookie.expirationDate() > QDateTime::currentDateTimeUtc();
}
bool cookieMatches(const QNetworkCookie &cookie, const QUrl &url)
{
    if (!cookieAllowed(cookie) || url.scheme() != QStringLiteral("https") || !isBilibiliHost(url.host()))
        return false;
    QString domain = cookie.domain().toLower();
    const bool domainCookie = domain.startsWith('.');
    if (domainCookie) domain.remove(0, 1);
    const auto host = url.host().toLower();
    if (host != domain && !(domainCookie && host.endsWith('.' + domain))) return false;
    const auto requestPath = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    const auto path = cookie.path();
    return requestPath == path || (requestPath.startsWith(path) &&
           (path.endsWith('/') || requestPath.at(path.size()) == '/'));
}
QUrl safeUserUrl(const QString &value)
{
    const QUrl url(value, QUrl::StrictMode);
    return url.isValid() && url.scheme() == QStringLiteral("https") && isBilibiliHost(url.host()) &&
                   url.userInfo().isEmpty() && (url.port(-1) == -1 || url.port(-1) == 443)
               ? url : QUrl{};
}
ApiError transportFailure(int networkCode, int httpStatus, bool timedOut, bool tlsAvailable,
                          bool mutation)
{
    ApiError error;
    error.networkCode = networkCode;
    error.httpStatus = httpStatus;
    error.outcomeUnknown = mutation;
    if (!tlsAvailable) {
        error.kind = ApiError::Kind::Network;
        error.message = QStringLiteral("HTTPS 初始化失败：未找到可用的 TLS 后端。请重新安装插件附带的 TLS 组件后重启 OBS。");
        if (networkCode == QNetworkReply::NoError && httpStatus == 0 && !mutation) {
            error.message += QStringLiteral("（TLS 预检失败，尚未发送 HTTP 请求）");
            return error;
        }
    } else if (timedOut || networkCode == QNetworkReply::TimeoutError) {
        error.kind = ApiError::Kind::Timeout;
        error.message = QStringLiteral("请求超时。请检查网络或代理；修改操作不会自动重试。");
    } else if (httpStatus != 0 && (httpStatus < 200 || httpStatus >= 300)) {
        error.kind = ApiError::Kind::Http;
        error.message = httpStatus == 412 ? QStringLiteral("服务器拒绝了当前请求（HTTP 412）。请稍后重试，或在官方网页确认账号状态。")
                                         : QStringLiteral("服务器返回 HTTP %1。请检查会话后重试。").arg(httpStatus);
    } else {
        error.kind = ApiError::Kind::Network;
        if (networkCode == QNetworkReply::SslHandshakeFailedError)
            error.message = QStringLiteral("TLS 握手失败。请检查系统时间、证书和网络代理。");
        else if (networkCode == QNetworkReply::HostNotFoundError)
            error.message = QStringLiteral("无法解析 API 主机。请检查 DNS 和网络连接。");
        else if (networkCode >= 101 && networkCode <= 199)
            error.message = QStringLiteral("网络代理连接失败。请检查系统代理设置。");
        else error.message = QStringLiteral("网络请求失败。请检查连接后重试；修改操作不会自动重试。");
    }
    const auto meta = QMetaEnum::fromType<QNetworkReply::NetworkError>();
    const auto *name = meta.valueToKey(networkCode);
    error.message += QStringLiteral("（Qt 网络 %1/%2；HTTP %3）")
                         .arg(networkCode).arg(QString::fromLatin1(name ? name : "UnknownEnum")).arg(httpStatus);
    return error;
}
Result<QJsonObject> envelope(const QByteArray &json)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalid(QStringLiteral("服务器返回了无效 JSON。"));
    const auto object = document.object();
    const auto codeValue = object.value(QStringLiteral("code"));
    const auto code = integer(codeValue, std::numeric_limits<qint64>::min());
    if (!codeValue.isDouble() || code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max())
        return invalid(QStringLiteral("服务器响应缺少有效的业务状态码。"));
    const auto data = object.value(QStringLiteral("data")).toObject();
    if (truth(data.value(QStringLiteral("need_face_auth")))) {
        ApiError error{ApiError::Kind::Verification, QStringLiteral("需要在哔哩哔哩完成身份核验后再开播。"), static_cast<int>(code)};
        error.verificationUrl = safeUserUrl(data.value(QStringLiteral("qr")).toString());
        return error;
    }
    if (code != 0) {
        ApiError error{code == -101 || code == -111 ? ApiError::Kind::Authentication : ApiError::Kind::Api,
                       displayText(object.value(QStringLiteral("message")),
                           displayText(object.value(QStringLiteral("msg")), QStringLiteral("服务器拒绝了请求。"))),
                       static_cast<int>(code)};
        error.verificationUrl = safeUserUrl(data.value(QStringLiteral("url")).toString());
        return error;
    }
    // Successful writes may have null data; typed read parsers require their fields.
    return data;
}
Result<UserInfo> parseUser(const QJsonObject &data)
{
    const auto uid = integer(data.value(QStringLiteral("mid")));
    const auto login = data.value(QStringLiteral("isLogin"));
    // The native signed nav parser accepts code=0 with identity fields, without
    // requiring the web-only isLogin member. An explicit false is never ignored.
    if ((!login.isUndefined() && (!login.isBool() || !login.toBool())) || uid <= 0 ||
        !data.value(QStringLiteral("uname")).isString() || data.value(QStringLiteral("uname")).toString().isEmpty())
        return ApiError{ApiError::Kind::Authentication, QStringLiteral("扫码会话尚未通过身份验证，请重新登录。")};
    return UserInfo{uid, data.value(QStringLiteral("uname")).toString()};
}
Result<QList<AreaInfo>> parseAreas(const QByteArray &json)
{
    // Reuse the exact code/verification/error checks, then parse this endpoint's
    // array-valued data (ordinary account endpoints have object-valued data).
    const auto checked = envelope(json);
    if (const auto *error = std::get_if<ApiError>(&checked)) return *error;
    const auto data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data"));
    if (!data.isArray() || data.toArray().isEmpty() || data.toArray().size() > 128)
        return invalid(QStringLiteral("分区列表格式无效。"));
    QList<AreaInfo> areas;
    QSet<int> ids;
    for (const auto &parentValue : data.toArray()) {
        if (!parentValue.isObject()) return invalid(QStringLiteral("分区父分类格式无效。"));
        const auto parent = parentValue.toObject();
        const auto parentId = integer(parent.value(QStringLiteral("id")));
        const auto parentName = parent.value(QStringLiteral("name")).toString().trimmed();
        const auto children = parent.value(QStringLiteral("list"));
        if (parentId <= 0 || parentId > std::numeric_limits<int>::max() || parentName.isEmpty() || !children.isArray())
            return invalid(QStringLiteral("分区父分类缺少名称或有效 ID。"));
        for (const auto &childValue : children.toArray()) {
            if (!childValue.isObject()) return invalid(QStringLiteral("分区条目格式无效。"));
            const auto child = childValue.toObject();
            const auto id = integer(child.value(QStringLiteral("id")));
            const auto childParentId = integer(child.value(QStringLiteral("parent_id")), parentId);
            const auto name = child.value(QStringLiteral("name")).toString().trimmed();
            if (id <= 0 || id > std::numeric_limits<int>::max() || childParentId != parentId ||
                name.isEmpty() || ids.contains(static_cast<int>(id)) || areas.size() >= 20000)
                return invalid(QStringLiteral("分区条目缺少名称、ID 重复或父分类不一致。"));
            ids.insert(static_cast<int>(id));
            areas.append({static_cast<int>(id), static_cast<int>(parentId), name, parentName});
        }
    }
    if (areas.isEmpty()) return invalid(QStringLiteral("服务器返回的分区列表为空。"));
    return areas;
}
Result<RoomInfo> parseRoom(const QJsonObject &data)
{
    RoomInfo room;
    room.roomId = integer(data.value(QStringLiteral("room_id")));
    if (room.roomId <= 0) return invalid(QStringLiteral("账号没有有效直播间，请先在哔哩哔哩开通直播。"));
    room.areaId = static_cast<int>(integer(data.value(QStringLiteral("area_v2_id"))));
    room.parentAreaId = static_cast<int>(integer(data.value(QStringLiteral("parent_id"))));
    room.areaName = data.value(QStringLiteral("area_v2_name")).toString();
    // Optional forward-compatible name; the examined native parser guarantees
    // only parent_id, so the public area list remains the parent-name fallback.
    room.parentAreaName = data.value(QStringLiteral("parent_name")).toString();
    room.title = data.value(QStringLiteral("title")).toString();
    room.liveStatus = static_cast<int>(integer(data.value(QStringLiteral("live_status")), -1));
    if (integer(data.value(QStringLiteral("lock_status_v2"))) != 0) {
        room.canLive = false;
        room.blockMessage = displayText(data.value(QStringLiteral("lock_reason")), QStringLiteral("直播间当前被限制开播。"));
    }
    return room;
}
Result<RoomInfo> mergePreLive(RoomInfo room, const QJsonObject &data)
{
    if (data.isEmpty()) return invalid(QStringLiteral("开播准备信息为空，无法确认标题与封面状态。"));
    if (data.value(QStringLiteral("title")).isString()) room.title = data.value(QStringLiteral("title")).toString();
    const auto cover = data.value(QStringLiteral("cover")).toObject();
    room.coverUrl = coverUrl(cover.value(QStringLiteral("url")).toString());
    room.coverStatus = static_cast<int>(integer(cover.value(QStringLiteral("auditStatus")), -99));
    const auto audit = data.value(QStringLiteral("audit_info")).toObject();
    room.titleStatus = static_cast<int>(integer(audit.value(QStringLiteral("audit_title_status")), -99));
    const auto intercept = data.value(QStringLiteral("cover_title_start_live_intercept")).toObject();
    if (truth(intercept.value(QStringLiteral("need_intercept")))) {
        room.canLive = false;
        const auto message = displayText(intercept.value(QStringLiteral("intercept_toast")),
                                         QStringLiteral("标题或封面状态不允许开播，请在哔哩哔哩处理。"));
        room.blockMessage = room.blockMessage.isEmpty() ? message : room.blockMessage + QStringLiteral("；") + message;
    }
    return room;
}
Result<RoomInfo> preLiveFailure(RoomInfo room, const ApiError &error)
{
    if (error.kind == ApiError::Kind::Authentication) return error;
    room.canLive = false;
    const auto message = QStringLiteral("标题与封面状态读取失败，暂不能开播：") + error.message;
    room.blockMessage = room.blockMessage.isEmpty() ? message : room.blockMessage + QStringLiteral("；") + message;
    return room;
}
Result<LiveSession> parseStart(qint64 roomId, const QJsonObject &data)
{
    if (truth(data.value(QStringLiteral("need_face_auth")))) {
        ApiError error{ApiError::Kind::Verification, QStringLiteral("需要在哔哩哔哩完成身份核验后再开播。")};
        error.verificationUrl = safeUserUrl(data.value(QStringLiteral("qr")).toString());
        return error;
    }
    const auto notice = data.value(QStringLiteral("notice")).toObject();
    if (integer(notice.value(QStringLiteral("status"))) != 0) {
        ApiError error{ApiError::Kind::Verification, displayText(notice.value(QStringLiteral("msg")),
                         QStringLiteral("服务器要求先处理开播提示。"))};
        error.verificationUrl = safeUserUrl(notice.value(QStringLiteral("button_url")).toString());
        error.outcomeUnknown = true;
        return error;
    }
    // Do not reinterpret live_key/sub_session_key as an RTMP stream key.
    const auto protocols = data.value(QStringLiteral("protocols")).toArray();
    for (const auto &entry : protocols) {
        const auto item = entry.toObject();
        if (item.value(QStringLiteral("protocol")).toString() != QStringLiteral("rtmp")) continue;
        auto result = rtmp(roomId, item);
        if (std::holds_alternative<LiveSession>(result)) return result;
    }
    if (data.value(QStringLiteral("rtmp")).isObject()) {
        auto result = rtmp(roomId, data.value(QStringLiteral("rtmp")).toObject());
        if (std::holds_alternative<LiveSession>(result)) return result;
    }
    ApiError error{ApiError::Kind::UnsupportedProtocol,
        QStringLiteral("服务器未返回可用的普通 RTMP 地址；不支持将 rtmpsrt 转换为 OBS SRT。")};
    // startLive may already have opened the room even when media selection fails.
    error.outcomeUnknown = true;
    return error;
}
} // namespace livehime::protocol
