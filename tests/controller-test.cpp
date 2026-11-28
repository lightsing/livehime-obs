// Real controller, fake BiliClient/ObsBridge. No network calls or OBS runtime.
#include "live-controller.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <iostream>

using namespace livehime;
namespace {
int failures = 0;
QStringList events;
UserInfo nextUser{123, QStringLiteral("Synthetic")};
RoomInfo serverRoom;
std::optional<ApiError> nextStartError;
std::optional<ApiError> nextStopError;
std::optional<ApiError> nextTitleError;
std::optional<ApiError> nextCoverError;
std::optional<RoomInfo> nextRoomReply;
std::optional<ApiError> nextRoomError;
QStringList titleWrites;
QStringList coverWrites;
QString titleAtStart;
QUrl coverAtStart;
struct NetworkState { QList<QNetworkCookie> cookies; quint64 generation = 0; };
QHash<BiliClient *, NetworkState> networkState;
struct MediaState { bool busy = false; bool owned = false; };
QHash<ObsBridge *, MediaState> mediaState;
ObsBridge *activeBridge = nullptr;
int starts = 0;
int stops = 0;
bool holdNextRoom = false;
bool holdNextTitle = false;
bool holdNextCover = false;
std::function<void()> releaseRoom;
std::function<void()> releaseTitle;
std::function<void()> releaseCover;

void check(bool value, const char *name)
{
    if (!value) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}
void drain()
{
    for (int i = 0; i != 20; ++i) QCoreApplication::processEvents();
}
template<class T>
void complete(BiliClient *client, Callback<T> callback, Result<T> result)
{
    const auto generation = networkState[client].generation;
    QTimer::singleShot(0, client, [client, generation, callback = std::move(callback), result = std::move(result)]() mutable {
        if (networkState[client].generation == generation) callback(std::move(result));
    });
}
QList<QNetworkCookie> syntheticCookies()
{
    QNetworkCookie cookie("bili_jct", "synthetic-only");
    cookie.setDomain(QStringLiteral(".bilibili.com"));
    cookie.setPath(QStringLiteral("/"));
    cookie.setSecure(true);
    return {cookie};
}
void reset()
{
    events.clear(); starts = 0; stops = 0;
    nextStartError.reset(); nextStopError.reset();
    nextTitleError.reset(); nextCoverError.reset(); nextRoomReply.reset(); nextRoomError.reset();
    titleWrites.clear(); coverWrites.clear(); titleAtStart.clear(); coverAtStart = {};
    holdNextRoom = false; holdNextTitle = false; holdNextCover = false;
    releaseRoom = {}; releaseTitle = {}; releaseCover = {};
    nextUser = {123, QStringLiteral("Synthetic")};
    serverRoom = {};
    serverRoom.roomId = 456;
    serverRoom.areaId = 1;
    serverRoom.liveStatus = 0;
    serverRoom.title = QStringLiteral("Original");
}
void signIn(LiveController &controller)
{
    controller.acceptCookies(syntheticCookies());
    drain();
    check(controller.authenticated() && controller.state() == LiveController::State::Ready, "synthetic login ready");
    events.clear();
}
void start(LiveController &controller)
{
    controller.start(QStringLiteral("Updated"), QStringLiteral("synthetic-cover.jpg"), 1);
    drain();
}
void normalLifecycle()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    start(controller);
    check(controller.state() == LiveController::State::Live && controller.hasRemoteSession(), "normal live state");
    const QStringList expected{QStringLiteral("api.title"), QStringLiteral("api.cover"), QStringLiteral("api.room"),
                               QStringLiteral("api.start"), QStringLiteral("obs.start")};
    check(events == expected, "metadata and room check precede start then OBS output");
    emit activeBridge->reconnecting();
    check(controller.state() == LiveController::State::Reconnecting, "OBS automatic reconnect state");
    emit activeBridge->reconnected();
    check(controller.state() == LiveController::State::Live && starts == 1, "automatic reconnect does not call startLive");
    mediaState[activeBridge].busy = false;
    emit activeBridge->stopped(false);
    check(controller.state() == LiveController::State::Recoverable && controller.hasStreamCredentials(), "unexpected disconnect retains credentials");
    controller.retryConnection();
    drain();
    check(controller.state() == LiveController::State::Live && starts == 1, "manual retry reuses one remote session");
    events.clear();
    controller.stop();
    drain();
    check(events.indexOf(QStringLiteral("obs.stop")) >= 0 &&
              events.indexOf(QStringLiteral("obs.stop")) < events.indexOf(QStringLiteral("api.stop")), "intentional stop ends media before API");
    check(controller.state() == LiveController::State::Ready && !controller.hasRemoteSession() &&
              !controller.hasStreamCredentials(), "successful stop clears session");
}
void uncertainStart()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    nextStartError = ApiError{ApiError::Kind::Timeout, QStringLiteral("Synthetic timeout")};
    nextStartError->outcomeUnknown = true;
    QList<PluginIssue> startIssues;
    QObject::connect(&controller, &LiveController::problem, &controller, [&startIssues](const PluginIssue &issue) {
        if (issue.operation == QStringLiteral("开播") || issue.operation == QStringLiteral("直播状态"))
            startIssues.append(issue);
    });
    start(controller);
    check(startIssues.size() == 1 && startIssues.first().codes.value("outcome_unknown").toBool()
          && startIssues.first().exportReason.isEmpty(), "start timeout reports one structured failure");
    check(controller.state() == LiveController::State::Uncertain && controller.hasRemoteSession(), "start timeout retains remote ownership");
    check(!events.contains(QStringLiteral("obs.start")) && !controller.hasStreamCredentials(), "uncertain start never invents credentials");
    controller.start(QStringLiteral("Again"), {}, 1);
    controller.retryConnection();
    drain();
    check(starts == 1, "uncertain result prevents duplicate start");
    controller.stop(); drain();
    check(stops == 1 && !controller.hasRemoteSession(), "uncertain start supports explicit stop recovery");
}
void stopFailureAndAccountBoundary()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller); start(controller);
    const auto oldUid = controller.user().uid;
    nextUser = {999, QStringLiteral("Other account")};
    controller.acceptCookies(syntheticCookies()); drain();
    check(controller.user().uid == oldUid && controller.hasRemoteSession(), "account switch cannot transfer an unfinished room");
    // An authenticated active session should be rejected before replacing cookies.
    check(controller.authenticated(), "active-session account switch preserves original authentication");
    nextStopError = ApiError{ApiError::Kind::Network, QStringLiteral("Synthetic stop failure")};
    nextStopError->outcomeUnknown = true;
    controller.stop(); drain();
    check(controller.state() == LiveController::State::Uncertain && controller.hasRemoteSession(), "failed stop retains remote session");
    controller.stop(); drain();
    check(stops == 2 && controller.state() == LiveController::State::Ready && !controller.hasRemoteSession(), "stop failure can be explicitly retried");
}
void roomGates()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    serverRoom.liveStatus = -1;
    controller.start(QStringLiteral("Original"), {}, 1); drain();
    check(starts == 0 && controller.state() == LiveController::State::Uncertain, "unknown live status cannot start");
    serverRoom.liveStatus = 0;
    serverRoom.canLive = false;
    serverRoom.blockMessage = QStringLiteral("Synthetic blocked cover");
    controller.refresh(); drain();
    controller.start(QStringLiteral("Original"), {}, 1); drain();
    check(starts == 0, "metadata audit intercept cannot start");
}
void staleCallbacksAfterExternalStop()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller); start(controller);
    holdNextRoom = true;
    controller.refresh(); drain();
    check(controller.working() && static_cast<bool>(releaseRoom), "room refresh held in flight");
    mediaState[activeBridge].busy = false;
    emit activeBridge->stopped(true); // A stop from OBS while the dock refreshes.
    drain();
    check(!controller.hasRemoteSession() && controller.state() == LiveController::State::Ready, "external stop completes during in-flight refresh");
    releaseRoom(); releaseRoom = {}; drain();
    check(!controller.hasRemoteSession() && !controller.working() && controller.state() == LiveController::State::Ready,
          "stale live-room response cannot resurrect completed session");

    start(controller);
    check(controller.state() == LiveController::State::Live, "second synthetic session live");
    holdNextTitle = true;
    events.clear();
    controller.saveMetadata(QStringLiteral("Delayed title"), QStringLiteral("delayed-cover.jpg"));
    drain();
    check(controller.metadataBusy() && !controller.working() && static_cast<bool>(releaseTitle), "metadata save held without blocking control actions");
    mediaState[activeBridge].busy = false;
    emit activeBridge->stopped(true);
    drain();
    releaseTitle(); releaseTitle = {}; drain();
    check(!events.contains(QStringLiteral("api.cover")) && !events.contains(QStringLiteral("api.room")),
          "stale title result cannot continue cover upload or room refresh after stop");
    check(!controller.hasRemoteSession() && !controller.working() && controller.state() == LiveController::State::Ready,
          "stale metadata result cannot overwrite completed-stop state");
}

