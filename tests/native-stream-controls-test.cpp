// Production adapter + actual Qt widgets/metaobjects. No OBS process or network.
#include "native-stream-controls.hpp"
#include "native-controls-fixture.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <iostream>
#include <memory>

using livehime::NativeStreamControls;
using Action = NativeStreamControls::Action;
using test_support::NativeControlsWindow;
using test_support::OBSBasicControls;

namespace {
int failures = 0;
int checks = 0;
void check(bool value, const char *message)
{
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void drain()
{
    for (int i = 0; i != 12; ++i) QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
bool attach(NativeStreamControls &adapter, NativeControlsWindow &host)
{
    QString error;
    const bool attached = adapter.attach(&host, &error);
    check(attached, "attach accepts exact OBS-shaped typed signal graph");
    if (!attached) std::cerr << "Attach detail: " << error.toStdString() << '\n';
    return attached;
}
struct Calls {
    explicit Calls(NativeStreamControls &adapter)
    {
        startConnection = QObject::connect(&adapter, &NativeStreamControls::startRequested,
                                           &adapter, [this] { ++starts; });
        stopConnection = QObject::connect(&adapter, &NativeStreamControls::stopRequested,
                                          &adapter, [this] { ++stops; });
    }
    ~Calls() { QObject::disconnect(startConnection); QObject::disconnect(stopConnection); }
    int starts = 0;
    int stops = 0;
    QMetaObject::Connection startConnection;
    QMetaObject::Connection stopConnection;
};
QAction *namedAction(QMenu *menu, const QString &text)
{
    if (!menu) return nullptr;
    for (auto *action : menu->actions()) if (action->text() == text) return action;
    return nullptr;
}

void typedFixtureBaseline()
{
    NativeControlsWindow host;
    host.streamButton->click();
    host.controls->startStreamAction->trigger();
    host.controls->stopStreamAction->trigger();
    host.controls->forceStopStreamAction->trigger();
    check(host.originalClicks == 1 && host.originalStarts == 1 && host.originalStops == 1 && host.forceStops == 1,
          "fixture begins with four working typed native connections");
    check(host.controls->metaObject()->indexOfSignal("StreamButtonClicked()") >= 0 &&
              host.metaObject()->indexOfMethod("StreamActionTriggered()") >= 0,
          "fixture exposes real meta signatures for QMetaMethod interception");
}

void primaryRoutingAndObservers()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    QObject observer;
    int buttonObservers = 0;
    int controlObservers = 0;
    int menuObservers = 0;
    QObject::connect(host.streamButton, &QPushButton::clicked, &observer, [&] { ++buttonObservers; });
    QObject::connect(host.controls, &OBSBasicControls::StreamButtonClicked, &observer, [&] { ++controlObservers; });
    QObject::connect(host.controls, &OBSBasicControls::StartStreamMenuActionClicked, &observer, [&] { ++menuObservers; });
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Start, true, QStringLiteral("Plugin start"), QStringLiteral("Plugin tip"));
    drain();
    check(host.streamButton->text() == QStringLiteral("Plugin start") && host.streamButton->isEnabled(),
          "start state updates existing native button");
    host.streamButton->click();
    check(calls.starts == 1 && calls.stops == 0 && host.originalClicks == 0,
          "button invokes only plugin start and does not run native handler");
    check(buttonObservers == 1 && controlObservers == 1, "other button and controls receivers remain connected");
    host.controls->startStreamAction->trigger();
    check(calls.starts == 2 && host.originalStarts == 0 && menuObservers == 1,
          "original start action signal is intercepted while other observers survive");
    host.controls->stopStreamAction->trigger();
    check(calls.starts == 2 && calls.stops == 0 && host.originalStops == 0,
          "stale opposite stop menu cannot initiate a plugin start");

    adapter.setState(Action::Stop, true, QStringLiteral("Plugin stop"), {});
    host.streamButton->click();
    host.controls->stopStreamAction->trigger();
    check(calls.stops == 2 && host.originalClicks == 0 && host.originalStops == 0,
          "button and stop action invoke only plugin stop");
    host.controls->startStreamAction->trigger();
    check(calls.starts == 2 && calls.stops == 2 && host.originalStarts == 0,
          "stale opposite start menu cannot initiate a plugin stop");
    adapter.setState(Action::CancelStart, true, QStringLiteral("Cancel preparation"), {});
    host.streamButton->click();
    check(calls.stops == 3 && calls.starts == 2, "preparing cancel routes to stopRequested");

    adapter.setState(Action::Busy, false, QStringLiteral("Busy"), {});
    host.streamButton->click();
    emit host.controls->StreamButtonClicked();
    emit host.controls->StartStreamMenuActionClicked();
    emit host.controls->StopStreamMenuActionClicked();
    check(!host.streamButton->isEnabled() && calls.starts == 2 && calls.stops == 3 &&
              host.originalClicks == 0 && host.originalStarts == 0 && host.originalStops == 0,
          "busy state blocks physical button and all forwarded ordinary signals");
    host.controls->forceStopStreamAction->trigger();
    check(host.forceStops == 1, "force-stop native connection is never disconnected");

    adapter.setState(Action::Start, true, QStringLiteral("Plugin start"), {});
    const auto setBusy = QObject::connect(&adapter, &NativeStreamControls::startRequested, &observer, [&] {
        adapter.setState(Action::Busy, false, QStringLiteral("Starting"), {});
    });
    host.streamButton->click();
    host.streamButton->click();
    host.controls->startStreamAction->trigger();
    check(calls.starts == 3 && host.originalClicks == 0 && host.originalStarts == 0,
          "synchronous controller busy update prevents a second start");
    QObject::disconnect(setBusy);
}

void passThroughAndIdempotentDetach()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    if (!attach(adapter, host)) return;
    adapter.setState(Action::PassThrough, true, QStringLiteral("Must not override native"), QStringLiteral("Unused tip"));
    drain();
    host.streamButton->click();
    host.controls->startStreamAction->trigger();
    host.controls->stopStreamAction->trigger();
    check(host.originalClicks == 1 && host.originalStarts == 1 && host.originalStops == 1 &&
              calls.starts == 0 && calls.stops == 0, "pass-through calls each corresponding native slot once");
    check(host.streamButton->text() == QStringLiteral("Start Streaming"), "pass-through keeps native presentation");

