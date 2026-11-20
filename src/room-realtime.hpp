#pragma once
#include "live-controller.hpp"
#include "moderation-notice.hpp"
#include <QJsonArray>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

namespace livehime {
class GoimClient;
// One account/room context shared by all windows. Never controls OBS output.
class RoomRealtime final : public QObject {
    Q_OBJECT
  public:
    explicit RoomRealtime(LiveController *controller, QObject *parent = nullptr);
    ~RoomRealtime() override;
    void setVisible(int kind, bool visible); // 0 interaction, 1 audience, 2 guard
    void selectAudience(const QString &type, const QString &sort);
    void selectGuard(int typ);
    void refreshAudience();
    void refreshGuard();
    void loadMoreGuard();
    void stop();
    QString status() const { return status_; }
    QJsonObject rankInfo() const { return controller_ ? controller_->room().rankInfo : QJsonObject{}; }
    QJsonObject audienceData() const { return audience_; }
    QJsonObject guardData() const { return guard_; }
    QString audienceStatus() const { return audienceStatus_; }
    QString guardStatus() const { return guardStatus_; }
    bool guardHasMore() const { return guardHasMore_; }
    quint64 contextEpoch() const { return contextEpoch_; }
  signals:
    void events(const QJsonArray &batch);
    void moderationNotice(const ModerationNotice &notice);
    void contextChanged();
    void stopped();
    void rankConfigChanged();
    void statusChanged(const QString &status);
    void audienceChanged();
    void guardChanged();

  private:
    void syncContext();
    void fetchConfig();
    void connectEndpoint();
    void requestGuard(int page);
    void setStatus(const QString &status);
    QPointer<LiveController> controller_;
    GoimClient *goim_;
    QTimer refreshTimer_, retryTimer_;
    QPointer<QNetworkReply> configReply_, audienceReply_, guardReply_;
    quint64 accountEpoch_ = ~quint64(0), contextEpoch_ = 0, audienceEpoch_ = 0, guardEpoch_ = 0;
    qint64 uid_ = 0, roomId_ = 0;
    QString status_ = QStringLiteral("请先登录并加载房间");
    QString audienceType_ = QStringLiteral("online_rank"),
            audienceSort_ = QStringLiteral("contribution_rank");
    QString audienceStatus_, guardStatus_;
    QJsonObject audience_, guard_;
    QJsonObject rankConfig_;
    QJsonArray endpoints_;
    QString connectionToken_;
    int endpointIndex_ = 0;
    int guardTyp_ = 4, guardPage_ = 0;

    bool guardHasMore_ = true, stopped_ = false;
    bool visible_[3] = {false, false, false};
    bool audienceDirty_ = false, guardDirty_ = false;
    qint64 audienceRequestedAt_ = 0, guardRequestedAt_ = 0, audienceNextAt_ = 0, guardNextAt_ = 0;
};
} // namespace livehime