void debounceAndNewDraftDuringSave()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    check(controller.titleDraft() == QStringLiteral("Original"), "editor initialized from room title");
    controller.setTitleDraft(QStringLiteral("First keystrokes"));
    controller.setTitleDraft(QStringLiteral("Final draft"));
    drain();
    check(titleWrites.isEmpty() && controller.metadataBusy(), "typing debounces without immediate requests");
    controller.flushTitle();
    drain();
    check(titleWrites == QStringList{QStringLiteral("Final draft")} &&
              controller.titleSyncStatus() == QStringLiteral("已同步"), "flush sends only final debounced title");

    holdNextTitle = true;
    controller.setTitleDraft(QStringLiteral("In flight A"));
    controller.flushTitle(); drain();
    controller.setTitleDraft(QStringLiteral("New input B"));
    controller.flushTitle(); drain();
    check(titleWrites.size() == 2 && controller.titleDraft() == QStringLiteral("New input B"),
          "new draft retained while older title is in flight");
    releaseTitle(); releaseTitle = {}; drain();
    check(titleWrites == QStringList{QStringLiteral("Final draft"), QStringLiteral("In flight A"), QStringLiteral("New input B")},
          "in-flight response drains latest pending draft exactly once");
    check(controller.titleDraft() == QStringLiteral("New input B") && !controller.metadataBusy(),
          "editor and metadata queue settle on latest title");
}