    adapter.setState(Action::Start, true, QStringLiteral("Plugin start"), {});
    QString error;
    check(adapter.attach(&host, &error), "second attach to same host succeeds idempotently");
    host.streamButton->click();
    check(calls.starts == 1 && host.originalClicks == 1, "idempotent attach has only one plugin route");
    adapter.detach();
    adapter.detach();
    drain();
    host.streamButton->click();
    host.controls->startStreamAction->trigger();
    host.controls->stopStreamAction->trigger();
    check(host.originalClicks == 2 && host.originalStarts == 2 && host.originalStops == 2 && calls.starts == 1,
          "repeated detach restores each original route exactly once");
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Start, true, QStringLiteral("Plugin again"), {});
    host.streamButton->click();
    check(calls.starts == 2 && host.originalClicks == 2, "reattach after detach does not accumulate native connections");
    adapter.detach();
    host.streamButton->click();
    check(host.originalClicks == 3, "second lifecycle restores one native connection");
}

void latestNativeStateAndQueuedReapply()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Stop, true, QStringLiteral("Plugin stop"), QStringLiteral("Plugin tooltip"));
    emit host.StreamingPreparing();
    // A root state update after OBS's signal must win over queued reapplication.
    adapter.setState(Action::CancelStart, true, QStringLiteral("Latest cancel"), QStringLiteral("Latest tooltip"));
    drain();
    check(host.streamButton->isEnabled() && host.streamButton->text() == QStringLiteral("Latest cancel") &&
              host.streamButton->toolTip() == QStringLiteral("Latest tooltip"),
          "native widget mutation is overlaid with the newest plugin state");
    adapter.detach();
    drain();
    check(!host.streamButton->isEnabled() && host.streamButton->text() == QStringLiteral("Preparing native stream"),
          "detach restores most recent native state rather than initial idle snapshot");
    check(host.streamButton->toolTip() == QStringLiteral("Native streaming control"),
          "plugin tooltip does not contaminate restored native snapshot");

    emit host.StreamingStopped(false);
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Stop, true, QStringLiteral("Plugin stop"), {});
    emit host.StreamingStarted(false);
    adapter.detach(); // An OBS-state singleShot is pending here.
    drain();
    check(host.streamButton->text() == QStringLiteral("Stop Streaming") && host.streamButton->isEnabled(),
          "pending native-state callback cannot reapply plugin UI after detach");
    host.streamButton->click();
    check(host.originalClicks == 1 && calls.stops == 0, "pending reapply cannot leave plugin click interception behind");
}

