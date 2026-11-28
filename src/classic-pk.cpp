#include "classic-pk.hpp"
#include <QPointer>
#include <algorithm>

namespace livehime {
namespace {
qint64 number(const QJsonValue &value, qint64 fallback = -1)
{
    if (!value.isString()) return value.toInteger(fallback);
    bool ok = false;
    const auto result = value.toString().toLongLong(&ok);
    return ok ? result : fallback;
}

// Classic broadcasts and snapshots can use different timestamp precisions.
qint64 unixMillis(qint64 time)
{
    if (time >= 1000000000000000LL) time /= 1000;
    if (time >= 1000000000000LL && time < 10000000000000LL) return time;
    if (time >= 1000000000 && time < 10000000000LL) return time * 1000;
    return 0;
}

void updateSide(PkSide &side, const QJsonObject &data)
{
    const auto name = data.value("uname").toString();
    if (!name.isEmpty()) side.name = name.left(80);
    const auto votes = number(data.value("votes"));
    if (votes >= 0) side.votes = votes;
}
} // namespace

ClassicPkController::ClassicPkController(Send send, QObject *parent)
    : QObject(parent), send_(std::move(send))
{
    pollTimer_.setSingleShot(true);
    connect(&pollTimer_, &QTimer::timeout, this, &ClassicPkController::poll);
}

void ClassicPkController::clearScores()
{
    home_ = {};
    away_ = {};
    serverLabel_.clear();
    resultText_.clear();
    deadline_.reset();
}

void ClassicPkController::setContext(quint64 accountEpoch, qint64 uid, qint64 liveRoom, bool allowed)
{
    if (accountEpoch == accountEpoch_ && uid == uid_ && liveRoom == room_) {
        if (allowed_ != allowed) {
            allowed_ = allowed;
            prompt_.clear();
            emit changed();
        }
        return;
    }
    ++epoch_;
    accountEpoch_ = accountEpoch;
    uid_ = uid;
    room_ = liveRoom;
    allowed_ = allowed;
    pollTimer_.stop();
    phase_ = Phase::Idle;
    pkId_ = hintedId_ = timestamp_ = 0;
    endedIds_.clear();
    pending_.reset();
    prompt_.clear();
    message_.clear();
    queryError_.clear();
    switchHint_.clear();
    clearScores();
    reading_ = roomLookup_ = uncertain_ = switchReading_ = switchKnown_ = switchDisabled_ = continuous_ = false;
    emit changed();
    refresh();
}

void ClassicPkController::request(ClassicPkOp op, Callback<QJsonObject> callback, bool enabled, qint64 lookupId)
{
    const QPointer<ClassicPkController> self(this);
    const auto id = lookupId >= 0 ? lookupId : pkId_;
    send_(op, uid_, room_, id, enabled,
          [self, epoch = epoch_, op, callback = std::move(callback)](Result<QJsonObject> result) {
              if (!self || self->epoch_ != epoch) return;
              if (const auto *error = std::get_if<ApiError>(&result)) {
                  auto issue = PluginIssue::api(QStringLiteral("互动窗口"), *error);
                  issue.codes.insert("operation_id", static_cast<int>(op));
                  emit self->problem(issue);
              }
              if (self && self->epoch_ == epoch) callback(std::move(result));
          });
}

void ClassicPkController::refresh()
{
    if (!online()) return;
    roomLookup_ = true;
    if (!switchReading_ && pending_ != ClassicPkOp::SetContinuous) {
        switchReading_ = true;
        const auto revision = switchRevision_;
        request(ClassicPkOp::ReadContinuous, [this, revision](Result<QJsonObject> result) {
            switchReading_ = false;
            if (revision != switchRevision_) {
                if (pending_ != ClassicPkOp::SetContinuous) refresh();
                return;
            }
            if (const auto *data = std::get_if<QJsonObject>(&result)) {
                const auto status = number(data->value("status"));
                switchKnown_ = status >= 0 && (status == 0 || data->value("pk_version").isArray());
                continuous_ = false;
                for (const auto &version : data->value("pk_version").toArray())
                    if (number(version) == 1) continuous_ = status == 1 || status == 2;
                const auto disable = data->value("button_disable");
                switchDisabled_ = disable.toBool() || number(disable, 0) != 0;
                switchHint_ = data->value("toast").toString().left(300);
            } else {
                switchKnown_ = false;
                switchHint_ = std::get<ApiError>(result).message;
            }
            emit changed();
        });
    }
    schedule(0);
}

void ClassicPkController::schedule(int delay)
{
    if (online() && (!pollTimer_.isActive() || pollTimer_.remainingTime() > delay))
        pollTimer_.start(delay);
}

void ClassicPkController::poll()
{
    if (!online() || reading_) return;
    reading_ = true;
    const auto revision = readRevision_;
    const auto id = roomLookup_ ? 0 : (pkId_ ? pkId_ : hintedId_);
    roomLookup_ = false;
    request(ClassicPkOp::Info, [this, revision, id](Result<QJsonObject> result) {
        reading_ = false;
        if (revision != readRevision_) { schedule(0); return; }
        int delay = 1000;
        if (const auto *error = std::get_if<ApiError>(&result)) {
            queryError_ = error->message;
            delay = 5000;
        } else {
            queryError_.clear();
            const auto &data = std::get<QJsonObject>(result);
            if (apply(data, !id)) uncertain_ = false;
            else if (!data.contains("status")) queryError_ = QStringLiteral("对局状态暂不可用。");
        }
        if (roomLookup_) schedule(0);
        else if (uncertain_ || (phase_ != Phase::Idle && phase_ != Phase::Finished))
            schedule(delay);
        emit changed();
    }, false, id);
}

bool ClassicPkController::apply(const QJsonObject &data, bool roomLookup)
{
    const auto status = number(data.value("status"));
    if (status < 0) return false;
    const auto id = number(data.value("pk_id"), 0);
    if (id < 0) return false;
    const auto rawStamp = number(data.value("timestamp"), 0);
    const auto serverTime = unixMillis(rawStamp);
    const auto stamp = serverTime ? serverTime : rawStamp;
    if (id > 0 && endedIds_.contains(id) && status != 0) return false;
    const bool switchedRound = pkId_ && id && pkId_ != id;
    if (switchedRound && !roomLookup) return false;
    if (!switchedRound && stamp && timestamp_ && stamp < timestamp_) return false;

    const auto match = number(data.value("match_status"));
    if (status == 0 && match < 0) return false;
    const auto first = data.value("init_info").toObject();
    const auto second = data.value("match_info").toObject();
    const auto firstRoom = number(first.value("room_id"), 0);
    const auto secondRoom = number(second.value("room_id"), 0);
    if (firstRoom && secondRoom && firstRoom != room_ && secondRoom != room_) return false;
    Phase next;
    if (status == 0 && match == 0) next = phase_ == Phase::Finished ? Phase::Finished : Phase::Idle;
    else if (number(data.value("battle_type"), 1) > 1) next = Phase::Other;
    else if (status == 0 && match == 1) next = Phase::Matching;
    else if (status == 100 || status == 101) next = Phase::Preparing;
    else if (status == 201) next = Phase::Playing;
    else if (status == 301) next = Phase::Sprint;
    else if (status == 304 || status == 401 || status == 404 || status == 501 || status == 905) next = Phase::Settling;
    else if (status == 601 || status == 602 || status == 610) next = Phase::Punishing;
    else if (status > 1000) next = Phase::Finished;
    else return false;

    const auto previous = phase_;
    const bool sameTick = stamp && timestamp_ == stamp && !switchedRound && next == previous;
    if (switchedRound) {
        finishRound(pkId_);
        ++phaseRevision_;
    }
    if (switchedRound || (next == Phase::Matching && previous != Phase::Matching) || next == Phase::Idle ||
        (id && !pkId_ && next != Phase::Finished)) {
        clearScores();
        timestamp_ = 0;
    }
    if (next != Phase::Idle && next != Phase::Finished) prompt_.clear();
    if (next != previous) {
        ++phaseRevision_;
        serverLabel_.clear();
    }
    if (firstRoom == room_) { updateSide(home_, first); updateSide(away_, second); }
    else if (secondRoom == room_) { updateSide(home_, second); updateSide(away_, first); }
    const auto label = data.value("status_msg").toString();
    if (!label.isEmpty() && status > 0) serverLabel_ = label.left(120);
    if (id) pkId_ = id;
    if (stamp) timestamp_ = stamp;
    if (next == Phase::Finished || next == Phase::Idle || next == Phase::Matching) {
        if (next == Phase::Finished && previous != Phase::Finished) schedule(1000);
        if (next != Phase::Matching || previous != Phase::Matching) {
            if (pkId_ && (next != Phase::Matching || pkId_ != id)) finishRound(pkId_);
            pkId_ = next == Phase::Matching ? id : 0;
        }
        hintedId_ = 0;
        if (!pkId_) timestamp_ = 0;
    }

    qint64 end = 0;
    if (next == Phase::Matching) end = number(data.value("match_end_time"));
    else if (next == Phase::Preparing) end = number(data.value("pk_start_time"));
    else if (next == Phase::Playing || next == Phase::Sprint) end = number(data.value("pk_frozen_time"));
    else if (next == Phase::Punishing) end = number(data.value("pk_end_time"));
    if (previous != next) deadline_.reset();
    if (const auto endMs = unixMillis(end); endMs && (!sameTick || !deadline_)) {
        const auto remaining = endMs - (serverTime ? serverTime : QDateTime::currentMSecsSinceEpoch());
        if (remaining >= -60000 && remaining <= 86400000)
            deadline_.emplace(std::max<qint64>(0, remaining));
    }
    phase_ = next;
    if (!pending_ && (previous != next || uncertain_)) message_.clear();
    return true;
}

QString ClassicPkController::message() const
{
    QStringList lines{resultText_, message_, queryError_};
    lines.removeAll(QString{});
    return lines.join(QChar('\n'));
}

void ClassicPkController::finishRound(qint64 id)
{
    if (id <= 0 || endedIds_.contains(id)) return;
    endedIds_.append(id);
    if (endedIds_.size() > 16) endedIds_.removeFirst();
}

bool ClassicPkController::applyUnified(const QJsonObject &data)
{
    const auto basic = data.value("pk_basic").toObject();
    const auto id = number(basic.value("pk_id"));
    if (number(basic.value("type")) != 1 || id <= 0) return false;
    const auto members = data.value("members").toArray();
    if (members.size() > 2) return false;
    const auto first = members.isEmpty() ? QJsonObject{} : members.first().toObject();
    const auto second = members.size() == 2 ? members.last().toObject() : QJsonObject{};
    if (id != pkId_ && number(first.value("room_id")) != room_ && number(second.value("room_id")) != room_)
        return false;
    const auto match = data.value("pk_match_info").toObject();
    return apply({{"pk_id", id}, {"status", basic.value("status")}, {"battle_type", 1},
                  {"timestamp", data.value("timestamp")}, {"status_msg", basic.value("status_msg")},
                  {"match_status", match.value("match_status")}, {"match_end_time", match.value("match_end_time")},
                  {"pk_start_time", basic.value("start_time")}, {"pk_frozen_time", basic.value("end_time")},
                  {"pk_end_time", basic.value("punish_end_time")}, {"init_info", first}, {"match_info", second}});
}

void ClassicPkController::receive(const QJsonArray &events)
{
    if (!online()) return;
    for (const auto &value : events) {
        const auto event = value.toObject();
        const auto cmd = event.value("cmd").toString().section(':', 0, 0);
        if (cmd != "PK_INFO" && !cmd.startsWith("PK_BATTLE_")) continue;
        const auto data = event.value("data").toObject();
        const auto basic = data.value("pk_basic").toObject();
        const auto id = number(basic.value("pk_id"), number(data.value("pk_id"), number(event.value("pk_id"), 0)));
        if (number(basic.value("type"), number(data.value("battle_type"), 1)) != 1) continue;
        if (cmd.startsWith("PK_BATTLE_SETTLE") && id > 0 &&
            (id == pkId_ || (phase_ == Phase::Finished && !endedIds_.isEmpty() && id == endedIds_.last()))) {
            const auto text = data.value("star_light_msg").toString().trimmed().left(300);
            if (!text.isEmpty()) { resultText_ = text; emit changed(); }
        }
        if (id && endedIds_.contains(id)) continue;
        if (id && pkId_ && pkId_ != id) {
            roomLookup_ = true;
            schedule(1000);
            continue;
        }
        if (cmd == "PK_INFO" && applyUnified(data)) {
            ++readRevision_;
            uncertain_ = false;
            queryError_.clear();
            schedule(1000);
            emit changed();
            continue;
        }
        if (cmd == "PK_BATTLE_MATCH_TIMEOUT" && phase_ == Phase::Matching &&
            number(data.value("battle_type")) == 1) {
            apply({{"status", 0}, {"match_status", 0}});
            ++readRevision_;
            message_ = QStringLiteral("本次匹配已超时。");
            emit changed();
        }
        if (id && !pkId_ && hintedId_ != id) {
            hintedId_ = id;
            ++readRevision_;
        }
        schedule(pkId_ ? 1000 : 0);
    }
}

bool ClassicPkController::canExit() const
{
    return actionable() && pkId_ > 0 && (phase_ == Phase::Preparing || phase_ == Phase::Playing ||
        phase_ == Phase::Sprint || phase_ == Phase::Punishing || phase_ == Phase::Settling);
}

void ClassicPkController::confirmPrecheck(bool accept)
{
    if (prompt_.isEmpty()) return;
    prompt_.clear();
    if (accept && canStart()) join();
    else emit changed();
}

void ClassicPkController::join()
{
    if (canStart()) command(ClassicPkOp::Join);
}

void ClassicPkController::start()
{
    if (!canStart()) return;
    pending_ = ClassicPkOp::Precheck;
    message_.clear();
    emit changed();
    request(ClassicPkOp::Precheck, [this, revision = phaseRevision_](Result<QJsonObject> result) {
        pending_.reset();
        if (revision != phaseRevision_ || !canStart()) { emit changed(); return; }
        if (const auto *data = std::get_if<QJsonObject>(&result)) {
            if (number(data->value("show_type")) == 1) {
                const auto tips = data->value("tips").toObject();
                prompt_ = QStringList{tips.value("title").toString(), tips.value("content").toString()}
                              .join('\n').trimmed().left(1000);
                if (prompt_.isEmpty()) prompt_ = QStringLiteral("确认开启本次匹配？");
                confirmText_ = tips.value("confirm_content").toString().left(50);
                emit changed();
                return;
            }
        } else {
            const auto &error = std::get<ApiError>(result);
            if (error.kind == ApiError::Kind::Authentication || error.kind == ApiError::Kind::Verification) {
                message_ = error.message;
                emit changed();
                return;
            }
        }
        join();
    });
}

void ClassicPkController::cancelMatch()
{
    if (actionable() && phase_ == Phase::Matching) command(ClassicPkOp::Cancel);
}
void ClassicPkController::exitBattle()
{
    if (canExit()) command(ClassicPkOp::Exit);
}
void ClassicPkController::setContinuous(bool enabled)
{
    if (canSetContinuous() && enabled != continuous_) command(ClassicPkOp::SetContinuous, enabled);
}

void ClassicPkController::command(ClassicPkOp op, bool enabled)
{
    pending_ = op;
    message_.clear();
    ++readRevision_;
    if (op == ClassicPkOp::SetContinuous) ++switchRevision_;
    emit changed();
    request(op, [this, op, enabled, revision = phaseRevision_](Result<QJsonObject> result) {
        pending_.reset();
        ++readRevision_;
        if (const auto *error = std::get_if<ApiError>(&result)) {
            message_ = error->message;
            if (error->outcomeUnknown) {
                if (op == ClassicPkOp::SetContinuous) switchKnown_ = false;
                else uncertain_ = true;
                message_ += QStringLiteral(" 正在确认服务器状态。");
            }
        } else if (op == ClassicPkOp::Join && revision == phaseRevision_ &&
                   (phase_ == Phase::Idle || phase_ == Phase::Finished)) {
            phase_ = Phase::Matching;
            clearScores();
            const auto estimate = number(std::get<QJsonObject>(result).value("match_estimate"));
            if (estimate > 0) message_ = QStringLiteral("预计等待 %1 秒").arg(estimate);
        } else if (op == ClassicPkOp::Cancel && revision == phaseRevision_ && phase_ == Phase::Matching) {
            phase_ = Phase::Idle;
            clearScores();
            pkId_ = hintedId_ = timestamp_ = 0;
        } else if (op == ClassicPkOp::Exit && canExit()) {
            if (!applyUnified(std::get<QJsonObject>(result)) && revision == phaseRevision_) {
                uncertain_ = true;
                message_ = QStringLiteral("已提交结束请求，正在等待结算。");
            }
        } else if (op == ClassicPkOp::SetContinuous) {
            continuous_ = enabled;
        }
        if (op == ClassicPkOp::SetContinuous) refresh();
        else schedule(0);
        emit changed();
    }, enabled);
}

QString ClassicPkController::phaseLabel() const
{
    if (!online()) return QStringLiteral("开播后可使用");
    if (pending_ == ClassicPkOp::Precheck || pending_ == ClassicPkOp::Join) return QStringLiteral("正在申请匹配");
    if (!prompt_.isEmpty()) return QStringLiteral("等待确认");
    if (!serverLabel_.isEmpty()) return serverLabel_;
    switch (phase_) {
    case Phase::Idle: return QStringLiteral("尚未匹配");
    case Phase::Matching: return QStringLiteral("正在匹配");
    case Phase::Preparing: return QStringLiteral("赛前准备");
    case Phase::Playing: return QStringLiteral("对战中");
    case Phase::Sprint: return QStringLiteral("绝杀时刻");
    case Phase::Settling: return QStringLiteral("正在结算");
    case Phase::Punishing: return QStringLiteral("惩罚阶段");
    case Phase::Finished: return QStringLiteral("本场已结束");
    case Phase::Other: return QStringLiteral("正在参与其他 PK");
    }
    return {};
}

qint64 ClassicPkController::secondsLeft() const
{
    return deadline_ ? (deadline_->remainingTime() + 999) / 1000 : -1;
}
} // namespace livehime
