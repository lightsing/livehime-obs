// Reuse the explicit offline controller transport/output doubles. No account or network I/O.
#define main controller_fixture_unused_main
#include "controller-test.cpp"
#undef main
#include "room-realtime.hpp"
#include "goim-client.hpp"
#include <QUrlQuery>

namespace {
struct Read {
    protocol::Request request;
    Callback<QJsonObject> callback;
};
QList<Read> reads;
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
void BiliClient::cancelRealtime(QNetworkReply *) {}
} // namespace livehime
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    RoomRealtime service(&controller);
    signIn(controller);
    auto *transport = service.findChild<GoimClient *>();
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
    service.refreshAudience();
    const int staleAccount = reads.size() - 1;
    const auto epoch = service.contextEpoch();
    controller.logout();
    drain();
    transport->events(QJsonArray{warning});
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