void externalNativeHandshake()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Start, true, QStringLiteral("Plugin start"), {});
    emit host.StreamingPreparing();
    drain();
    check(host.streamButton->text() == QStringLiteral("Preparing native stream") && !host.streamButton->isEnabled(),
          "external native preparing takes precedence over plugin ready-start state");
    emit host.StreamingStarting(false);
    drain();
    check(host.streamButton->text() == QStringLiteral("Connecting native stream"),
          "external native handshake retains native connecting text");
    emit host.StreamingStarted(false);
    drain();
    check(host.streamButton->text() == QStringLiteral("Stop Streaming") && host.streamButton->isEnabled(),
          "external native active output keeps native stop control");
    host.streamButton->click();
    check(host.originalClicks == 1 && calls.starts == 0 && calls.stops == 0,
          "external native streaming passes through instead of applying a duplicate Bilibili start");
    emit host.StreamingStopped(false);
    drain();
    check(host.streamButton->text() == QStringLiteral("Plugin start"), "native stopped restores plugin start presentation");
    host.streamButton->click();
    check(calls.starts == 1 && host.originalClicks == 1, "plugin start routing resumes only after native stopped");
}

void delayedMenuAndForceStop()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    QObject observer;
    int extraActions = 0;
    auto *extra = host.controls->streamMenu->addAction(QStringLiteral("Other native menu command"));
    QObject::connect(extra, &QAction::triggered, &observer, [&] { ++extraActions; });
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Stop, true, QStringLiteral("Plugin delayed stop"), {});
    check(host.streamButton->menu() == nullptr, "fixture begins without delay menu attached to button");
    emit host.StreamingStarted(true); // Menu arrives after interception was installed.
    drain();
    auto *proxy = host.streamButton->menu();
    check(proxy && proxy != host.controls->streamMenu, "late native delay menu receives a separate plugin action menu");
    if (!proxy) return;
    check(proxy->actions().contains(host.controls->forceStopStreamAction) && proxy->actions().contains(extra),
          "delay menu preserves original force-stop and unrelated actions");
    auto *stop = namedAction(proxy, QStringLiteral("Plugin delayed stop"));
    check(stop && stop->isEnabled(), "ordinary delay action reflects current plugin stop state");
    if (stop) stop->trigger();
    host.controls->stopStreamAction->trigger();
    check(calls.stops == 2 && host.originalStops == 0, "proxy and original delayed stop signals use plugin route");

    adapter.setState(Action::Busy, false, QStringLiteral("Plugin busy"), {});
    drain();
    proxy = host.streamButton->menu();
    auto *busy = namedAction(proxy, QStringLiteral("Plugin busy"));
    check(busy && !busy->isEnabled(), "busy state disables ordinary delayed action");
    check(host.streamButton->isEnabled(), "delay menu remains reachable for native force-stop while busy");
    if (busy) busy->trigger();
    emit host.controls->StartStreamMenuActionClicked();
    emit host.controls->StopStreamMenuActionClicked();
    check(calls.starts == 0 && calls.stops == 2, "busy direct menu signals cannot bypass ordinary-action gate");
    host.controls->forceStopStreamAction->trigger();
    extra->trigger();
    check(host.forceStops == 1 && extraActions == 1, "force-stop and unrelated menu commands remain operational");
    check(host.controls->startStreamAction->text() == QStringLiteral("Start Streaming") &&
              host.controls->stopStreamAction->text() == QStringLiteral("Stop Streaming"),
          "proxy does not rewrite original menu actions");

    adapter.setState(Action::PassThrough, true, {}, {});
    drain();
    check(host.streamButton->menu() == host.controls->streamMenu, "pass-through restores original delayed menu identity");
    host.controls->stopStreamAction->trigger();
    check(host.originalStops == 1 && calls.stops == 2, "pass-through delayed stop invokes native stop once");
    adapter.detach();
    drain();
    check(host.streamButton->menu() == host.controls->streamMenu, "detach retains most recent native delay menu");
    host.controls->forceStopStreamAction->trigger();
    extra->trigger();
    check(host.forceStops == 2 && extraActions == 2, "shared original actions survive proxy lifecycle");

    if (!attach(adapter, host)) return;
    emit host.StreamingStopped(true);
    adapter.setState(Action::Start, true, QStringLiteral("Plugin delayed start"), {});
    drain();
    check(host.streamButton->menu() == host.controls->streamMenu,
          "delay drain keeps the existing native session controls");
    host.controls->startStreamAction->trigger();
    check(calls.starts == 0 && host.originalStarts == 1,
          "delay drain cannot request a second platform session");
    emit host.StreamingStopped(false);
    drain();
    check(host.streamButton->menu() == nullptr && host.streamButton->text() == QStringLiteral("Plugin delayed start"),
          "terminal output stop restores the managed start button");
    host.streamButton->click();
    check(calls.starts == 1 && host.originalStarts == 1, "new platform start waits for terminal output stop");
    adapter.detach();
}