void backgroundReadPreservesDraft()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    holdNextRoom = true;
    controller.setTitleDraft(QStringLiteral("Saved before read"));
    controller.flushTitle(); drain();
    check(controller.working() && controller.metadataBusy() && static_cast<bool>(releaseRoom),
          "post-save room read remains metadataBusy so the editor stays enabled");
    controller.setTitleDraft(QStringLiteral("Typed during read"));
    controller.flushTitle();
    releaseRoom(); releaseRoom = {}; drain();
    check(controller.titleDraft() == QStringLiteral("Typed during read") && titleWrites.back() == controller.titleDraft(),
          "old room read cannot replace text typed while it was in flight");
}

void debounceTimerSavesWithoutBlur()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    controller.setTitleDraft(QStringLiteral("Fi"));
    QEventLoop wait;
    QTimer::singleShot(1500, &wait, &QEventLoop::quit);
    wait.exec();
    check(titleWrites.isEmpty(), "a short typing pause does not save a partial title");
    QElapsedTimer idle;
    idle.start();
    controller.setTitleDraft(QStringLiteral("Final title"));
    QObject::connect(&controller, &LiveController::metadataSaved, &wait, &QEventLoop::quit);
    QTimer::singleShot(8000, &wait, &QEventLoop::quit);
    wait.exec();
    drain();
    check(idle.elapsed() >= 4900 && titleWrites == QStringList{QStringLiteral("Final title")} &&
              !controller.metadataBusy(), "save only the final title after five idle seconds");
}

