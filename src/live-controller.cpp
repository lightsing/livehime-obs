#include "live-controller.hpp"
#include <QTimer>

namespace livehime {
LiveController::LiveController(QString storageDirectory, QObject *parent)
    : QObject(parent), store_(std::move(storageDirectory)), client_(new BiliClient(this)), bridge_(new ObsBridge(this))
{
    QString deviceError;
    deviceId_ = store_.deviceId(&deviceError);
    if (!deviceError.isEmpty()) status_ = deviceError;
    client_->setDeviceId(deviceId_);
    titleDebounce_.setSingleShot(true);
    titleDebounce_.setInterval(1000);
    connect(&titleDebounce_, &QTimer::timeout, this, [this] {
        titleReady_ = true;
        notifyMetadata();
        pumpMetadata();
    });
    connect(bridge_, &ObsBridge::availabilityChanged, this, &LiveController::changed);
    connect(client_, &BiliClient::cookiesChanged, this, &LiveController::persistCookies);
    connect(client_, &BiliClient::authenticationExpired, this, [this] {
        if (!authenticated_) return;
        authenticated_ = false; ++accountEpoch_;
        status_ = QStringLiteral("B站登录已失效，请重新登录；OBS 输出状态未更改。");
        emit changed();
    });
    connect(bridge_, &ObsBridge::started, this, [this] {
        if (remoteRoomId_ && state_ != State::Stopping) setState(State::Live, QStringLiteral("直播中 · OBS 正在直接推流。"));
    });
    connect(bridge_, &ObsBridge::stopped, this, [this](bool intentional) {
        if (!remoteRoomId_) return;
        if (intentional || state_ == State::Stopping) stopRemote();
        else setState(State::Recoverable, QStringLiteral("OBS 输出已中断。本场信息已保留，可重试连接或下播。"));
    });
    connect(bridge_, &ObsBridge::connectionFailed, this, [this](const QString &message) {
        if (remoteRoomId_ && state_ != State::Stopping) setState(State::Recoverable, message);
    });
    connect(bridge_, &ObsBridge::reconnecting, this, [this] {
        if (remoteRoomId_ && state_ != State::Stopping) setState(State::Reconnecting, QStringLiteral("网络中断，OBS 正在自动重连本场直播…"));
    });
    connect(bridge_, &ObsBridge::reconnected, this, [this] {
        if (remoteRoomId_ && state_ != State::Stopping) setState(State::Live, QStringLiteral("直播中 · OBS 已重新连接。"));
    });
}
LiveController::~LiveController() = default;

void LiveController::setState(State state, const QString &message)
{ state_ = state; status_ = message; emit changed(); }

void LiveController::persistCookies()
{
    if (!authenticated_) return;
    QString error;
    if (!store_.saveCookies(client_->cookies(), &error)) {
        status_ = QStringLiteral("当前登录可用，但无法保存会话：") + error;
        emit changed();
    }
}

void LiveController::initialize()
{
    QString error;
    auto cookies = store_.loadCookies(&error);
    if (!error.isEmpty()) { status_ = error + QStringLiteral(" 请重新扫码登录。"); emit changed(); }
    if (!cookies.isEmpty()) acceptCookies(cookies);
}

void LiveController::acceptCookies(const QList<QNetworkCookie> &cookies)
{
    if (working_ || (remoteRoomId_ && authenticated_)) return;
    ++accountEpoch_;
    cancelMetadata(true);
    ++areasRequest_; ++coverRequest_;
    areasLoading_ = false; coverLoading_ = false;
    authenticated_ = false;
    client_->setCookies(cookies);
    validate();
}

void LiveController::validate()
{
    const auto operation = ++operation_;
    working_ = true;
    status_ = QStringLiteral("正在验证登录…"); emit changed();
    client_->validateSession([this, operation](Result<UserInfo> result) {
        if (operation != operation_) return;
        if (auto *error = std::get_if<ApiError>(&result)) { fail(*error, State::SignedOut); return; }
        const auto identity = std::get<UserInfo>(result);
        if (remoteRoomId_ && user_.uid && identity.uid != user_.uid) {
            client_->logout();
            fail({ApiError::Kind::Authentication, QStringLiteral("还有未结束的直播，请重新登录原账号后下播。")}, State::Uncertain);
            return;
        }
        user_ = identity;
        authenticated_ = true;
        persistCookies();
        working_ = false;
        loadAreas();
        loadRoom();
    });
}

void LiveController::logout()
{
    if (working_ || remoteRoomId_ || bridge_->busy()) return;
    ++accountEpoch_;
    ++operation_;
    cancelMetadata(true);
    authenticated_ = false;
    client_->logout();
    ++areasRequest_; ++coverRequest_;
    areasLoading_ = false; coverLoading_ = false;
    coverBytes_.clear(); coverError_.clear(); coverRequestUrl_ = {};
    user_ = {}; room_ = {}; session_.reset(); verificationUrl_ = {};
    QString error;
    store_.clear(&error);
    setState(State::SignedOut, error.isEmpty() ? QStringLiteral("已退出登录。") : error);
    emit roomChanged();
    emit coverChanged();
}

void LiveController::fail(const ApiError &error, State fallback)
{
    working_ = false;
    metadataRefreshing_ = false;
    pendingStartArea_.reset();
    verificationUrl_ = error.verificationUrl;
    if (error.kind == ApiError::Kind::Authentication) authenticated_ = false;
    QString message = error.message;
    if (error.code) message += QStringLiteral("（代码 %1）").arg(error.code);
    if (error.outcomeUnknown) message += QStringLiteral(" 请求结果未确认，请刷新房间状态；不要重复开播。");
    setState(remoteRoomId_ && fallback == State::SignedOut ? State::Uncertain : fallback, message);
}

void LiveController::refresh()
{
    if (working_ || metadataBusy() || pendingStartArea_) {
        metadataRefreshPending_ = true;
        return;
    }
    ++operation_;
    if (!authenticated_) { validate(); return; }
    loadAreas();
    loadRoom();
}

void LiveController::loadRoom(std::function<void()> continuation)
{
    const auto operation = operation_;
    const auto titleRevision = titleRevision_;
    const auto metadataRevision = metadataRevision_;
    working_ = true;
    status_ = QStringLiteral("正在读取房间和审核状态…"); emit changed();
    client_->fetchRoom(user_.uid, [this, operation, titleRevision, metadataRevision,
                                  continuation = std::move(continuation)](Result<RoomInfo> result) {
        if (operation != operation_) return;
        if (auto *error = std::get_if<ApiError>(&result)) {
            const bool backgroundRead = metadataRefreshing_;
            const auto fallback = backgroundRead ? (state_ == State::Preparing ? State::Ready : state_)
                                                 : (remoteRoomId_ ? State::Uncertain : State::Ready);
            fail(*error, fallback);
            if (backgroundRead) pumpMetadata();
            return;
        }
        room_ = std::get<RoomInfo>(result);
        roomAccountEpoch_ = accountEpoch_;
        working_ = false;
        metadataRefreshing_ = false;
        // A read begun before a draft edit or an accepted write cannot roll the
        // editor back. An untouched/synchronized editor can follow a refresh.
        const bool titleEchoed = room_.title.trimmed() == confirmedTitle_;
        if (!titleInitialized_ || (!pendingStartArea_ && metadataKind_ != MetadataKind::Title &&
                                  !titleDirty() && titleRevision == titleRevision_ &&
                                  metadataRevision == metadataRevision_ &&
                                  (!awaitingTitleEcho_ || titleEchoed))) {
            titleDraft_ = room_.title;
            confirmedTitle_ = room_.title.trimmed();
            titleInitialized_ = true;
            titleError_.clear();
        }
        if (titleEchoed) awaitingTitleEcho_ = false;
        notifyMetadata();
        emit roomChanged();
        loadCurrentCover();
        if (continuation) { continuation(); return; }
        if (bridge_->busy()) {
            status_ = state_ == State::Reconnecting ? QStringLiteral("OBS 正在自动重连。房间信息已更新。")
                                                   : QStringLiteral("OBS 输出运行中。房间与审核状态已更新。");
            if (!coverSyncError_.isEmpty()) status_ = QStringLiteral("封面上传失败：") + coverSyncError_;
            emit changed(); pumpMetadata(); return;
        }
        if (room_.liveStatus == 1) {
            remoteRoomId_ = room_.roomId;
            setState(session_ ? State::Recoverable : State::Uncertain,
                     session_ ? QStringLiteral("房间仍在直播，可重试 OBS 连接或下播。")
                              : QStringLiteral("房间已在直播，但没有本场推流信息。可先下播，再一键开播。"));
        } else if (room_.liveStatus == 0 || room_.liveStatus == 2) {
            remoteRoomId_ = 0;
            session_.reset();
            bridge_->restoreService();
            setState(State::Ready, room_.canLive ? QStringLiteral("已就绪。设置标题、封面和分区后可一键开播。") : room_.blockMessage);
        } else {
            setState(State::Uncertain, QStringLiteral("房间直播状态未知，请稍后刷新或在网页确认。"));
        }
        if (!coverSyncError_.isEmpty()) { status_ = QStringLiteral("封面上传失败：") + coverSyncError_; emit changed(); }
        pumpMetadata();
    });
}

void LiveController::loadAreas()
{
    if (areasLoading_) return;
    const auto request = ++areasRequest_;
    areasLoading_ = true;
    areasError_.clear();
    emit areasChanged();
    client_->fetchAreas([this, request](Result<QList<AreaInfo>> result) {
        if (request != areasRequest_) return;
        areasLoading_ = false;
        if (auto *error = std::get_if<ApiError>(&result)) areasError_ = error->message;
        else { areas_ = std::get<QList<AreaInfo>>(result); areasError_.clear(); }
        emit areasChanged();
    });
}

void LiveController::loadCurrentCover()
{
    if (coverLoading_ && coverRequestUrl_ == room_.coverUrl) return;
    const auto request = ++coverRequest_;
    coverRequestUrl_ = room_.coverUrl;
    coverBytes_.clear();
    coverError_.clear();
    coverLoading_ = !room_.coverUrl.isEmpty();
    emit coverChanged();
    if (!coverLoading_) return;
    client_->fetchCover(room_.coverUrl, [this, request](Result<QByteArray> result) {
        if (request != coverRequest_) return;
        coverLoading_ = false;
        if (auto *error = std::get_if<ApiError>(&result)) coverError_ = error->message;
        else coverBytes_ = std::get<QByteArray>(result);
        emit coverChanged();
    });
}

bool LiveController::titleDirty() const
{ return titleInitialized_ && titleDraft_.trimmed() != confirmedTitle_; }

bool LiveController::metadataBusy() const
{
    return metadataKind_ != MetadataKind::None || metadataRefreshing_ || titleDebounce_.isActive() ||
           (titleReady_ && titleDirty() && titleError_.isEmpty()) || !coverQueue_.isEmpty();
}

QString LiveController::titleSyncStatus() const
{
    if (!authenticated_ || !titleInitialized_) return QStringLiteral("登录后同步标题");
    if (titleDraft_.trimmed().isEmpty()) return QStringLiteral("标题不能为空");
    if (!titleError_.isEmpty()) return QStringLiteral("同步失败：") + titleError_;
    if (metadataKind_ == MetadataKind::Title)
        return titleDraft_.trimmed() == activeMetadataValue_ ? QStringLiteral("正在同步…")
                                                           : QStringLiteral("新标题待同步…");
    return titleDirty() ? QStringLiteral("待同步…") : QStringLiteral("已同步");
}

void LiveController::notifyMetadata()
{ emit titleSyncChanged(); emit changed(); }

void LiveController::cancelMetadata(bool clearDraft)
{
    ++metadataGeneration_;
    ++metadataRevision_;
    titleDebounce_.stop();
    titleReady_ = false;
    pendingStartArea_.reset();
    metadataRefreshPending_ = false;
    metadataRefreshing_ = false;
    if (!clearDraft) {
        if (!coverQueue_.isEmpty()) failedCover_ = coverQueue_.back();
        else if (metadataKind_ == MetadataKind::Cover) failedCover_ = activeMetadataValue_;
        if (!failedCover_.isEmpty()) coverSyncError_ = QStringLiteral("封面同步已取消；再次开播或重新保存时可重试。");
    }
    metadataKind_ = MetadataKind::None;
    activeMetadataValue_.clear();
    coverQueue_.clear();
    if (clearDraft) {
        titleDraft_.clear(); confirmedTitle_.clear(); titleError_.clear();
        coverSyncError_.clear(); failedCover_.clear(); lastCoverRequested_.clear();
        titleInitialized_ = false;
        awaitingTitleEcho_ = false;
        titleRevision_ = 0;
    }
    notifyMetadata();
}

void LiveController::setTitleDraft(const QString &title)
{
    if (!authenticated_ || title == titleDraft_) return;
    titleDraft_ = title;
    titleInitialized_ = true;
    ++titleRevision_;
    titleError_.clear();
    titleReady_ = false;
    titleDebounce_.stop();
    const bool needsSave = titleDirty() ||
        (metadataKind_ == MetadataKind::Title && title.trimmed() != activeMetadataValue_);
    if (!title.trimmed().isEmpty() && needsSave) {
        if (pendingStartArea_) titleReady_ = true;
        else titleDebounce_.start();
    }
    notifyMetadata();
    pumpMetadata();
}

void LiveController::flushTitle()
{
    if (!authenticated_ || state_ == State::Stopping) return;
    titleDebounce_.stop();
    titleError_.clear();
    titleReady_ = !titleDraft_.trimmed().isEmpty();
    notifyMetadata();
    pumpMetadata();
}

void LiveController::saveCover(const QString &filePath)
{
    if (!authenticated_ || filePath.isEmpty()) return;
    coverQueue_.enqueue(filePath);
    lastCoverRequested_ = filePath;
    failedCover_.clear();
    coverSyncError_.clear();
    ++metadataRevision_;
    status_ = metadataKind_ == MetadataKind::None ? QStringLiteral("正在上传封面…")
                                                : QStringLiteral("封面已加入上传队列…");
    notifyMetadata();
    pumpMetadata();
}

void LiveController::metadataFailed(const ApiError &error, MetadataKind kind, quint64 sentTitleRevision)
{
    pendingStartArea_.reset(); // A failed save never silently resumes a start intent.
    metadataRefreshPending_ = false;
    verificationUrl_ = error.verificationUrl;
    if (error.kind == ApiError::Kind::Authentication) authenticated_ = false;
    QString detail = error.message;
    if (error.code) detail += QStringLiteral("（代码 %1）").arg(error.code);
    if (kind == MetadataKind::Title) {
        if (sentTitleRevision == titleRevision_) {
            titleError_ = detail;
            titleReady_ = false;
            titleDebounce_.stop();
        }
        status_ = QStringLiteral("标题同步失败：") + detail + QStringLiteral("。请编辑标题或失焦重试。");
    } else {
        failedCover_ = activeMetadataValue_;
        coverSyncError_ = detail;
        status_ = QStringLiteral("封面上传失败：") + detail + QStringLiteral("。请重新保存封面，或再次点击开播重试。");
    }
    metadataKind_ = MetadataKind::None;
    activeMetadataValue_.clear();
    if (state_ == State::Preparing) state_ = State::Ready;
    notifyMetadata();
    // A newer explicitly entered draft / confirmed crop remains queued, but
    // the failed revision itself is not retried automatically.
    pumpMetadata();
}

void LiveController::pumpMetadata()
{
    if (metadataBatching_ || !authenticated_ || working_ || metadataKind_ != MetadataKind::None || state_ == State::Stopping) return;
    const auto generation = metadataGeneration_;
    if (titleReady_ && titleDirty() && titleError_.isEmpty() && !titleDraft_.trimmed().isEmpty()) {
        const QString title = titleDraft_.trimmed();
        const auto revision = titleRevision_;
        metadataKind_ = MetadataKind::Title;
        activeMetadataValue_ = title;
        titleReady_ = false;
        verificationUrl_ = {};
        notifyMetadata();
        client_->updateTitle(title, [this, generation, revision, title](Result<Done> result) {
            if (generation != metadataGeneration_) return;
            if (auto *error = std::get_if<ApiError>(&result)) { metadataFailed(*error, MetadataKind::Title, revision); return; }
            confirmedTitle_ = title;
            awaitingTitleEcho_ = true;
            room_.title = title;
            ++metadataRevision_;
            metadataKind_ = MetadataKind::None;
            activeMetadataValue_.clear();
            titleError_.clear();
            if (!titleDirty()) { titleReady_ = false; titleDebounce_.stop(); }
            metadataRefreshPending_ = true;
            emit metadataSaved();
            notifyMetadata();
            pumpMetadata();
        });
        return;
    }
    if (!coverQueue_.isEmpty()) {
        const QString path = coverQueue_.dequeue();
        metadataKind_ = MetadataKind::Cover;
        activeMetadataValue_ = path;
        status_ = QStringLiteral("正在上传并保存封面…");
        notifyMetadata();
        client_->uploadCover(path, [this, generation](Result<QUrl> result) {
            if (generation != metadataGeneration_) return;
            if (auto *error = std::get_if<ApiError>(&result)) { metadataFailed(*error, MetadataKind::Cover, 0); return; }
            room_.coverUrl = std::get<QUrl>(result);
            ++metadataRevision_;
            metadataKind_ = MetadataKind::None;
            activeMetadataValue_.clear();
            failedCover_.clear(); coverSyncError_.clear();
            metadataRefreshPending_ = true;
            status_ = QStringLiteral("封面已提交，正在读取平台审核状态…");
            emit metadataSaved();
            notifyMetadata();
            pumpMetadata();
        });
        return;
    }
    if (pendingStartArea_) {
        if (!titleError_.isEmpty() || !coverSyncError_.isEmpty() || titleDraft_.trimmed().isEmpty()) {
            pendingStartArea_.reset();
            setState(State::Ready, QStringLiteral("标题或封面尚未同步，已取消本次开播。请修正后重试。"));
            return;
        }
        if (titleDirty() || titleDebounce_.isActive()) return;
        beginQueuedStart();
        return;
    }
    if (metadataRefreshPending_) {
        metadataRefreshPending_ = false;
        metadataRefreshing_ = true;
        loadRoom();
    }
}

void LiveController::saveMetadata(const QString &title, const QString &coverFile)
{
    metadataBatching_ = true;
    setTitleDraft(title);
    flushTitle();
    if (!coverFile.isEmpty()) saveCover(coverFile);
    metadataBatching_ = false;
    pumpMetadata();
}

void LiveController::start(const QString &title, const QString &coverFile, int areaId)
{
    if (!authenticated_ || remoteRoomId_ || bridge_->busy() ||
        (state_ != State::Ready && !(state_ == State::Preparing && pendingStartArea_))) return;
    if (areaId <= 0 || title.trimmed().isEmpty()) { status_ = QStringLiteral("请填写标题和有效的直播分区。"); emit changed(); return; }
    metadataBatching_ = true;
    pendingStartArea_ = areaId;
    setTitleDraft(title);
    if (!coverFile.isEmpty() && (coverFile != lastCoverRequested_ || !failedCover_.isEmpty())) saveCover(coverFile);
    else if (!failedCover_.isEmpty()) {
        const QString retryPath = failedCover_;
        saveCover(retryPath); // An explicit new click permits one failed-crop retry.
    }
    flushTitle();
    setState(State::Preparing, QStringLiteral("正在同步最新标题和封面，完成后开播…"));
    metadataBatching_ = false;
    pumpMetadata();
}

void LiveController::beginQueuedStart()
{
    if (!pendingStartArea_ || working_) return;
    ++operation_;
    metadataRefreshPending_ = false;
    loadRoom([this] {
        if (!pendingStartArea_) return;
        if (metadataKind_ != MetadataKind::None || titleDirty() || titleDebounce_.isActive() || !coverQueue_.isEmpty()) {
            pumpMetadata();
            return;
        }
        const int areaId = *pendingStartArea_;
        if (!room_.canLive) { pendingStartArea_.reset(); setState(State::Ready, room_.blockMessage); return; }
        if (room_.liveStatus == 1) {
            pendingStartArea_.reset();
            remoteRoomId_ = room_.roomId;
            setState(State::Uncertain, QStringLiteral("房间已在直播。请先下播再重新开播。")); return;
        }
        if (room_.liveStatus != 0 && room_.liveStatus != 2) {
            pendingStartArea_.reset(); setState(State::Uncertain, QStringLiteral("房间状态未知，无法开播，请刷新。")); return;
        }
        pendingStartArea_.reset();
        working_ = true;
        setState(State::Preparing, QStringLiteral("正在向 B 站申请开播…"));
        const auto operation = operation_;
        client_->startLive(room_.roomId, areaId, [this, operation, areaId](Result<LiveSession> result) {
            if (operation != operation_) return;
            working_ = false;
            if (auto *error = std::get_if<ApiError>(&result)) {
                if (error->outcomeUnknown || error->kind == ApiError::Kind::UnsupportedProtocol || error->kind == ApiError::Kind::InvalidResponse)
                    remoteRoomId_ = room_.roomId;
                fail(*error, remoteRoomId_ ? State::Uncertain : State::Ready); return;
            }
            session_ = std::get<LiveSession>(result);
            remoteRoomId_ = session_->roomId;
            room_.areaId = areaId;
            room_.liveStatus = 1;
            for (const auto &area : areas_) if (area.id == areaId) {
                room_.areaName = area.name; room_.parentAreaName = area.parentName; break;
            }
            emit roomChanged();
            beginOutput();
            pumpMetadata();
        });
    });
}

void LiveController::beginOutput()
{
    if (!session_) return;
    QString error;
    setState(State::Connecting, QStringLiteral("B 站已开播，正在连接 OBS 输出…"));
    if (!bridge_->start(*session_, &error)) setState(State::Recoverable, error + QStringLiteral(" 本场已保留，可重试或下播。"));
}

void LiveController::retryConnection()
{
    if (!authenticated_ || working_ || !session_ || bridge_->busy() || state_ != State::Recoverable) return;
    ++operation_;
    beginOutput();
}

void LiveController::stop()
{
    if (pendingStartArea_ && !remoteRoomId_) {
        ++operation_;
        working_ = false;
        cancelMetadata(false);
        setState(State::Ready, QStringLiteral("已取消本次开播。"));
        return;
    }
    if ((working_ && !metadataRefreshing_) || !remoteRoomId_ || stoppingRequest_) return;
    ++operation_;
    working_ = false;
    cancelMetadata(false);
    setState(State::Stopping, QStringLiteral("正在停止 OBS 输出…"));
    if (bridge_->ownsService()) {
        const bool wasBusy = bridge_->busy();
        // Latch intentional stop even after a failed/disconnected output, so
        // OBS's native Start button cannot race the pending server-side stop.
        bridge_->stop();
        if (!wasBusy) stopRemote();
    } else {
        stopRemote();
    }
}

void LiveController::stopRemote()
{
    if (!remoteRoomId_ || stoppingRequest_) return;
    const auto operation = ++operation_;
    cancelMetadata(false);
    if (!authenticated_) { working_ = false; setState(State::Uncertain, QStringLiteral("OBS 已停止。登录已失效，请登录原账号后重试下播。")); return; }
    stoppingRequest_ = true;
    working_ = true;
    setState(State::Stopping, QStringLiteral("OBS 已停止，正在通知 B 站下播…"));
    client_->stopLive(remoteRoomId_, [this, operation](Result<Done> result) {
        if (operation != operation_) return;
        stoppingRequest_ = false;
        working_ = false;
        if (auto *error = std::get_if<ApiError>(&result)) { fail(*error, State::Uncertain); return; }
        remoteRoomId_ = 0;
        session_.reset();
        room_.liveStatus = 0;
        bridge_->restoreService();
        setState(State::Ready, QStringLiteral("已下播，OBS 原推流服务已恢复。"));
        emit roomChanged();
    });
}
}
