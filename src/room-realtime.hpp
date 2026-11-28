#pragma once
#include "live-controller.hpp"
#include "moderation-notice.hpp"
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace livehime {
class GoimClient;
struct RealtimeConnection {
    enum class Phase { Idle, Connecting, Reconnecting, Connected };
    Phase phase = Phase::Idle;
    int attempt = 0;
    QDeadlineTimer retryAt = QDeadlineTimer::Forever;
    QString detail = QStringLiteral("请先登录并加载房间");
};
struct RoomEntry {
    QString name;
    qint64 timestamp = 0;
    QString messageId;
    qint64 sequence = 0;
    qint64 receivedAtMs = QDeadlineTimer::current(Qt::PreciseTimer).deadline();
    qint64 uid = 0, glory = 0;
    QJsonObject medal;
    QString nameColor, tailText;
    bool anonymous = false;
};
// One account/room context shared by all windows. Never controls OBS output.
class RoomRealtime final : public QObject {
    Q_OBJECT
  public:
    explicit RoomRealtime(LiveController *controller, QObject *parent = nullptr);
    ~RoomRealtime() override;
    void setVisible(int kind, bool visible); // 0 interaction, 1 audience, 2 guard
    void selectAudience(const QString &type, const QString &sort);
    void selectGuard(int typ);
    bool canSendDanmaku() const { return !stopped_ && uid_ > 0 && roomId_ > 0; }
    bool sendingDanmaku() const { return sendingDanmaku_; }
    void sendDanmaku(const QString &text);
    bool canModerate(qint64 targetUid) const {
        return canSendDanmaku() && !moderating_ && targetUid > 0 && targetUid != uid_;
    }
    void moderateUser(UserModeration action, qint64 targetUid, quint64 expectedEpoch, const QString &message = {},
                      int durationHours = 2);
    void refreshAudience();
    void refreshGuard();
    void loadMoreGuard();
    void stop();
    QString status() const { return connection_.detail; }
    RealtimeConnection connection() const { return connection_; }
    QJsonObject rankInfo() const { return controller_ ? controller_->room().rankInfo : QJsonObject{}; }
    QJsonObject audienceData() const { return audience_; }
    QJsonObject guardData() const { return guard_; }
    QString audienceStatus() const { return audienceStatus_; }
    bool audienceLoadFailed() const { return audienceLoadFailed_; }
    QString guardStatus() const { return guardStatus_; }
    bool guardHasMore() const { return guardHasMore_; }
    quint64 contextEpoch() const { return contextEpoch_; }
    const QList<RoomEntry> &entryHistory() const { return entryHistory_; }
  signals:
    void events(const QJsonArray &batch);
    void entryHistoryChanged();
    void moderationNotice(const ModerationNotice &notice);
    void contextChanged();
    void stopped();
    void danmakuSendFinished(bool accepted, const QString &message);
    void moderationFinished(bool accepted, const QString &message);
    void rankConfigChanged();
    void statusChanged(const QString &status);
    void connectionChanged(const RealtimeConnection &connection);
    void audienceChanged();
    void guardChanged();

  private:
    void syncContext();
    void fetchConfig();
    void connectEndpoint();
    void requestGuard(int page);
    void setConnection(RealtimeConnection::Phase phase, QString detail, int attempt = 0,
                       QDeadlineTimer retryAt = QDeadlineTimer::Forever);
    void connecting(const QString &detail);
    void retryConnection(const QString &detail, QDeadlineTimer retryAt = QDeadlineTimer::Forever);
    QPointer<LiveController> controller_;
    GoimClient *goim_;
    QTimer refreshTimer_, retryTimer_;
    bool sendingDanmaku_ = false;
    bool moderating_ = false;
    QPointer<QNetworkReply> moderationReply_;
    QPointer<QNetworkReply> sendReply_;
    QPointer<QNetworkReply> configReply_, audienceReply_, guardReply_;
    quint64 accountEpoch_ = ~quint64(0), contextEpoch_ = 0, audienceEpoch_ = 0, guardEpoch_ = 0;
    qint64 uid_ = 0, roomId_ = 0;
    RealtimeConnection connection_;
    QString audienceType_ = QStringLiteral("online_rank"),
            audienceSort_ = QStringLiteral("contribution_rank");
    QString audienceStatus_, guardStatus_;
    QJsonObject audience_, guard_;
    QJsonObject rankConfig_;
    QJsonArray endpoints_;
    QList<RoomEntry> entryHistory_;
    QSet<QString> entryIds_;
    QString connectionToken_;
    int endpointIndex_ = 0;
    int guardTyp_ = 4, guardPage_ = 0;

    bool guardHasMore_ = true, stopped_ = false;
    bool visible_[3] = {false, false, false};
    bool audienceDirty_ = false, guardDirty_ = false, audienceLoadFailed_ = false;
    qint64 audienceRequestedAt_ = 0, guardRequestedAt_ = 0, audienceNextAt_ = 0, guardNextAt_ = 0;
};
} // namespace livehime
