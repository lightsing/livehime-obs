#pragma once
#include "bili-client.hpp"
#include "obs-bridge.hpp"
#include "session-store.hpp"
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <optional>

namespace livehime {
class LiveController final : public QObject {
    Q_OBJECT
public:
    enum class State { SignedOut, Ready, Preparing, Connecting, Live, Reconnecting, Recoverable, Uncertain, Stopping };
    explicit LiveController(QString storageDirectory, QObject *parent = nullptr);
    ~LiveController() override;
    void initialize();
    void acceptCookies(const QList<QNetworkCookie> &cookies);
    void logout();
    void refresh();
    void setTitleDraft(const QString &title);
    void flushTitle();
    void saveCover(const QString &filePath);
    const QString &titleDraft() const { return titleDraft_; }
    QString titleSyncStatus() const;
    bool metadataBusy() const;
    void saveMetadata(const QString &title, const QString &coverFile);
    void start(const QString &title, const QString &coverFile, int areaId);
    void retryConnection();
    void stop();

    State state() const { return state_; }
    bool working() const { return working_; }
    bool authenticated() const { return authenticated_; }
    bool hasRemoteSession() const { return remoteRoomId_ > 0; }
    bool hasStreamCredentials() const { return session_.has_value(); }
    bool outputBusy() const { return bridge_->busy(); }
    bool ownsOutput() const { return bridge_->ownsService(); }
    bool canCancelStart() const { return pendingStartArea_.has_value() && !remoteRoomId_; }
    bool canStop() const
    {
        return canCancelStart() || (remoteRoomId_ && !stoppingRequest_ && (!working_ || metadataRefreshing_) &&
                                   state_ != State::Stopping);
    }
    const UserInfo &user() const { return user_; }
    const RoomInfo &room() const { return room_; }
    QString status() const { return status_; }
    QUrl verificationUrl() const { return verificationUrl_; }
    const QList<AreaInfo> &areas() const { return areas_; }
    bool areasLoading() const { return areasLoading_; }
    QString areasError() const { return areasError_; }
    const QByteArray &coverBytes() const { return coverBytes_; }
    bool coverLoading() const { return coverLoading_; }
    QString coverError() const { return coverError_; }
    QString browserDataDirectory() const { return store_.browserDataDirectory(); }
    QString deviceId() const { return deviceId_; }

signals:
    void changed();
    void roomChanged();
    void metadataSaved();
    void titleSyncChanged();
    void areasChanged();
    void coverChanged();
private:
    void setState(State state, const QString &message);
    void fail(const ApiError &error, State fallback);
    void validate();
    void loadRoom(std::function<void()> continuation = {});
    enum class MetadataKind { None, Title, Cover };
    void pumpMetadata();
    void beginQueuedStart();
    void metadataFailed(const ApiError &error, MetadataKind kind, quint64 sentTitleRevision);
    void cancelMetadata(bool clearDraft);
    void notifyMetadata();
    bool titleDirty() const;
    void beginOutput();
    void stopRemote();
    void persistCookies();
    void loadAreas();
    void loadCurrentCover();
    SessionStore store_;
    QString deviceId_;
    BiliClient *client_;
    ObsBridge *bridge_;
    State state_ = State::SignedOut;
    UserInfo user_;
    RoomInfo room_;
    std::optional<LiveSession> session_;
    qint64 remoteRoomId_ = 0;
    bool working_ = false;
    bool authenticated_ = false;
    bool stoppingRequest_ = false;
    quint64 operation_ = 0;
    QTimer titleDebounce_;
    QString titleDraft_;
    QString confirmedTitle_;
    QString titleError_;
    QString coverSyncError_;
    QString failedCover_;
    QString lastCoverRequested_;
    QString activeMetadataValue_;
    bool titleInitialized_ = false;
    bool awaitingTitleEcho_ = false;
    bool titleReady_ = false;
    bool metadataRefreshPending_ = false;
    bool metadataRefreshing_ = false;
    bool metadataBatching_ = false;
    MetadataKind metadataKind_ = MetadataKind::None;
    QQueue<QString> coverQueue_;
    std::optional<int> pendingStartArea_;
    quint64 titleRevision_ = 0;
    quint64 metadataGeneration_ = 0;
    quint64 metadataRevision_ = 0;
    QString status_ = QStringLiteral("请扫码登录 B 站账号。");
    QUrl verificationUrl_;
    QList<AreaInfo> areas_;
    bool areasLoading_ = false;
    QString areasError_;
    quint64 areasRequest_ = 0;
    QByteArray coverBytes_;
    bool coverLoading_ = false;
    QString coverError_;
    QUrl coverRequestUrl_;
    quint64 coverRequest_ = 0;
};
}