void failedAttachRollsBack()
{
    NativeControlsWindow host;
    NativeStreamControls adapter;
    Calls calls(adapter);
    QObject observer;
    int observed = 0;
    QObject::connect(host.controls, &OBSBasicControls::StreamButtonClicked, &observer, [&] { ++observed; });
    check(QObject::disconnect(host.controls, &OBSBasicControls::StopStreamMenuActionClicked,
                              &host, &NativeControlsWindow::StopStreaming), "fixture deliberately removes third native route");
    QString error;
    check(!adapter.attach(&host, &error) && !error.isEmpty(), "attach fails visibly when a required native route is missing");
    adapter.setState(Action::Start, true, QStringLiteral("Must not apply"), {});
    drain();
    host.streamButton->click();
    host.controls->startStreamAction->trigger();
    host.controls->stopStreamAction->trigger();
    host.controls->forceStopStreamAction->trigger();
    check(host.originalClicks == 1 && host.originalStarts == 1 && host.originalStops == 0 && host.forceStops == 1,
          "failed attach restores only the routes it actually removed");
    check(calls.starts == 0 && calls.stops == 0 && observed == 1 &&
              host.streamButton->text() == QStringLiteral("Start Streaming"),
          "failed attach leaves UI and unrelated listeners unchanged");
    adapter.detach();
    check(static_cast<bool>(QObject::connect(host.controls, &OBSBasicControls::StopStreamMenuActionClicked,
                           &host, &NativeControlsWindow::StopStreaming)), "fixture repairs missing native route");
    if (!attach(adapter, host)) return;
    adapter.setState(Action::Start, true, QStringLiteral("Plugin repaired"), {});
    host.streamButton->click();
    check(calls.starts == 1 && host.originalClicks == 1 && observed == 2, "attach succeeds after host graph is repaired");
    adapter.detach();
    host.streamButton->click();
    check(host.originalClicks == 2, "rollback and later detach do not duplicate native button handler");

    QWidget unrelated;
    error.clear();
    check(!adapter.attach(&unrelated, &error) && !error.isEmpty(), "unrelated window fails without guessed interception");
}

void destructionSafety()
{
    NativeControlsWindow host;
    {
        auto adapter = std::make_unique<NativeStreamControls>();
        if (!attach(*adapter, host)) return;
        adapter->setState(Action::Stop, true, QStringLiteral("Plugin stop"), {});
        emit host.StreamingPreparing(); // Leave a reapply callback queued.
    }
    drain();
    emit host.StreamingStopped(false);
    host.streamButton->click();
    check(host.originalClicks == 1, "adapter destruction restores native handler and cancels queued work");

    NativeStreamControls survivingAdapter;
    auto temporaryHost = std::make_unique<NativeControlsWindow>();
    if (!attach(survivingAdapter, *temporaryHost)) return;
    survivingAdapter.setState(Action::Stop, true, QStringLiteral("Plugin stop"), {});
    emit temporaryHost->StreamingStarted(true);
    temporaryHost.reset();
    drain();
    survivingAdapter.setState(Action::Busy, false, QStringLiteral("Busy after host destruction"), {});
    survivingAdapter.detach();
    drain();
    check(true, "host may die before adapter and queued menu reapplication");

    auto childHost = std::make_unique<NativeControlsWindow>();
    auto *childAdapter = new NativeStreamControls(childHost.get());
    if (!attach(*childAdapter, *childHost)) return;
    childAdapter->setState(Action::Stop, true, QStringLiteral("Child plugin stop"), {});
    emit childHost->StreamingStarted(true);
    childHost.reset();
    drain();
    check(true, "host-owned adapter tolerates QObject child destruction order");
}
} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    typedFixtureBaseline();
    primaryRoutingAndObservers();
    passThroughAndIdempotentDetach();
    latestNativeStateAndQueuedReapply();
    externalNativeHandshake();
    delayedMenuAndForceStop();
    failedAttachRollsBack();
    destructionSafety();
    if (failures) return 1;
    std::cout << "Native stream controls behavioral tests passed (" << checks << " checks).\n";
    return 0;
}
