#pragma once

#include "plugin-issue.hpp"
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <optional>

namespace livehime {

struct PkSide {
    QString name;
    std::optional<qint64> votes;
};

class ClassicPkController final : public QObject {
    Q_OBJECT
public:
    using Send = std::function<void(ClassicPkOp, qint64, qint64, qint64, bool, Callback<QJsonObject>)>;
    enum class Phase { Idle, Matching, Preparing, Playing, Sprint, Settling, Punishing, Finished, Other };
    explicit ClassicPkController(Send send, QObject *parent = nullptr);
    void setContext(quint64 accountEpoch, qint64 uid, qint64 liveRoom, bool actionsAllowed);
    void refresh();
    void receive(const QJsonArray &events);
    void start();
    void confirmPrecheck(bool accept);
    void cancelMatch();
    void exitBattle();
    void setContinuous(bool enabled);

    bool online() const { return room_ > 0; }
    bool actionable() const { return online() && allowed_ && !pending_ && !uncertain_ && prompt_.isEmpty(); }
    bool canStart() const { return actionable() && (phase_ == Phase::Idle || phase_ == Phase::Finished); }
    bool canExit() const;
    bool canSetContinuous() const { return actionable() && switchKnown_ && !switchDisabled_; }
    bool continuous() const { return continuous_; }
    QString switchHint() const { return switchHint_; }
    Phase phase() const { return phase_; }
    qint64 battleId() const { return pkId_; }
    quint64 contextVersion() const { return epoch_; }
    const PkSide &home() const { return home_; }
    const PkSide &away() const { return away_; }
    QString message() const;
    QString phaseLabel() const;
    QString precheckText() const { return prompt_; }
    QString confirmText() const { return confirmText_; }
    qint64 secondsLeft() const;
signals:
    void changed();
    void problem(const PluginIssue &issue);
private:
    void request(ClassicPkOp op, Callback<QJsonObject> callback, bool enabled = false, qint64 lookupId = -1);
    void poll();
    void schedule(int delay);
    void join();
    void command(ClassicPkOp op, bool enabled = false);
    bool apply(const QJsonObject &data, bool roomLookup = false);
    bool applyUnified(const QJsonObject &data);
    void finishRound(qint64 id);
    void clearScores();
    Send send_;
    QTimer pollTimer_;
    quint64 epoch_ = 0, accountEpoch_ = 0, readRevision_ = 0, switchRevision_ = 0, phaseRevision_ = 0;
    qint64 uid_ = 0, room_ = 0, pkId_ = 0, hintedId_ = 0, timestamp_ = 0;
    QList<qint64> endedIds_;
    Phase phase_ = Phase::Idle;
    PkSide home_, away_;
    std::optional<QDeadlineTimer> deadline_;
    std::optional<ClassicPkOp> pending_;
    QString prompt_, confirmText_, message_, queryError_, resultText_, serverLabel_, switchHint_;
    bool allowed_ = false, reading_ = false, roomLookup_ = false, uncertain_ = false;
    bool switchReading_ = false, switchKnown_ = false, switchDisabled_ = false, continuous_ = false;
};
} // namespace livehime
