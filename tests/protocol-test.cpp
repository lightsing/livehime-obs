#include "protocol.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <stdexcept>

using namespace livehime;
namespace p = livehime::protocol;
namespace {
int failures = 0;
void check(bool value, const char *name)
{
    if (!value) { qCritical().noquote() << "FAIL:" << name; ++failures; }
}
QJsonObject fixture(const QString &name)
{
    const auto path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/fixtures/" + name;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot load fixed evidence fixture");
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) throw std::runtime_error("invalid fixed evidence fixture");
    return document.object();
}
QByteArray fixtureBytes(const QString &name)
{
    QFile file(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/fixtures/" + name);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot load public API fixture");
    return file.readAll();
}
QJsonObject object(const char *json) { return QJsonDocument::fromJson(json).object(); }
bool errorKind(const auto &result, ApiError::Kind kind)
{
    const auto *error = std::get_if<ApiError>(&result);
    return error && error->kind == kind;
}
void originalMachineCodeVectors()
{
    const auto vectors = fixture(QStringLiteral("serialization-vectors.json"));
    const auto all = vectors.value(QStringLiteral("all_byte_escape")).toObject();
    check(p::escape(QByteArray::fromHex(all.value(QStringLiteral("input_hex")).toString().toLatin1())) ==
              all.value(QStringLiteral("output")).toString().toLatin1(), "all 256 original escape bytes");
    for (const auto &value : vectors.value(QStringLiteral("serializations")).toArray()) {
        const auto row = value.toObject();
        p::Params params;
        for (const auto &pairValue : row.value(QStringLiteral("pairs_hex")).toArray()) {
            const auto pair = pairValue.toArray();
            params[QByteArray::fromHex(pair.at(0).toString().toLatin1())] =
                QByteArray::fromHex(pair.at(1).toString().toLatin1());
        }
        check(p::serialize(params) == row.value(QStringLiteral("output")).toString().toLatin1(), "original map serialization");
    }
    for (const auto &value : vectors.value(QStringLiteral("comparisons")).toArray()) {
        const auto row = value.toObject();
        check(p::ByteLess{}(QByteArray::fromHex(row.value(QStringLiteral("left_hex")).toString().toLatin1()),
                           QByteArray::fromHex(row.value(QStringLiteral("right_hex")).toString().toLatin1())) ==
                  row.value(QStringLiteral("less")).toBool(), "original unsigned comparator");
    }
    for (const auto &value : fixture(QStringLiteral("request-vectors.json")).value(QStringLiteral("results")).toArray()) {
        const auto row = value.toObject();
        const auto operation = row.value(QStringLiteral("operation")).toString();
        p::Request request;
        if (operation == QStringLiteral("title"))
            request = p::title(QString::fromUtf8("测试 !+&= 🎮"), "synthetic-csrf", 1700000000);
        else if (operation == QStringLiteral("start")) request = p::start(12345, 1, "synthetic-csrf", 1700000000);
        else request = p::stop(12345, "synthetic-csrf", 1700000000);
        check(request.method == "POST", "request method");
        check(request.url.toEncoded() == row.value(QStringLiteral("url")).toString().toUtf8(), "original request exact URL");
        check(request.body == row.value(QStringLiteral("body")).toString().toUtf8(), "original request exact body");
        check(request.signedBytes == row.value(QStringLiteral("signed_bytes")).toString().toUtf8(), "original request signed snapshot");
    }
    bool refused = false;
    try { (void)p::signature({{"sign", "old"}}); } catch (const std::invalid_argument &) { refused = true; }
    check(refused, "refuse signing already signed inputs");
    const auto navVector = fixture(QStringLiteral("nav-vector.json"));
    const auto nav = p::nav(1700000000);
    check(nav.method == "GET" && nav.body.isEmpty(), "signed nav is a GET without body");
    check(nav.url.toEncoded() == navVector.value(QStringLiteral("url")).toString().toLatin1(), "native account nav exact URL");
    check(nav.signedBytes == navVector.value(QStringLiteral("signed_bytes")).toString().toLatin1(), "native account nav signed fields");
}
void responseFixtures()
{
    check(errorKind(p::envelope("broken"), ApiError::Kind::InvalidResponse), "reject non-JSON response");
    check(errorKind(p::envelope("[]"), ApiError::Kind::InvalidResponse), "reject array envelope");
    check(errorKind(p::envelope("{\"data\":{}}"), ApiError::Kind::InvalidResponse), "require code");
    check(errorKind(p::envelope("{\"code\":\"0\"}"), ApiError::Kind::InvalidResponse), "code has numeric type");
    check(errorKind(p::envelope("{\"code\":0.5}"), ApiError::Kind::InvalidResponse), "code has integer value");
    check(errorKind(p::envelope("{\"code\":-101,\"message\":\"session expired\"}"), ApiError::Kind::Authentication), "expired login");
    check(std::holds_alternative<QJsonObject>(p::envelope("{\"code\":0,\"data\":null}")), "write null data success");
    const auto user = p::parseUser(object(R"({"isLogin":true,"mid":12345,"uname":"Example"})"));
    check(std::holds_alternative<UserInfo>(user) && std::get<UserInfo>(user).uid == 12345, "valid nav");
    check(errorKind(p::parseUser(object(R"({"isLogin":false,"mid":12345})")), ApiError::Kind::Authentication), "nav requires login");
    check(std::holds_alternative<UserInfo>(p::parseUser(object(R"({"mid":12345,"uname":"Example","face":"https://i0.hdslb.com/synthetic.jpg"})"))), "signed native nav may omit isLogin");
    check(errorKind(p::parseUser(object(R"({"isLogin":false,"mid":12345,"uname":"Example"})")), ApiError::Kind::Authentication), "explicit logged-out nav always rejected");
    check(errorKind(p::parseUser(object(R"({"mid":12345})")), ApiError::Kind::Authentication), "nav fallback requires identity name");
    check(errorKind(p::parseUser(object(R"({"isLogin":true,"mid":9007199254740992})")), ApiError::Kind::Authentication), "reject imprecise numeric uid");
    auto room = p::parseRoom(object(R"({"room_id":12345,"area_v2_id":1,"parent_id":2,"area_v2_name":"Example area","parent_name":"Example parent","title":"Example","live_status":0,"lock_status_v2":0})"));
    check(std::holds_alternative<RoomInfo>(room), "room parse");
    check(std::get<RoomInfo>(room).areaName == QStringLiteral("Example area") &&
              std::get<RoomInfo>(room).parentAreaName == QStringLiteral("Example parent"), "room preserves readable area names");
    const auto unknown = p::parseRoom(object(R"({"room_id":12345})"));
    check(std::holds_alternative<RoomInfo>(unknown) && std::get<RoomInfo>(unknown).liveStatus == -1, "missing status remains unknown");
    auto locked = p::parseRoom(object(R"({"room_id":12345,"lock_status_v2":1,"lock_reason":"Restricted"})"));
    check(std::holds_alternative<RoomInfo>(locked) && !std::get<RoomInfo>(locked).canLive, "locked room cannot start");
    auto pre = p::mergePreLive(std::get<RoomInfo>(room), object(R"({"title":"Updated","cover":{"url":"https://i0.hdslb.com/bfs/live/cover.jpg","auditStatus":0},"audit_info":{"audit_title_status":1},"cover_title_start_live_intercept":{"need_intercept":true,"intercept_toast":"Wait for cover"}})"));
    check(std::holds_alternative<RoomInfo>(pre) && !std::get<RoomInfo>(pre).canLive &&
              std::get<RoomInfo>(pre).coverStatus == 0, "PreLive intercept and pending audit");
    const auto httpCover = p::mergePreLive(std::get<RoomInfo>(room), object(R"({"cover":{"url":"http://i0.hdslb.com/bfs/live/cover.jpg","auditStatus":1}})"));
    check(std::holds_alternative<RoomInfo>(httpCover) &&
              std::get<RoomInfo>(httpCover).coverUrl == QUrl(QStringLiteral("https://i0.hdslb.com/bfs/live/cover.jpg")),
          "native PreLive cover.url HTTP CDN upgrades to HTTPS");
    check(errorKind(p::mergePreLive(std::get<RoomInfo>(room), {}), ApiError::Kind::InvalidResponse), "empty PreLive is not success");
    auto knownLive = std::get<RoomInfo>(locked);
    knownLive.liveStatus = 1;
    const auto partial = p::preLiveFailure(knownLive, ApiError{ApiError::Kind::Timeout, QStringLiteral("Synthetic timeout")});
    check(std::holds_alternative<RoomInfo>(partial) && std::get<RoomInfo>(partial).roomId == 12345 &&
              std::get<RoomInfo>(partial).liveStatus == 1 && !std::get<RoomInfo>(partial).canLive &&
              std::get<RoomInfo>(partial).blockMessage.startsWith(QStringLiteral("Restricted")),
          "PreLive failure preserves known live room and lock while blocking new start");
    check(errorKind(p::preLiveFailure(knownLive, ApiError{ApiError::Kind::Authentication, QStringLiteral("Expired")}),
                    ApiError::Kind::Authentication), "PreLive authentication failure remains an error");
    const auto live = p::parseStart(12345, object(R"({"protocols":[{"protocol":"srt","addr":"rtmpsrt://example.invalid/live/","code":"synthetic"},{"protocol":"rtmp","addr":"rtmp://example.invalid/live/","code":"stream?synthetic=1"}],"live_key":"different-session-field"})"));
    check(std::holds_alternative<LiveSession>(live) && std::get<LiveSession>(live).key == QStringLiteral("stream?synthetic=1"), "select explicit ordinary RTMP key");
    const auto legacy = p::parseStart(12345, object(R"({"rtmp":{"addr":"rtmp://example.invalid/live/","code":"synthetic","new_link":""}})"));
    check(std::holds_alternative<LiveSession>(legacy), "legacy rtmp addr/code");
    const auto noRtmp = p::parseStart(12345, object(R"({"protocols":[{"protocol":"rtmp","addr":"rtmpsrt://example.invalid/live/","code":"synthetic"}],"live_key":"not-a-stream-key"})"));
    check(errorKind(noRtmp, ApiError::Kind::UnsupportedProtocol) && std::get<ApiError>(noRtmp).outcomeUnknown, "unsupported response might already have opened room");
    const auto face = p::parseStart(12345, object(R"({"need_face_auth":true,"qr":"https://passport.bilibili.com/verify","rtmp":{"addr":"rtmp://example.invalid/live/","code":"synthetic"}})"));
    check(errorKind(face, ApiError::Kind::Verification) && !std::get<ApiError>(face).verificationUrl.isEmpty(), "face check precedes stream selection");
    const auto badFace = p::envelope(R"({"code":0,"data":{"need_face_auth":true,"qr":"https://bilibili.com.evil.invalid/verify"}})");
    check(errorKind(badFace, ApiError::Kind::Verification) && std::get<ApiError>(badFace).verificationUrl.isEmpty(), "reject foreign verification URL");
    check(errorKind(p::parseStart(12345, object(R"({"notice":{"status":1,"msg":"Confirm","button_url":"https://live.bilibili.com/verify"},"rtmp":{"addr":"rtmp://example.invalid/live/","code":"synthetic"}})")), ApiError::Kind::Verification), "risk notice blocks push");
}
void areaFixtures()
{
    const auto result = p::parseAreas(fixtureBytes(QStringLiteral("areas-response.json")));
    check(std::holds_alternative<QList<AreaInfo>>(result), "public official area list parses");
    if (const auto *areas = std::get_if<QList<AreaInfo>>(&result)) {
        check(areas->size() == 455, "all public fixture area entries retained");
        const auto first = areas->first();
        check(first.id == 86 && first.parentId == 2 && first.name == QStringLiteral("英雄联盟") &&
                  first.parentName == QStringLiteral("网游"), "public area IDs and Chinese names preserved");
    }
    check(errorKind(p::parseAreas(R"({"code":-101,"data":[]})"), ApiError::Kind::Authentication), "area business failure checked");
    check(errorKind(p::parseAreas(R"({"code":0,"data":{}})"), ApiError::Kind::InvalidResponse), "area data must be array");
    check(errorKind(p::parseAreas(R"({"code":0,"data":[]})"), ApiError::Kind::InvalidResponse), "empty areas rejected");
    check(errorKind(p::parseAreas(R"({"code":0,"data":[{"id":1,"name":"Parent","list":[{"id":"10","parent_id":"2","name":"Child"}]}]})"), ApiError::Kind::InvalidResponse), "area parent mismatch rejected");
    check(errorKind(p::parseAreas(R"({"code":0,"data":[{"id":1,"name":"Parent","list":[{"id":"10","name":"Child"},{"id":10,"name":"Duplicate"}]}]})"), ApiError::Kind::InvalidResponse), "duplicate area IDs rejected");
    const auto mixed = p::parseAreas(R"({"code":0,"data":[{"id":1,"name":"Parent","list":[{"id":"10","parent_id":"1","name":"Text ID"},{"id":11,"parent_id":1,"name":"Numeric ID"}]}]})");
    check(std::holds_alternative<QList<AreaInfo>>(mixed) && std::get<QList<AreaInfo>>(mixed).size() == 2,
          "area numeric and string IDs supported");
}
void transportDiagnostics()
{
    const auto http = p::transportFailure(QNetworkReply::ContentOperationNotPermittedError, 412, false, true, false);
    check(http.kind == ApiError::Kind::Http && http.httpStatus == 412 &&
              http.networkCode == QNetworkReply::ContentOperationNotPermittedError,
          "HTTP rejection takes precedence over Qt reply network error");
    check(http.message.contains(QStringLiteral("ContentOperationNotPermittedError")) &&
              http.message.contains(QStringLiteral("412")), "safe enum and HTTP diagnostics visible");
    const auto noTls = p::transportFailure(QNetworkReply::NoError, 0, false, false, false);
    check(noTls.kind == ApiError::Kind::Network && noTls.message.contains(QStringLiteral("TLS")) &&
              !noTls.outcomeUnknown, "TLS preflight failure cannot have committed a request");
    const auto timeout = p::transportFailure(QNetworkReply::OperationCanceledError, 0, true, true, true);
    check(timeout.kind == ApiError::Kind::Timeout && timeout.outcomeUnknown, "POST timeout preserves unknown outcome");
    const auto proxy = p::transportFailure(QNetworkReply::ProxyConnectionRefusedError, 0, false, true, false);
    check(proxy.message.contains(QStringLiteral("代理")) && !proxy.message.contains(QStringLiteral("https://")),
          "proxy diagnostic contains no request URL");
}
void cookieAndUrlBoundaries()
{
    QNetworkCookie cookie("bili_jct", "synthetic");
    cookie.setDomain(QStringLiteral(".bilibili.com"));
    cookie.setPath(QStringLiteral("/"));
    cookie.setSecure(true);
    cookie.setHttpOnly(true);
    check(p::cookieAllowed(cookie), "accept in-scope cookie");
    check(p::cookieMatches(cookie, QUrl("https://api.live.bilibili.com/room/v1")), "domain cookie matches subdomain");
    check(!p::cookieMatches(cookie, QUrl("https://bilibili.com.evil.invalid/")), "suffix confusion blocked");
    check(!p::cookieMatches(cookie, QUrl("http://api.live.bilibili.com/")), "cookies never sent over HTTP");
    cookie.setDomain(QStringLiteral("api.bilibili.com"));
    check(!p::cookieMatches(cookie, QUrl("https://sub.api.bilibili.com/")), "host-only cookie stays exact");
    cookie.setPath(QStringLiteral("/room"));
    check(p::cookieMatches(cookie, QUrl("https://api.bilibili.com/room/info")), "path directory matches");
    check(!p::cookieMatches(cookie, QUrl("https://api.bilibili.com/roommate")), "path prefix confusion blocked");
    cookie.setDomain(QStringLiteral(".com"));
    check(!p::cookieAllowed(cookie), "public suffix cookie blocked");
    cookie.setDomain(QStringLiteral(".bilibili.com"));
    cookie.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-60));
    check(!p::cookieAllowed(cookie), "expired cookies rejected");
    cookie.setExpirationDate({});
    cookie.setValue("bad\r\nInjected: 1");
    check(!p::cookieAllowed(cookie), "cookie header injection blocked");
    check(p::isApiUrl(QUrl("https://api.live.bilibili.com/room/v1/Room/startLive")), "known HTTPS API permitted");
    check(!p::isApiUrl(QUrl("https://api.live.bilibili.com.evil.invalid/")), "foreign API rejected");
    check(!p::isApiUrl(QUrl("https://api.live.bilibili.com:444/")), "foreign port rejected");
    check(p::safeUserUrl(QStringLiteral("javascript:alert(1)")).isEmpty(), "unsafe user action scheme rejected");
    check(p::coverUrl(QStringLiteral("http://i0.hdslb.com/bfs/live/cover.jpg")) == QUrl(QStringLiteral("https://i0.hdslb.com/bfs/live/cover.jpg")), "owned image HTTP upgrades");
    check(p::coverUrl(QStringLiteral("//i1.hdslb.com/bfs/live/cover.png")) == QUrl(QStringLiteral("https://i1.hdslb.com/bfs/live/cover.png")), "scheme-relative owned image upgrades");
    check(p::isCoverUrl(QUrl(QStringLiteral("https://i0.hdslb.com/bfs/live/cover.jpg"))), "HTTPS owned image accepted");
    check(!p::isCoverUrl(QUrl(QStringLiteral("http://i0.hdslb.com/bfs/live/cover.jpg"))), "download predicate requires HTTPS");
    check(p::isCoverContentType(QStringLiteral("image/jpeg")) &&
              p::isCoverContentType(QStringLiteral("Image/PNG; charset=binary")), "raster content types accepted");
    for (const auto &mime : {QStringLiteral("text/html"), QStringLiteral("application/json"),
                            QStringLiteral("image/svg+xml"), QStringLiteral("application/octet-stream"), QString{}})
        check(!p::isCoverContentType(mime), "non-raster or missing image type rejected");
    for (const auto &url : {QStringLiteral("https://i0.hdslb.com.evil.invalid/cover.jpg"),
                           QStringLiteral("https://evilhdslb.com/cover.jpg"),
                           QStringLiteral("https://user@i0.hdslb.com/cover.jpg"),
                           QStringLiteral("http://i0.hdslb.com:8080/cover.jpg"),
                           QStringLiteral("https://i0.hdslb.com/cover.jpg#fragment"),
                           QStringLiteral("file:///C:/cover.jpg"),
                           QStringLiteral("https://api.bilibili.com/cover.jpg")})
        check(p::coverUrl(url).isEmpty(), "non-image-origin cover URL rejected");
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        const auto danmu = p::danmuInfo(123, 1700000000);
        const auto query = danmu.url.query(QUrl::FullyEncoded).toUtf8();
        check(danmu.method == "GET" && danmu.body.isEmpty() && query.contains("access_key=&") &&
              query.contains("is_anchor=true") && query.contains("user_type=0") && !query.contains("build="),
              "anchor danmu GET exact common and credential scope");
        p::Params params = p::common(1700000000);
        params.erase("build"); params["room_id"] = "123"; params["user_type"] = "0"; params["is_anchor"] = "true";
        check(query.endsWith("version=7.64.0.10819") && query.contains("sign=" + p::signature(params)), "danmu signs all business fields");
        for (const auto &pair : QList<QPair<QString, QString>>{{"online_rank", "contribution_rank"}, {"online_rank", "entry_time_rank"},
            {"daily_rank", "today_rank"}, {"daily_rank", "yesterday_rank"}, {"weekly_rank", "current_week_rank"},
            {"weekly_rank", "last_week_rank"}, {"monthly_rank", "current_month_rank"}, {"monthly_rank", "last_month_rank"}, {"intimacy_rank", "intimacy_rank"}}) {
            const auto rank = p::contributionRank(9, 123, pair.first, pair.second, 1700000000);
            check(rank.body.isEmpty() && rank.signedBytes.contains("page_size=100") && rank.signedBytes.contains("room_id=123") &&
                rank.signedBytes.contains("switch=" + pair.second.toUtf8()) && !rank.signedBytes.contains("sign="), "rank unsigned input carries selected mode and page size");
        }
        const auto guard = p::guardRank(9, 123, 5, 2, 1700000000);
        check(guard.signedBytes.contains("roomid=123") && guard.signedBytes.contains("typ=5") &&
            guard.signedBytes.contains("page=2") && guard.signedBytes.contains("page_size=30"), "guard exact spelling and mode pagination");
        const auto parsed = p::parseRoom(object(R"({"room_id":123,"room_rank_info":{"count":42}})"));
        check(std::holds_alternative<RoomInfo>(parsed) && std::get<RoomInfo>(parsed).rankInfo.value("count").toInt() == 42, "room rank config preserved independently from rank data.config");
    }
    try {
        originalMachineCodeVectors();
        responseFixtures();
        cookieAndUrlBoundaries();
        transportDiagnostics();
        areaFixtures();
    } catch (const std::exception &exception) {
        qCritical() << "Fixture setup failed:" << exception.what();
        return 2;
    }
    if (failures) return 1;
    qInfo() << "Protocol golden vectors, response fixtures and cookie boundaries passed.";
    return 0;
}
