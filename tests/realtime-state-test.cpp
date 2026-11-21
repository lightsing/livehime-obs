// Reuse the explicit offline controller transport/output doubles. No account or network I/O.
#define main controller_fixture_unused_main
#include "controller-test.cpp"
#undef main
#include "room-realtime.hpp"
#include "goim-client.hpp"
#include <QJsonDocument>
#include <QUrlQuery>

namespace {
struct Read {
    protocol::Request request;
    Callback<QJsonObject> callback;
};
QList<Read> reads;
QList<Callback<Done>> sends;
qint64 sentRoom = 0;
QString sentText;
int findRead(const QString &path, int start = 0) {
    for (int i = start; i < reads.size(); ++i)
        if (reads[i].request.url.path().contains(path))
            return i;
    return -1;
}
void reply(int index, QJsonObject data) {
    auto cb = reads[index].callback;
    cb(data);
}
} // namespace
namespace livehime {
QNetworkReply *BiliClient::fetchRealtime(const protocol::Request &request, Callback<QJsonObject> callback) {
    reads.append({request, std::move(callback)});
    return nullptr;
}
QNetworkReply *BiliClient::sendDanmaku(qint64 roomId, const QString &text, Callback<Done> callback) {
    sentRoom = roomId;
    sentText = text;
    sends.append(std::move(callback));
    return nullptr;
}
void BiliClient::cancelRealtime(QNetworkReply *) {}
} // namespace livehime
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    RoomRealtime service(&controller);
    int acknowledged = 0;
    QString sendStatus;
    QObject::connect(&service, &RoomRealtime::danmakuSendFinished, [&](bool accepted, const QString &status) {
        if (accepted) ++acknowledged;
        sendStatus = status;
    });
    service.sendDanmaku("hello");
    check(sends.isEmpty(), "signed-out chat cannot write");
    signIn(controller);
    service.sendDanmaku("   ");
    check(sends.isEmpty(), "invalid chat cannot write");
    service.sendDanmaku(QStringLiteral("主播你好"));
    service.sendDanmaku("duplicate");
    check(sends.size() == 1 && sentRoom == serverRoom.roomId && sentText == QStringLiteral("主播你好") &&
              service.sendingDanmaku(), "one pending chat write belongs to the current room");
    sends.last()(ApiError{ApiError::Kind::Api, QStringLiteral("已禁言"), 1001});
    check(!service.sendingDanmaku() && sendStatus.contains(QStringLiteral("已禁言")) && acknowledged == 0 &&
              sends.size() == 1, "server rejection is shown without retry");
    service.sendDanmaku("hello");
    sends.last()(Done{});
    check(acknowledged == 1 && !service.sendingDanmaku(), "only a confirmed write is acknowledged");
    auto *transport = service.findChild<GoimClient *>();
    const auto legacy = [](int type, qint64 room, QString name) {
        return QJsonObject{{"cmd", "INTERACT_WORD"}, {"data", QJsonObject{{"msg_type", type},
                    {"roomid", room}, {"uname", name}}}};
    };
    // Google protobuf vectors for InteractWord #2/5/6/7/21/22 and UserInfo.base/anon.
    const auto v2 = [](const char *base64) {
        return QJsonObject{{"cmd", "INTERACT_WORD_V2"}, {"data", QJsonObject{{"pb", QLatin1String(base64)}}}};
    };
    QJsonArray delivered;
    QObject::connect(&service, &RoomRealtime::events, [&](const QJsonArray &batch) { delivered = batch; });
    auto firstEntry = legacy(1, 456, "first");
    firstEntry.insert("msg_id", "offline-entry");
    transport->events({firstEntry, firstEntry, legacy(2, 456, "follow"), legacy(3, 456, "share"),
        legacy(1, 999, "wrong room"), v2("EgZsZWdhY3koATDIAziA4s+qBrIBDRILCglzeW50aGV0aWM="),
        v2("EgZsZWdhY3koAjDIAziA4s+qBrIBDRILCglzeW50aGV0aWM="),
        v2("EgZsZWdhY3koATDnBziA4s+qBrIBDRILCglzeW50aGV0aWM="), v2("!!!!"), v2("ogE=")});
    check(service.entryHistory().size() == 2 && service.entryHistory().first().name == "first" &&
          service.entryHistory().first().timestamp > 0 && service.entryHistory().last().name == "synthetic" &&
          service.entryHistory().last().timestamp == 1700000000 && delivered.size() == 5,
          "hidden room collects both entry formats, keeps other interactions live and drops wrong-room or malformed events");
    transport->events({v2("EgZsZWdhY3koATDIAziA4s+qBqgBAbIBDRILCglzeW50aGV0aWM="),
        v2("EgZsZWdhY3koATDIAziA4s+qBrIBDxINCglzeW50aGV0aWMgAQ=="),
        v2("EgZsZWdhY3koATDIAziA4s+qBrIBERILCglzeW50aGV0aWNKAigB")});
    check(service.entryHistory().size() == 5 && delivered.size() == 3 &&
          !QJsonDocument(delivered).toJson().contains("synthetic") &&
          !QJsonDocument(delivered).toJson().contains("legacy") &&
          service.entryHistory().last().name == QStringLiteral("神秘用户"), "V2 anonymity at all three levels");
    auto hidden = legacy(1, 456, "private-name");
    auto privateData = hidden.value("data").toObject();
    privateData.insert("user_info", QJsonObject{{"is_mystery", 1}});
    privateData.insert("id_str", "offline-hidden");
    hidden.insert("data", privateData);
    transport->events({hidden, hidden, QJsonObject{{"cmd", "ENTRY_EFFECT"}, {"data", privateData}}});
    check(service.entryHistory().size() == 6 &&
          service.entryHistory().last().name == QStringLiteral("神秘用户") &&
          delivered.first().toObject().value("data").toObject().value("uname") == QStringLiteral("神秘用户"),
          "V1 alternate user_info masks identity; stable event ID and separate effect are counted once");
    QJsonArray manyEntries;
    for (int i = 0; i < 501; ++i)
        manyEntries.append(legacy(1, 456, QString::number(i)));
    transport->events(manyEntries);
    check(service.entryHistory().size() == 500 && service.entryHistory().first().name == "1" &&
              service.entryHistory().last().name == "500" &&
              service.entryHistory().last().sequence - service.entryHistory().first().sequence == 499,
          "entry memory bounded in arrival order with stable row numbers");
    QStringList notices;
    QObject::connect(&service, &RoomRealtime::moderationNotice, [&](const ModerationNotice &notice) {
        notices.append(notice.body);
    });
    const QJsonObject warning{{"cmd", "WARNING"}, {"msg", "first"}, {"roomid", serverRoom.roomId}};
    const QJsonObject ecology{{"cmd", "ANCHOR_ECOLOGY_LIVING_DIALOG"},
                              {"data", QJsonObject{{"dialog_title", "live only"}}}};
    transport->events(QJsonArray{QJsonObject{{"cmd", "DANMU_MSG"}, {"msg", "pretend warning"}},
                                 QJsonObject{{"cmd", "WARNING"}, {"msg", "other room"}, {"roomid", 999}},
                                 ecology, warning});
    check(notices == QStringList{"first"}, "only current-room moderation is delivered with all docks hidden");
    check(starts == 0 && stops == 0, "displaying a moderation notice does not change stream state");
    serverRoom.liveStatus = 1;
    controller.refresh();
    drain();
    transport->events(QJsonArray{ecology});
    check(notices.size() == 2, "ecology dialog is delivered when this room is live");
    serverRoom.liveStatus = 0;
    controller.refresh();
    drain();
    int contextChanges = 0;
    QObject::connect(&service, &RoomRealtime::contextChanged, &app, [&] { ++contextChanges; });
    check(findRead("getDanmuInfo") >= 0, "authenticated room starts one realtime configuration");
    const int initial = reads.size();
    controller.refresh();
    drain();
    check(reads.size() == initial, "same-room refresh does not reconnect");
    check(contextChanges == 0, "same-room refresh does not clear history or reset rank mode");
    check(service.entryHistory().size() == 500, "same-room refresh keeps entries");
    service.setVisible(1, true);
    const int oldRank = findRead("queryContributionRank");
    service.selectAudience("intimacy_rank", "intimacy_rank");
    const int newRank = reads.size() - 1;
    reply(oldRank, QJsonObject{{"item", QJsonArray{QJsonObject{{"name", "old"}}}}});
    check(service.audienceData().isEmpty(), "late old-mode audience callback ignored");
    reply(newRank, QJsonObject{{"item", QJsonArray{QJsonObject{{"name", "new"}}}}});
    check(service.audienceData().value("item").toArray().first().toObject().value("name") == "new",
          "current audience callback accepted");
    service.setVisible(2, true);
    const int firstGuard = reads.size() - 1;
    reply(firstGuard, QJsonObject{{"info", QJsonObject{{"now", 1}, {"num", 4}}},
                                  {"top3", QJsonArray{QJsonObject{{"rank", 1}}}},
                                  {"list", QJsonArray{QJsonObject{{"rank", 2}}}}});
    service.loadMoreGuard();
    const int secondGuard = reads.size() - 1;
    reply(secondGuard, QJsonObject{{"info", QJsonObject{{"now", 2}, {"num", 4}}},
                                   {"top3", QJsonArray{QJsonObject{{"rank", 99}}}},
                                   {"list", QJsonArray{QJsonObject{{"rank", 3}}}}});
    check(service.guardData().value("rows").toArray().size() == 3,
          "guard top3 included only on first response page");
    service.refreshGuard();
    const int refreshPage = reads.size() - 1;
    reply(refreshPage, QJsonObject{{"info", QJsonObject{{"now", 1}, {"num", 4}}},
                                   {"top3", QJsonArray{QJsonObject{{"rank", 1}}}},
                                   {"list", QJsonArray{QJsonObject{{"rank", 2}}}}});
    check(service.guardData().value("rows").toArray().size() == 2,
          "explicit refresh starts a new coherent first-page snapshot");
    service.selectGuard(3);
    const int staleGuard = reads.size() - 1;
    service.selectGuard(5);
    const int currentGuard = reads.size() - 1;
    reply(staleGuard, QJsonObject{{"info", QJsonObject{{"now", 1}, {"num", 1}}},
                                  {"list", QJsonArray{QJsonObject{{"rank", 9}}}}});
    check(service.guardData().isEmpty(), "late guard mode page isolated");
    reply(currentGuard,
          QJsonObject{{"info", QJsonObject{{"now", 1}, {"num", 0}}}, {"list", QJsonValue::Null}});
    check(!service.guardHasMore() && service.guardData().value("rows").toArray().isEmpty(),
          "null list accepted as legitimate empty guard page");
    serverRoom.roomId = 789;
    controller.refresh();
    drain();
    transport->events({firstEntry, legacy(1, 789, "new room")});
    check(service.entryHistory().size() == 1 && service.entryHistory().first().name == "new room",
          "switching rooms clears entries and rejects late old-room arrivals");
    service.refreshAudience();
    const int staleAccount = reads.size() - 1;
    const auto epoch = service.contextEpoch();
    service.sendDanmaku("stale send");
    controller.logout();
    drain();
    sends.last()(Done{});
    check(acknowledged == 1 && !service.sendingDanmaku() && !service.canSendDanmaku(),
          "late chat reply cannot affect a new account context");
    transport->events(QJsonArray{warning});
    check(service.entryHistory().isEmpty(), "logout clears entry history");
    check(notices.size() == 2, "logged-out service ignores warnings");
    reply(staleAccount, QJsonObject{{"item", QJsonArray{QJsonObject{{"name", "leaked"}}}}});
    check(service.contextEpoch() > epoch && service.audienceData().isEmpty(),
          "logout clears model and invalidates late account callback");
    int serviceStops = 0;
    QObject::connect(&service, &RoomRealtime::stopped, &app, [&] { ++serviceStops; });
    const int beforeStop = contextChanges;
    service.stop();
    service.stop();
    check(serviceStops == 1 && contextChanges == beforeStop, "shutdown clears alerts without reloading room views");
    {
        auto *owner = new LiveController(directory.path());
        auto *dependent = new RoomRealtime(owner);
        delete owner;
        dependent->stop();
        delete dependent;
        check(true, "controller-first destruction safely stops realtime service");
    }
    if (failures)
        return 1;
    std::cout << "Realtime context, mode isolation and pagination passed.\n";
    return 0;
}
