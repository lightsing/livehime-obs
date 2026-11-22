#include "room-realtime.hpp"
#include "goim-client.hpp"
#include "room-interaction.hpp"
#include <QDateTime>
#include <QRandomGenerator>
#include <QSet>
#include <utility>

namespace livehime {
namespace {
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
QString failure(const ApiError &error) {
    return error.message + (error.code ? QStringLiteral("（代码 %1）").arg(error.code) : QString{});
}
} // namespace
RoomRealtime::RoomRealtime(LiveController *controller, QObject *parent)
    : QObject(parent), controller_(controller), goim_(new GoimClient(this)) {
    connect(controller_, &LiveController::changed, this, &RoomRealtime::syncContext);
    connect(controller_, &QObject::destroyed, this, [this] { stop(); });
    connect(controller_, &LiveController::roomChanged, this, [this] {
        syncContext();
        const auto config = rankInfo();
        if (config != rankConfig_) {
            rankConfig_ = config;
            emit rankConfigChanged();
        }
    });
    connect(goim_, &GoimClient::connectionProgress, this,
            [this](GoimClient::Progress phase, const QString &detail, QDeadlineTimer retryAt) {
                if (stopped_ || !uid_ || !roomId_)
                    return;
                switch (phase) {
                case GoimClient::Progress::Connecting: connecting(detail); break;
                case GoimClient::Progress::Connected: setConnection(RealtimeConnection::Phase::Connected, detail); break;
                case GoimClient::Progress::Waiting: retryConnection(detail, retryAt); break;
                }
            });
    connect(goim_, &GoimClient::tokenRefreshRequested, this, [this] {
        if (stopped_ || !uid_ || !roomId_)
            return;
        retryConnection(QStringLiteral("弹幕令牌已过期，正在更新"));
        fetchConfig();
    });
    connect(goim_, &GoimClient::endpointFailed, this, [this] {
        if (stopped_ || !uid_ || !roomId_)
            return;
        ++endpointIndex_;
        if (endpointIndex_ < endpoints_.size())
            retryConnection(QStringLiteral("弹幕节点连接失败，正在切换节点"));
        connectEndpoint();
    });
    connect(goim_, &GoimClient::events, this, [this](const QJsonArray &batch) {
        if (!uid_ || !roomId_ || stopped_)
            return;
        bool rank = false, guard = false, entriesChanged = false;
        QJsonArray displayEvents;
        for (const auto &v : batch) {
            const auto event = v.toObject();
            const auto cmd = event.value("cmd").toString().section(':', 0, 0);
            if (cmd == "INTERACT_WORD" || cmd == "INTERACT_WORD_V2") {
                const auto data = roomInteraction(event, roomId_);
                if (!data)
                    continue;
                if (data->value("msg_type").toInteger() == 1) {
                    const auto id = event.value("msg_id");
                    QString messageId = id.isString() ? id.toString() : QString{};
                    if (id.isDouble() && id.toInteger() > 0)
                        messageId = QString::number(id.toInteger());
                    if (messageId.size() > 128)
                        messageId.clear();
                    if (messageId.isEmpty())
                        messageId = data->value("tid").toString(data->value("id_str").toString());
                    if (!messageId.isEmpty() && entryIds_.contains(messageId))
                        continue;
                    if (entryHistory_.size() >= 500)
                        entryIds_.remove(entryHistory_.takeFirst().messageId);
                    if (!messageId.isEmpty())
                        entryIds_.insert(messageId);
                    const auto timestamp = data->value("timestamp").toInteger();
                    const auto sequence = entryHistory_.isEmpty() ? 1 : entryHistory_.constLast().sequence + 1;
                    entryHistory_.append({data->value("uname").toString(), timestamp > 0 ? timestamp : now(),
                                          messageId, sequence});
                    auto &entry = entryHistory_.last();
                    entry.uid = data->value("uid").toInteger();
                    entry.glory = data->value("wealth_level").toInteger();
                    entriesChanged = true;
                }
                displayEvents.append(QJsonObject{{"cmd", "INTERACT_WORD"}, {"data", *data}});
                continue;
            }
            displayEvents.append(v);
            const auto notice = parseModerationNotice(event, roomId_);
            if (notice && (notice->kind != ModerationNotice::Kind::Ecology || controller_->room().liveStatus == 1))
                emit moderationNotice(*notice);
            rank |= cmd == "ONLINE_RANK_V2" || cmd == "ONLINE_RANK_V3";
            guard |= cmd == "GUARD_BUY" || cmd == "USER_TOAST_MSG" || cmd == "USER_TOAST_MSG_V2";
        }
        // Broadcasts invalidate HTTP snapshots; never use them as a complete list.
        if (rank && audienceType_ == "online_rank")
            audienceDirty_ = true;
        if (guard)
            guardDirty_ = true;
        if (entriesChanged)
            emit entryHistoryChanged();
        if (!displayEvents.isEmpty())
            emit events(displayEvents);
    });
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        const auto time = QDateTime::currentMSecsSinceEpoch();
        if (visible_[1] &&
            (time >= audienceNextAt_ || (audienceDirty_ && time - audienceRequestedAt_ >= 10000)))
            refreshAudience();
        if (visible_[2] && (time >= guardNextAt_ || (guardDirty_ && time - guardRequestedAt_ >= 10000))) {
            if (guardPage_ <= 1)
                refreshGuard();
            else {
                // Preserve a coherent multi-page snapshot while it is being read.
                guardDirty_ = false;
                guardNextAt_ = time + 15000;
                const auto message = QStringLiteral("已加载多页，自动刷新已暂停；点击刷新重新载入");
                if (guardStatus_ != message) {
                    guardStatus_ = message;
                    emit guardChanged();
                }
            }
        }
    });
    refreshTimer_.start();
    retryTimer_.setSingleShot(true);
    retryTimer_.setInterval(10000);
    connect(&retryTimer_, &QTimer::timeout, this, &RoomRealtime::fetchConfig);
    syncContext();
}
RoomRealtime::~RoomRealtime() { stop(); }
void RoomRealtime::setConnection(RealtimeConnection::Phase phase, QString detail, int attempt, QDeadlineTimer retryAt) {
    connection_ = {phase, attempt, retryAt, std::move(detail)};
    emit connectionChanged(connection_);
    emit statusChanged(connection_.detail);
}
void RoomRealtime::connecting(const QString &detail) {
    setConnection(connection_.attempt ? RealtimeConnection::Phase::Reconnecting : RealtimeConnection::Phase::Connecting,
                  detail, connection_.attempt);
}
void RoomRealtime::retryConnection(const QString &detail, QDeadlineTimer retryAt) {
    setConnection(RealtimeConnection::Phase::Reconnecting, detail, connection_.attempt + 1, retryAt);
}
void RoomRealtime::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    ++contextEpoch_;
    ++audienceEpoch_;
    ++guardEpoch_;
    refreshTimer_.stop();
    retryTimer_.stop();
    sendingDanmaku_ = false;
    moderating_ = false;
    if (controller_) {
        controller_->client()->cancelRealtime(moderationReply_);
        controller_->client()->cancelRealtime(sendReply_);
        controller_->client()->cancelRealtime(configReply_);
        controller_->client()->cancelRealtime(audienceReply_);
        controller_->client()->cancelRealtime(guardReply_);
    }
    goim_->stop();
    entryHistory_.clear();
    entryIds_.clear();
    setConnection(RealtimeConnection::Phase::Idle, QStringLiteral("弹幕已停止"));
    emit stopped();
}
void RoomRealtime::syncContext() {
    if (stopped_ || !controller_)
        return;
    const auto epoch = controller_->accountEpoch();
    const auto uid = controller_->authenticated() ? controller_->user().uid : 0;
    const auto room = uid && controller_->roomMatchesAccount() ? controller_->room().roomId : 0;
    if (epoch == accountEpoch_ && uid == uid_ && room == roomId_)
        return;
    ++contextEpoch_;
    ++audienceEpoch_;
    ++guardEpoch_;
    accountEpoch_ = epoch;
    uid_ = uid;
    roomId_ = room;
    entryHistory_.clear();
    entryIds_.clear();
    sendingDanmaku_ = false;
    moderating_ = false;
    controller_->client()->cancelRealtime(moderationReply_);
    moderationReply_.clear();
    controller_->client()->cancelRealtime(sendReply_);
    sendReply_.clear();
    controller_->client()->cancelRealtime(configReply_);
    controller_->client()->cancelRealtime(audienceReply_);
    controller_->client()->cancelRealtime(guardReply_);
    configReply_.clear();
    audienceReply_.clear();
    guardReply_.clear();
    retryTimer_.stop();
    goim_->stop();
    connection_ = {};
    audience_ = {};
    endpoints_ = {};
    connectionToken_.clear();
    endpointIndex_ = 0;
    guard_ = {};
    guardPage_ = 0;
    guardHasMore_ = true;
    rankConfig_ = rankInfo();
    audienceDirty_ = guardDirty_ = false;
    audienceNextAt_ = guardNextAt_ = 0;
    audienceStatus_.clear();
    guardStatus_.clear();
    emit contextChanged();
    emit audienceChanged();
    emit guardChanged();
    if (!uid_ || !roomId_) {
        setConnection(RealtimeConnection::Phase::Idle, QStringLiteral("请先登录并加载房间"));
        return;
    }
    fetchConfig();
    if (visible_[1])
        refreshAudience();
    if (visible_[2])
        refreshGuard();
}
void RoomRealtime::fetchConfig() {
    if (stopped_ || !uid_ || !roomId_ || configReply_)
        return;
    const auto epoch = contextEpoch_;
    connecting(QStringLiteral("正在获取弹幕连接配置…"));
    configReply_ = controller_->client()->fetchRealtime(
        protocol::danmuInfo(roomId_, now()), [this, epoch](Result<QJsonObject> result) {
            if (epoch != contextEpoch_ || stopped_)
                return;
            configReply_.clear();
            if (const auto *error = std::get_if<ApiError>(&result)) {
                retryTimer_.start();
                retryConnection(failure(*error), QDeadlineTimer(retryTimer_.remainingTime(), Qt::PreciseTimer));
                return;
            }
            const auto data = std::get<QJsonObject>(result);
            const auto hosts = data.value("ip_list").toArray();
            const auto token = data.value("token").toString();
            endpoints_ = {};
            connectionToken_ = token;
            endpointIndex_ = 0;
            for (const auto &value : hosts) {
                const auto host = value.toObject();
                const auto name = host.value("host").toString();
                const auto port = host.value("port").toInt();
                if (name.isEmpty() || port < 1 || port > 65535 || token.isEmpty())
                    continue;
                if (endpoints_.size() < 16)
                    endpoints_.append(host);
            }
            connectEndpoint();
        });
}
void RoomRealtime::connectEndpoint() {
    if (stopped_ || !controller_ || !uid_ || !roomId_)
        return;
    if (endpointIndex_ >= endpoints_.size() || connectionToken_.isEmpty()) {
        retryTimer_.start();
        retryConnection(QStringLiteral("弹幕地址暂不可用，稍后重新获取连接配置"),
                        QDeadlineTimer(retryTimer_.remainingTime(), Qt::PreciseTimer));
        return;
    }
    const auto host = endpoints_.at(endpointIndex_).toObject();
    goim_->setContext(uid_, roomId_, connectionToken_, host.value("host").toString(),
                      quint16(host.value("port").toInt()), controller_->deviceId());
}
void RoomRealtime::sendDanmaku(const QString &text) {
    if (sendingDanmaku_) return;
    if (!canSendDanmaku() || !controller_) {
        emit danmakuSendFinished(false, QStringLiteral("请先登录并加载直播间。"));
        return;
    }
    if (!protocol::validDanmakuText(text)) {
        emit danmakuSendFinished(false, QStringLiteral("请输入非空的单行弹幕。"));
        return;
    }
    sendingDanmaku_ = true;
    const auto epoch = contextEpoch_;
    const QPointer<RoomRealtime> self(this);
    auto *reply = controller_->client()->sendDanmaku(roomId_, text, [self, epoch](Result<Done> result) {
        if (!self || self->contextEpoch_ != epoch || self->stopped_) return;
        self->sendingDanmaku_ = false;
        self->sendReply_.clear();
        if (const auto *error = std::get_if<ApiError>(&result)) {
            const auto message = error->outcomeUnknown
                ? QStringLiteral("发送结果未确认，请先查看弹幕记录。") : failure(*error);
            emit self->danmakuSendFinished(false, message);
        } else {
            emit self->danmakuSendFinished(true, QStringLiteral("已发送"));
        }
    });
    if (reply && self && self->contextEpoch_ == epoch && self->sendingDanmaku_)
        self->sendReply_ = reply;
}
void RoomRealtime::moderateUser(UserModeration action, qint64 targetUid, quint64 expectedEpoch, const QString &message,
                                int durationHours) {
    if (moderating_) return;
    if (!controller_ || expectedEpoch != contextEpoch_ || !canModerate(targetUid)) {
        emit moderationFinished(false, QStringLiteral("请选择当前直播间中身份明确的其他用户。"));
        return;
    }
    if (action == UserModeration::Mute && !protocol::validMuteDuration(durationHours)) {
        emit moderationFinished(false, QStringLiteral("请选择有效的禁言时长。"));
        return;
    }
    moderating_ = true;
    const QPointer<RoomRealtime> self(this);
    auto *reply = controller_->client()->moderateUser(action, uid_, roomId_, targetUid, message, durationHours,
        [self, expectedEpoch](Result<Done> result) {
            if (!self || self->stopped_ || self->contextEpoch_ != expectedEpoch) return;
            self->moderating_ = false;
            self->moderationReply_.clear();
            if (const auto *error = std::get_if<ApiError>(&result)) {
                emit self->moderationFinished(false, error->outcomeUnknown
                    ? QStringLiteral("操作结果未确认，请先在直播间管理中核实。") : failure(*error));
            } else {
                emit self->moderationFinished(true, QStringLiteral("操作成功"));
            }
        });
    if (reply && self && self->contextEpoch_ == expectedEpoch && self->moderating_)
        self->moderationReply_ = reply;
}
void RoomRealtime::setVisible(int kind, bool visible) {
    if (kind < 0 || kind > 2)
        return;
    const bool was = visible_[kind];
    visible_[kind] = visible;
    if (visible && !was) {
        if (kind == 1)
            refreshAudience();
        if (kind == 2)
            refreshGuard();
    }
}
void RoomRealtime::selectAudience(const QString &type, const QString &sort) {
    if (audienceType_ == type && audienceSort_ == sort)
        return;
    ++audienceEpoch_;
    controller_->client()->cancelRealtime(audienceReply_);
    audienceReply_.clear();
    audienceType_ = type;
    audienceSort_ = sort;
    audience_ = {};
    audienceStatus_.clear();
    emit audienceChanged();
    refreshAudience();
}
void RoomRealtime::refreshAudience() {
    if (stopped_ || !uid_ || !roomId_)
        return;
    if (audienceReply_) {
        audienceDirty_ = true;
        return;
    }
    audienceDirty_ = false;
    audienceRequestedAt_ = QDateTime::currentMSecsSinceEpoch();
    audienceNextAt_ = audienceRequestedAt_ + 10000 + QRandomGenerator::global()->bounded(10001);
    const auto epoch = contextEpoch_, request = ++audienceEpoch_;
    audienceStatus_ = QStringLiteral("加载中…");
    emit audienceChanged();
    audienceReply_ = controller_->client()->fetchRealtime(
        protocol::contributionRank(uid_, roomId_, audienceType_, audienceSort_, now()),
        [this, epoch, request](Result<QJsonObject> result) {
            if (epoch != contextEpoch_ || request != audienceEpoch_ || stopped_)
                return;
            audienceReply_.clear();
            if (const auto *error = std::get_if<ApiError>(&result))
                audienceStatus_ = failure(*error);
            else {
                const auto data = std::get<QJsonObject>(result);
                if (!data.value("item").isArray() && !data.value("item").isNull())
                    audienceStatus_ = QStringLiteral("榜单响应结构无效");
                else {
                    audience_ = data;
                    audienceStatus_ = QStringLiteral("%1 · %2 条")
                                          .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                          .arg(data.value("item").toArray().size());
                }
            }
            emit audienceChanged();
        });
}
void RoomRealtime::selectGuard(int typ) {
    if (typ != 1 && typ != 3 && typ != 4 && typ != 5)
        return;
    if (guardTyp_ == typ)
        return;
    ++guardEpoch_;
    controller_->client()->cancelRealtime(guardReply_);
    guardReply_.clear();
    guardTyp_ = typ;
    guardPage_ = 0;
    guard_ = {};
    guardHasMore_ = true;
    emit guardChanged();
    requestGuard(1);
}
void RoomRealtime::refreshGuard() {
    if (!guardReply_) {
        guardPage_ = 0;
    }
    requestGuard(1);
}
void RoomRealtime::loadMoreGuard() {
    if (guardHasMore_)
        requestGuard(guardPage_ + 1);
}
void RoomRealtime::requestGuard(int page) {
    if (stopped_ || !uid_ || !roomId_)
        return;
    if (guardReply_) {
        guardDirty_ = true;
        return;
    }
    guardDirty_ = false;
    guardRequestedAt_ = QDateTime::currentMSecsSinceEpoch();
    guardNextAt_ = guardRequestedAt_ + 10000 + QRandomGenerator::global()->bounded(10001);
    const auto epoch = contextEpoch_, request = ++guardEpoch_;
    guardStatus_ = QStringLiteral("加载中…");
    emit guardChanged();
    guardReply_ = controller_->client()->fetchRealtime(
        protocol::guardRank(uid_, roomId_, guardTyp_, page, now()),
        [this, epoch, request, page](Result<QJsonObject> result) {
            if (epoch != contextEpoch_ || request != guardEpoch_ || stopped_)
                return;
            guardReply_.clear();
            if (const auto *error = std::get_if<ApiError>(&result))
                guardStatus_ = failure(*error);
            else {
                const auto data = std::get<QJsonObject>(result);
                const auto info = data.value("info").toObject();
                if ((!data.value("list").isArray() && !data.value("list").isNull()) ||
                    info.value("now").toInt() != page)
                    guardStatus_ = QStringLiteral("大航海分页响应结构无效");
                else {
                    QJsonArray rows = page == 1 ? QJsonArray{} : guard_.value("rows").toArray();
                    if (info.value("now").toInt() == 1)
                        for (const auto &row : data.value("top3").toArray())
                            rows.append(row);
                    const auto list = data.value("list").toArray();
                    for (const auto &row : list)
                        if (rows.size() < 3000)
                            rows.append(row);
                    guard_ = data;
                    guard_["rows"] = rows;
                    guardPage_ = page;
                    guardHasMore_ =
                        !list.isEmpty() && rows.size() < info.value("num").toInt() && rows.size() < 3000;
                    guardStatus_ = QStringLiteral("%1 · 已加载 %2 / %3")
                                       .arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                                       .arg(rows.size())
                                       .arg(info.value("num").toInt());
                }
            }
            emit guardChanged();
        });
}
} // namespace livehime