void failedBackgroundReadDoesNotStrandNewDraft()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    holdNextRoom = true;
    nextRoomError = ApiError{ApiError::Kind::Network, QStringLiteral("Synthetic background read failure")};
    controller.setTitleDraft(QStringLiteral("Saved A")); controller.flushTitle(); drain();
    controller.setTitleDraft(QStringLiteral("New B during failed read")); controller.flushTitle();
    releaseRoom(); releaseRoom = {}; drain();
    check(titleWrites == QStringList{QStringLiteral("Saved A"), QStringLiteral("New B during failed read")} &&
              controller.titleDraft() == QStringLiteral("New B during failed read") && !controller.metadataBusy(),
          "background read failure releases queued newer draft without replaying failed reads");
}

void staleReadAfterSuccessfulWrite()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    nextRoomReply = serverRoom; // Cached/audit response still contains Original.
    controller.setTitleDraft(QStringLiteral("Newly committed title"));
    controller.flushTitle(); drain();
    check(controller.room().title == QStringLiteral("Original") &&
              controller.titleDraft() == QStringLiteral("Newly committed title"),
          "lagging read after accepted write cannot roll back committed editor text");
    check(titleWrites.size() == 1 && controller.titleSyncStatus() == QStringLiteral("已同步"),
          "lagging read neither invents a dirty draft nor replays the write");
    controller.refresh(); drain();
    check(controller.room().title == controller.titleDraft(), "later matching echo reconciles title state");
}

void blurStartWaitsForLatestMetadata()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    holdNextTitle = true;
    controller.setTitleDraft(QStringLiteral("Title A"));
    controller.flushTitle(); // editingFinished fires before the Start click.
    controller.saveCover(QStringLiteral("confirmed-crop-1.png"));
    controller.saveCover(QStringLiteral("confirmed-crop-2.png"));
    controller.start(QStringLiteral("Title A"), {}, 1);
    check(controller.state() == LiveController::State::Preparing && starts == 0,
          "start click queues behind blur-triggered autosave");
    controller.setTitleDraft(QStringLiteral("Latest title B"));
    controller.flushTitle();
    releaseTitle(); releaseTitle = {}; drain();
    check(titleWrites == QStringList{QStringLiteral("Title A"), QStringLiteral("Latest title B")},
          "queued start uses latest draft rather than the older in-flight title");
    check(coverWrites == QStringList{QStringLiteral("confirmed-crop-1.png"), QStringLiteral("confirmed-crop-2.png")},
          "all explicitly confirmed crops are serialized without loss");
    check(starts == 1 && titleAtStart == QStringLiteral("Latest title B") && coverAtStart == serverRoom.coverUrl &&
              controller.state() == LiveController::State::Live, "start occurs once after final title and cover save");
}

void metadataFailureCancelsStartIntent()
{
    {
        reset();
        QTemporaryDir directory;
        LiveController controller(directory.path());
        signIn(controller);
        nextTitleError = ApiError{ApiError::Kind::Api, QStringLiteral("Synthetic title rejection")};
        controller.start(QStringLiteral("Rejected title"), {}, 1); drain();
        check(starts == 0 && controller.state() == LiveController::State::Ready &&
                  controller.titleSyncStatus().contains(QStringLiteral("失败")), "title failure cancels queued start and is visible");
        drain();
        check(titleWrites.size() == 1, "failed title is not retried automatically");
        controller.start(QStringLiteral("Rejected title"), {}, 1); drain();
        check(starts == 1 && titleWrites.size() == 2, "a new explicit start permits one title retry");
    }
    {
        reset();
        QTemporaryDir directory;
        LiveController controller(directory.path());
        signIn(controller);
        nextCoverError = ApiError{ApiError::Kind::Network, QStringLiteral("Synthetic cover failure")};
        controller.saveCover(QStringLiteral("retryable-crop.png"));
        controller.start(QStringLiteral("Original"), {}, 1); drain();
        check(starts == 0 && controller.status().contains(QStringLiteral("封面上传失败")), "cover failure blocks start with visible error");
        drain();
        check(coverWrites.size() == 1, "failed cover does not retry in a loop");
        bool reentered = false;
        QObject::connect(&controller, &LiveController::changed, [&] {
            if (!reentered && controller.state() == LiveController::State::Preparing) {
                reentered = true;
                controller.flushTitle(); // Widget disable can emit editingFinished synchronously.
            }
        });
        controller.start(QStringLiteral("Original"), {}, 1); drain();
        check(reentered && starts == 1 && coverWrites == QStringList{QStringLiteral("retryable-crop.png"), QStringLiteral("retryable-crop.png")},
              "new explicit start can retry the retained failed crop once");
    }
}

