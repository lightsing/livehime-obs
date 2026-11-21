#pragma once

#include "protocol.hpp"
#include <QObject>
#include <QSet>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QHttpMultiPart;

namespace livehime {
class CookieJar;

class BiliClient final : public QObject {
    Q_OBJECT
public:
    explicit BiliClient(QObject *parent = nullptr);
    ~BiliClient() override;
    void setCookies(const QList<QNetworkCookie> &cookies);
    QList<QNetworkCookie> cookies() const;
    void logout();
    void setDeviceId(const QString &deviceId);

    void validateSession(Callback<UserInfo> callback);
    void fetchRoom(qint64 uid, Callback<RoomInfo> callback);
    void fetchAreas(Callback<QList<AreaInfo>> callback);
    void fetchCover(const QUrl &url, Callback<QByteArray> callback);
    void updateTitle(const QString &title, Callback<Done> callback);
    void uploadCover(const QString &filePath, Callback<QUrl> callback);
    void startLive(qint64 roomId, int areaId, Callback<LiveSession> callback);
    void stopLive(qint64 roomId, Callback<Done> callback);
    QNetworkReply *sendDanmaku(qint64 roomId, const QString &text, Callback<Done> callback);
    QNetworkReply *fetchRealtime(const protocol::Request &request, Callback<QJsonObject> callback);
    void cancelRealtime(QNetworkReply *reply);

signals:
    void cookiesChanged();
    void authenticationExpired();

private:
    void cancelRequests();
    QByteArray csrf() const;
    QNetworkReply *send(const protocol::Request &request, Callback<QJsonObject> callback,
              QHttpMultiPart *multipart = nullptr, bool web = false, bool requireMessage = false);
    void anonymousGet(const QUrl &url, Callback<QByteArray> callback, qint64 maxBytes,
                      const QByteArray &accept);
    std::optional<ApiError> sessionError() const;
    QNetworkAccessManager *manager_ = nullptr;
    QNetworkAccessManager *publicManager_ = nullptr;
    CookieJar *jar_ = nullptr;
    QSet<QNetworkReply *> pending_;
    quint64 generation_ = 0;
    QByteArray deviceId_;
    bool validated_ = false;
};
} // namespace livehime
