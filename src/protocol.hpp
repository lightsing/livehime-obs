#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <QUrl>
#include <functional>
#include <map>
#include <variant>

namespace livehime {

struct ApiError {
    enum class Kind { Network, Timeout, Http, Api, InvalidResponse, InvalidInput,
                      Authentication, Verification, UnsupportedProtocol, Redirect };
    Kind kind = Kind::Api;
    QString message;
    int code = 0;
    int httpStatus = 0;
    QUrl verificationUrl;
    // The server may have committed a POST when its response was lost.
    bool outcomeUnknown = false;
    int networkCode = 0; // QNetworkReply enum only; never a raw errorString/URL.
};
template<class T> using Result = std::variant<T, ApiError>;
template<class T> using Callback = std::function<void(Result<T>)>;
using Done = std::monostate;

struct UserInfo { qint64 uid = 0; QString name; };
struct AreaInfo { int id = 0; int parentId = 0; QString name; QString parentName; };
struct RoomInfo {
    qint64 roomId = 0;
    int areaId = 0;
    int parentAreaId = 0;
    QString title;
    QUrl coverUrl;
    int liveStatus = -1; // Unknown is never treated as an offline room.
    bool canLive = true;
    QString blockMessage;
    int titleStatus = -99;
    int coverStatus = -99;
    QString areaName;
    QString parentAreaName;
};
struct LiveSession { qint64 roomId = 0; QString server; QString key; };

namespace protocol {
// Byte ordering is unsigned and takes place BEFORE percent encoding.
struct ByteLess { bool operator()(const QByteArray &, const QByteArray &) const; };
using Params = std::map<QByteArray, QByteArray, ByteLess>;
struct Request {
    QByteArray method = "GET";
    QUrl url;
    QByteArray body;
    QByteArray signedBytes;
};
QByteArray escape(const QByteArray &bytes);
QByteArray serialize(const Params &params);
QByteArray signature(const Params &unsignedParams);
Params common(qint64 timestampSeconds);
Request nav(qint64 timestampSeconds);
Request title(const QString &title, const QByteArray &csrf, qint64 timestampSeconds);
Request start(qint64 roomId, int areaId, const QByteArray &csrf, qint64 timestampSeconds);
Request stop(qint64 roomId, const QByteArray &csrf, qint64 timestampSeconds);
Request room(qint64 uid, qint64 timestampSeconds);
Request preLive(qint64 timestampSeconds);
Request cover(const QUrl &location, const QByteArray &csrf, const QByteArray &visitId);
QByteArray userAgent();

bool isBilibiliHost(const QString &host);
bool isApiUrl(const QUrl &url);
QUrl coverUrl(const QString &value);
bool isCoverUrl(const QUrl &url);
bool isCoverContentType(const QString &contentType);
bool cookieAllowed(const QNetworkCookie &cookie);
bool cookieMatches(const QNetworkCookie &cookie, const QUrl &url);
QUrl safeUserUrl(const QString &value);
ApiError transportFailure(int networkCode, int httpStatus, bool timedOut, bool tlsAvailable,
                          bool mutation);
Result<QJsonObject> envelope(const QByteArray &json);
Result<UserInfo> parseUser(const QJsonObject &data);
Result<QList<AreaInfo>> parseAreas(const QByteArray &json);
Result<RoomInfo> parseRoom(const QJsonObject &data);
Result<RoomInfo> mergePreLive(RoomInfo room, const QJsonObject &data);
Result<RoomInfo> preLiveFailure(RoomInfo room, const ApiError &error);
Result<LiveSession> parseStart(qint64 roomId, const QJsonObject &data);
} // namespace protocol
} // namespace livehime