void accountSwitchClearsAutosaveQueue()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller);
    holdNextTitle = true;
    controller.setTitleDraft(QStringLiteral("Old account draft")); controller.flushTitle();
    controller.saveCover(QStringLiteral("old-account-crop.png"));
    nextUser = {999, QStringLiteral("New account")};
    serverRoom.title = QStringLiteral("New account room");
    controller.acceptCookies(syntheticCookies()); drain();
    releaseTitle(); releaseTitle = {}; drain();
    check(controller.user().uid == 999 && controller.titleDraft() == QStringLiteral("New account room") &&
              coverWrites.isEmpty() && !controller.metadataBusy(), "account switch cancels old drafts, crops and callbacks");
}

void explicitStopBypassesAutosave()
{
    reset();
    QTemporaryDir directory;
    LiveController controller(directory.path());
    signIn(controller); start(controller);
    holdNextTitle = true;
    controller.setTitleDraft(QStringLiteral("Typing before Stop")); controller.flushTitle();
    controller.saveCover(QStringLiteral("crop-before-stop.png"));
    controller.stop(); drain();
    check(stops == 1 && !controller.hasRemoteSession(), "blur-triggered autosave cannot delay explicit down-stream stop");
    const auto writesBefore = coverWrites.size();
    releaseTitle(); releaseTitle = {}; drain();
    check(coverWrites.size() == writesBefore && controller.state() == LiveController::State::Ready,
          "late save callback after stop cannot drain canceled cover queue");
    check(controller.titleDraft() == QStringLiteral("Typing before Stop"), "canceling autosave preserves visible unsaved draft");
}
} // namespace

