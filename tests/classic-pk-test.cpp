#include "classic-pk-window.hpp"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QUrlQuery>
#include <cstdio>
#include <cstdlib>

using namespace livehime;
using Op = ClassicPkOp;
using Phase = ClassicPkController::Phase;

namespace {
void check(bool ok, const char *reason)
{
    if (!ok) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
struct Call {
    Op op;
    qint64 room, id;
    bool enabled;
    Callback<QJsonObject> done;
};
QJsonObject score(qint64 id, int status, qint64 stamp, int votes = 17)
{
    return {{"pk_id", id}, {"battle_type", 1}, {"status", status}, {"match_status", 0},
            {"timestamp", stamp}, {"pk_frozen_time", stamp + 50},
            {"init_info", QJsonObject{{"room_id", 66}, {"uname", "away"}, {"votes", 13}}},
            {"match_info", QJsonObject{{"room_id", "55"}, {"uname", "home"}, {"votes", votes}}}};
}
QJsonArray broadcast(qint64 id, int status, qint64 stamp, int votes = 17)
{
    const auto s = score(id, status, stamp, votes);
    QJsonObject basic{{"pk_id", id}, {"status", status}, {"type", 1}, {"end_time", stamp + 50}};
    QJsonObject data{{"timestamp", stamp}, {"pk_basic", basic},
                     {"members", QJsonArray{s.value("init_info"), s.value("match_info")}}};
    return {QJsonObject{{"cmd", "PK_INFO"}, {"data", data}}};
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const auto build = [](Op op) { return protocol::classicPk(op, 11, 55, 789, true, "test-csrf", 1770000000); };
    const auto join = build(Op::Join), cancel = build(Op::Cancel), info = build(Op::Info), exit = build(Op::Exit);
    check(join.changesState && cancel.changesState && !info.changesState && join.method == "GET", "mutating GET");
    check(QUrlQuery(join.url).queryItemValue("battle_type") == "1" &&
          QUrlQuery(cancel.url).queryItemValue("sub_type") == "0" &&
          QUrlQuery(info.url).queryItemValue("is_anchor") == "1", "classic match wire parameters");
    check(exit.method == "POST" && exit.body.contains("stop_pk=1") && exit.body.contains("csrf=test-csrf") &&
          !exit.body.contains("anchor_uid") && !exit.signedBytes.contains("csrf"), "exit contract");
    const auto continuous = build(Op::SetContinuous);
    check(QUrlQuery(continuous.url).queryItemValue("ruid") == "11" &&
          !QUrlQuery(continuous.url).hasQueryItem("sign") && continuous.changesState, "continuous contract");
    QByteArray requeue = R"({"code":6204,"data":{"auto_match_status":2}})";
    check(std::holds_alternative<ApiError>(protocol::envelope(requeue)), "ordinary errors stay errors");
    const auto queued = std::get<QJsonObject>(protocol::envelope(requeue, false, true));
    check(queued.value("match_status").toInt() == 1, "classic queue status is preserved");

    QList<Call> calls;
    ClassicPkController session([&](Op op, qint64, qint64 room, qint64 id, bool enabled, Callback<QJsonObject> cb) {
        calls.append({op, room, id, enabled, std::move(cb)});
    });
    auto take = [&](Op op) -> Call {
        for (int attempt = 0; attempt < 4; ++attempt) {
            for (int i = 0; i < calls.size(); ++i)
                if (calls.at(i).op == op) return calls.takeAt(i);
            QCoreApplication::processEvents();
        }
        check(false, "expected request was not emitted");
        std::abort();
    };
    session.setContext(1, 11, 55, true);
    take(Op::ReadContinuous).done(QJsonObject{{"status", 1}, {"pk_version", QJsonArray{2}}});
    take(Op::Info).done(QJsonObject{{"status", 0}, {"match_status", 0}});
    check(session.canStart() && !session.continuous() && session.canSetContinuous(), "initial room");
    session.start();
    take(Op::Precheck).done(QJsonObject{{"show_type", 1}, {"tips", QJsonObject{{"content", "Confirm?"}}}});
    check(!session.precheckText().isEmpty() && calls.isEmpty(), "precheck waits for the user");
    session.confirmPrecheck(true);
    take(Op::Join).done(QJsonObject{{"match_estimate", 4}});
    check(session.phase() == Phase::Matching, "join acknowledges queue only");
    session.cancelMatch();
    auto slowCancel = take(Op::Cancel);
    const auto stamp = QDateTime::currentSecsSinceEpoch();
    session.receive(broadcast(789, 201, stamp));
    slowCancel.done(QJsonObject{});
    check(session.phase() == Phase::Playing && session.battleId() == 789 && session.canExit() &&
          session.home().votes == 17 && session.away().votes == 13 && session.secondsLeft() > 0,
          "start broadcast wins race with cancel and identifies home by room");
    session.receive(broadcast(789, 101, stamp - 1, 0));
    check(session.phase() == Phase::Playing && session.home().votes == 17, "old snapshot discarded");
    QWidget host;
    ClassicPkWindow window(&session, &host);
    window.show();
    QCoreApplication::processEvents();
    check(window.isWindow() && !window.isModal() && window.isVisible(), "independent modeless window");
    window.close();
    check(!window.isVisible() && session.battleId() == 789, "close leaves match active");
    window.show();
    QPushButton *finish = nullptr;
    for (auto *button : window.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("结束 PK")) finish = button;
    check(finish && finish->isEnabled(), "reopened window displays active match");
    finish->click();
    check(finish->text() == QStringLiteral("确认结束"), "manual exit requires confirmation");
    finish->click();
    auto leave = take(Op::Exit);
    check(leave.id == 789 && leave.room == 55, "manual exit bound to active round");
    leave.done(QJsonObject{});
    session.receive(broadcast(789, 1001, stamp + 1, 28));
    check(session.phase() == Phase::Finished && session.home().votes == 28, "terminal scoreboard retained");
    session.receive(broadcast(789, 201, stamp + 2));
    check(session.phase() == Phase::Finished, "finished battle cannot be resurrected");
    session.refresh();
    auto response = take(Op::Info);
    response.done(queued);
    if (session.phase() == Phase::Finished) take(Op::Info).done(queued); // first read predates the terminal push
    check(session.phase() == Phase::Matching, "server requeue without sending join");
    session.setContinuous(true);
    const auto toggle = take(Op::SetContinuous);
    check(toggle.enabled, "toggle sends the selected value");
    toggle.done(QJsonObject{});
    // A switch read already in flight when toggled must not restore the old value.
    take(Op::ReadContinuous).done(QJsonObject{{"status", 0}});
    take(Op::ReadContinuous).done(QJsonObject{{"status", 2}, {"pk_version", QJsonArray{1}}});
    check(session.continuous(), "server confirms continuous matching");
    session.cancelMatch();
    ApiError timeout{ApiError::Kind::Timeout, QStringLiteral("超时")};
    timeout.outcomeUnknown = true;
    QList<PluginIssue> problems;
    QObject::connect(&session, &ClassicPkController::problem, [&problems](const PluginIssue &issue) {
        problems.append(issue);
    });
    take(Op::Cancel).done(timeout);
    check(problems.size() == 1 && problems.first().codes["operation_id"].toInt() == int(Op::Cancel) &&
          problems.first().codes["outcome_unknown"].toBool(), "ambiguous error reaches shared feedback");
    session.cancelMatch();
    check(!session.actionable(), "ambiguous mutation blocks duplicate");
    auto old = take(Op::Info);
    session.setContext(2, 22, 99, true);
    old.done(score(789, 201, stamp));
    check(session.phase() == Phase::Idle && !session.home().votes, "stale callback isolated after room change");
    session.setContext(2, 22, 0, false);
    check(!session.canStart(), "no active room");

    session.setContext(3, 11, 55, true);
    session.receive(broadcast(801, 201, stamp));
    session.refresh();
    // Discard reads issued for the previous context in this simulated transport.
    auto current = take(Op::Info);
    while (current.room != 55) current = take(Op::Info);
    check(current.id == 0, "reconnect queries the room even with a cached battle id");
    current.done(score(802, 201, stamp + 8));
    check(session.battleId() == 802, "missed end and new round recover from the room snapshot");
    session.receive(broadcast(801, 201, stamp + 10, 999));
    check(session.battleId() == 802 && session.home().votes == 17, "old room event cannot replace recovered round");
    session.exitBattle();
    const auto settlement = broadcast(802, 1001, stamp + 12, 29).first().toObject();
    take(Op::Exit).done(settlement.value("data").toObject());
    check(session.phase() == Phase::Finished && session.home().votes == 29, "exit response carries authoritative settlement");
    return 0;
}