// Substitute implementations keep the production public headers and Qt signals.
namespace livehime {
BiliClient::BiliClient(QObject *parent) : QObject(parent) { networkState.insert(this, {}); }
BiliClient::~BiliClient() { networkState.remove(this); }
void BiliClient::setDeviceId(const QString &) {}
void BiliClient::setCookies(const QList<QNetworkCookie> &cookies)
{
    networkState[this].cookies = cookies;
    ++networkState[this].generation;
    emit cookiesChanged();
}
QList<QNetworkCookie> BiliClient::cookies() const { return networkState[const_cast<BiliClient *>(this)].cookies; }
void BiliClient::logout() { networkState[this].cookies.clear(); ++networkState[this].generation; emit cookiesChanged(); }
void BiliClient::validateSession(Callback<UserInfo> callback)
{ events.append(QStringLiteral("api.nav")); complete<UserInfo>(this, std::move(callback), nextUser); }
void BiliClient::fetchRoom(qint64, Callback<RoomInfo> callback)
{
    events.append(QStringLiteral("api.room"));
    Result<RoomInfo> reply = nextRoomReply.value_or(serverRoom);
    if (nextRoomError) { reply = *nextRoomError; nextRoomError.reset(); }
    nextRoomReply.reset();
    if (holdNextRoom) {
        holdNextRoom = false;
        releaseRoom = [callback = std::move(callback), reply]() mutable { callback(reply); };
    } else complete<RoomInfo>(this, std::move(callback), reply);
}
void BiliClient::fetchAreas(Callback<QList<AreaInfo>> callback)
{
    complete<QList<AreaInfo>>(this, std::move(callback), QList<AreaInfo>{{1, 2, QStringLiteral("Synthetic area"), QStringLiteral("Synthetic parent")}});
}
void BiliClient::fetchCover(const QUrl &, Callback<QByteArray> callback)
{
    complete<QByteArray>(this, std::move(callback), ApiError{ApiError::Kind::InvalidResponse, QStringLiteral("No image in offline controller fixture")});
}
void BiliClient::updateTitle(const QString &title, Callback<Done> callback)
{
    events.append(QStringLiteral("api.title")); titleWrites.append(title);
    Result<Done> result = Done{};
    if (nextTitleError) { result = *nextTitleError; nextTitleError.reset(); }
    else serverRoom.title = title;
    if (holdNextTitle) {
        holdNextTitle = false;
        releaseTitle = [callback = std::move(callback), result]() mutable { callback(result); };
    } else complete<Done>(this, std::move(callback), result);
}
void BiliClient::uploadCover(const QString &path, Callback<QUrl> callback)
{
    events.append(QStringLiteral("api.cover")); coverWrites.append(path);
    Result<QUrl> result = QUrl(QStringLiteral("https://i0.hdslb.com/synthetic-%1.jpg").arg(coverWrites.size()));
    if (nextCoverError) { result = *nextCoverError; nextCoverError.reset(); }
    else serverRoom.coverUrl = std::get<QUrl>(result);
    if (holdNextCover) {
        holdNextCover = false;
        releaseCover = [callback = std::move(callback), result]() mutable { callback(result); };
    } else complete<QUrl>(this, std::move(callback), result);
}
void BiliClient::startLive(qint64 roomId, int, Callback<LiveSession> callback)
{
    events.append(QStringLiteral("api.start")); ++starts;
    titleAtStart = serverRoom.title;
    coverAtStart = serverRoom.coverUrl;
    if (nextStartError) {
        const auto error = *nextStartError; nextStartError.reset();
        complete<LiveSession>(this, std::move(callback), error); return;
    }
    serverRoom.liveStatus = 1;
    complete<LiveSession>(this, std::move(callback), LiveSession{roomId, QStringLiteral("rtmp://example.invalid/live/"), QStringLiteral("synthetic")});
}
void BiliClient::stopLive(qint64, Callback<Done> callback)
{
    events.append(QStringLiteral("api.stop")); ++stops;
    if (nextStopError) {
        const auto error = *nextStopError; nextStopError.reset();
        complete<Done>(this, std::move(callback), error); return;
    }
    serverRoom.liveStatus = 0;
    complete<Done>(this, std::move(callback), Done{});
}
ObsBridge::ObsBridge(QObject *parent) : QObject(parent) { mediaState.insert(this, {}); activeBridge = this; }
ObsBridge::~ObsBridge() { mediaState.remove(this); if (activeBridge == this) activeBridge = nullptr; }
bool ObsBridge::busy() const { return mediaState.value(const_cast<ObsBridge *>(this)).busy; }
bool ObsBridge::ownsService() const { return mediaState.value(const_cast<ObsBridge *>(this)).owned; }
bool ObsBridge::start(const LiveSession &, QString *)
{
    events.append(QStringLiteral("obs.start"));
    mediaState[this] = {true, true};
    QTimer::singleShot(0, this, [this] { emit started(); });
    return true;
}
void ObsBridge::stop()
{
    events.append(QStringLiteral("obs.stop"));
    mediaState[this].busy = false;
    QTimer::singleShot(0, this, [this] { emit stopped(true); });
}
void ObsBridge::restoreService() { events.append(QStringLiteral("obs.restore")); mediaState[this].owned = false; }
} // namespace livehime

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    normalLifecycle();
    uncertainStart();
    stopFailureAndAccountBoundary();
    roomGates();
    staleCallbacksAfterExternalStop();
    debounceAndNewDraftDuringSave();
    backgroundReadPreservesDraft();
    debounceTimerSavesWithoutBlur();
    failedBackgroundReadDoesNotStrandNewDraft();
    staleReadAfterSuccessfulWrite();
    blurStartWaitsForLatestMetadata();
    metadataFailureCancelsStartIntent();
    accountSwitchClearsAutosaveQueue();
    explicitStopBypassesAutosave();
    if (failures) return 1;
    std::cout << "Offline controller lifecycle, autosave and account-boundary tests passed.\n";
    return 0;
}
