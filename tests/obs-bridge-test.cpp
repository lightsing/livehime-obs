// Exercise the production ObsBridge against an in-process OBS C API double.
// This executable links no OBS DLL and never reads an OBS profile or account.
#include "obs-bridge.hpp"
#include <util/config-file.h>

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct obs_data {
    std::map<std::string, std::string> strings;
    std::map<std::string, bool> booleans;
};
struct obs_service {
    int references = 1;
    std::string id;
    obs_data settings;
};
struct SignalSubscription {
    std::string name;
    signal_callback_t callback = nullptr;
    void *data = nullptr;
};
struct signal_handler {
    std::vector<SignalSubscription> subscriptions;
};
struct obs_output {
    int references = 1; // The simulated frontend's owning reference.
    bool active = false;
    signal_handler handler;
};
struct config_data {
    bool multitrack = false;
    std::string authType;
};

namespace {
using livehime::LiveSession;
using livehime::ObsBridge;
int failures = 0;

void check(bool value, const char *message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void drain()
{
    for (int i = 0; i < 12; ++i)
        QCoreApplication::processEvents();
}

struct FrontendSubscription {
    obs_frontend_event_cb callback = nullptr;
    void *data = nullptr;
};

struct FakeObs {
    config_data config;
    obs_output output;
    std::vector<std::unique_ptr<obs_service>> services;
    obs_service *current = nullptr;
    obs_service *original = nullptr;
    std::vector<FrontendSubscription> callbacks;
    std::function<void()> startAction;
    QStringList order;
    int starts = 0;
    int stops = 0;
    int forceStops = 0;
    int settingsAlive = 0;
    int frontendCallsAfterExit = 0;
    bool frontendUsable = true;
    bool streaming = false;
    bool createFails = false;
    bool synchronousStop = false;
    bool synchronousForceStop = false;

    FakeObs();
    ~FakeObs();
    obs_service *makeService(const char *id, const obs_data &settings = {})
    {
        auto service = std::make_unique<obs_service>();
        service->id = id;
        service->settings = settings;
        auto *pointer = service.get();
        services.push_back(std::move(service));
        return pointer;
    }
    void frontend(const char *call)
    {
        order.push_back(QString::fromLatin1(call));
        if (!frontendUsable)
            ++frontendCallsAfterExit;
    }
    void event(obs_frontend_event type)
    {
        const auto snapshot = callbacks;
        for (const auto &subscription : snapshot)
            subscription.callback(type, subscription.data);
    }
    void outputSignal(const char *name, int code = 0)
    {
        const auto snapshot = output.handler.subscriptions;
        long long signalCode = code;
        calldata_t payload{};
        payload.stack = reinterpret_cast<uint8_t *>(&signalCode);
        payload.size = sizeof(signalCode);
        for (const auto &subscription : snapshot) {
            if (subscription.name == name)
                subscription.callback(subscription.data, &payload);
        }
    }
    void becomeLive()
    {
        event(OBS_FRONTEND_EVENT_STREAMING_STARTING);
        outputSignal("starting");
        output.active = true;
        streaming = true;
        event(OBS_FRONTEND_EVENT_STREAMING_STARTED);
    }
    void beginConnecting()
    {
        event(OBS_FRONTEND_EVENT_STREAMING_STARTING);
        outputSignal("starting"); // obs_output_start accepted, RTMP thread started.
        output.active = false; // Data capture begins only after the handshake.
        streaming = false;
    }
    void completeConnection()
    {
        output.active = true;
        streaming = true;
        event(OBS_FRONTEND_EVENT_STREAMING_STARTED);
    }
    void finishStop(int code)
    {
        output.active = false;
        streaming = false;
        outputSignal("stop", code);
        event(OBS_FRONTEND_EVENT_STREAMING_STOPPED);
    }
    void exit()
    {
        event(OBS_FRONTEND_EVENT_EXIT);
        frontendUsable = false;
        callbacks.clear(); // OBS invalidates the frontend API after EXIT.
    }
};
FakeObs *fake = nullptr;

FakeObs::FakeObs()
{
    check(fake == nullptr, "only one simulated OBS instance");
    fake = this;
    original = makeService("rtmp_common");
    original->settings.strings["server"] = "rtmp://original.invalid/app";
    original->settings.strings["key"] = "original-synthetic-key";
    current = original;
    startAction = [this] { becomeLive(); };
}

FakeObs::~FakeObs()
{
    check(output.references == 1, "bridge releases every output reference");
    check(output.handler.subscriptions.empty(), "bridge disconnects all output signals");
    check(callbacks.empty(), "bridge removes frontend callback before destruction");
    check(settingsAlive == 0, "bridge releases temporary service settings");
    if (current)
        obs_service_release(current);
    for (const auto &service : services)
        check(service->references == 0, "service references balanced after frontend teardown");
    fake = nullptr;
}

LiveSession session()
{
    return {789, QStringLiteral("rtmps://synthetic.invalid/live"), QStringLiteral("synthetic-session-key")};
}

void normalStartAndRestore()
{
    FakeObs fixture;
    {
        ObsBridge bridge;
        QString error;
        int started = 0, reconnecting = 0, reconnected = 0;
        std::vector<bool> stopped;
        QObject::connect(&bridge, &ObsBridge::started, [&] { ++started; });
        QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) { stopped.push_back(intentional); });
        QObject::connect(&bridge, &ObsBridge::reconnecting, [&] { ++reconnecting; });
        QObject::connect(&bridge, &ObsBridge::reconnected, [&] { ++reconnected; });
        check(bridge.start(session(), &error) && error.isEmpty(), "normal start accepted");
        auto *temporary = fixture.current;
        check(started == 1 && fixture.starts == 1 && bridge.busy(), "normal startup reaches active output once");
        check(temporary != fixture.original && temporary->id == "rtmp_custom", "temporary custom service selected");
        check(temporary->settings.strings["server"] == "rtmps://synthetic.invalid/live" &&
                  temporary->settings.strings["key"] == "synthetic-session-key" &&
                  !temporary->settings.booleans["use_auth"], "session fields applied to temporary service");
        check(fixture.original->references == 1 && temporary->references == 2,
              "previous service retained and frontend retains temporary service");
        check(fixture.original->settings.strings["key"] == "original-synthetic-key",
              "previous service settings remain intact");
        bridge.restoreService();
        check(fixture.current == temporary, "active output prevents service replacement");
        fixture.outputSignal("reconnect");
        fixture.outputSignal("reconnect_success");
        drain();
        check(reconnecting == 1 && reconnected == 1 && fixture.starts == 1,
              "reconnect signals do not create a second output session");
        bridge.stop();
        check(fixture.stops == 1 && stopped.empty(), "stop waits for output completion");
        fixture.finishStop(OBS_OUTPUT_SUCCESS);
        drain();
        check(stopped.size() == 1 && stopped[0], "requested output stop is intentional");
        bridge.restoreService();
        check(fixture.current == fixture.original && fixture.original->references == 1 && temporary->references == 0,
              "restore returns original service and releases temporary references");
        check(fixture.output.references == 1 && fixture.output.handler.subscriptions.empty(),
              "restore releases output observation");
    }
}

void setupAndCreationFailure()
{
    {
        FakeObs fixture;
        ObsBridge bridge;
        fixture.startAction = [] {}; // Setup rejects before any public STARTING event.
        int failed = 0, stopped = 0;
        QObject::connect(&bridge, &ObsBridge::connectionFailed, [&](const QString &) { ++failed; });
        QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) {
            check(intentional, "setup rejection can be intentionally stopped");
            ++stopped;
        });
        QString error;
        check(bridge.start(session(), &error), "setup failure still represents an accepted start attempt");
        check(failed == 0 && bridge.busy(), "inactive setup remains pending until terminal evidence or explicit stop");
        auto *temporary = fixture.current;
        bridge.stop();
        drain();
        check(stopped == 1, "setup rejection without STOPPED event uses one fallback completion");
        bridge.restoreService();
        check(fixture.current == fixture.original && temporary->references == 0,
              "setup failure can restore the previous service");
    }
    {
        FakeObs fixture;
        ObsBridge bridge;
        fixture.createFails = true;
        QString error;
        check(!bridge.start(session(), &error) && !error.isEmpty(), "service creation failure returned");
        check(fixture.starts == 0 && fixture.current == fixture.original && fixture.original->references == 1,
              "service creation failure leaves frontend and references unchanged");
    }
}

void delayedConnectionAndCancellation()
{
    {
        FakeObs fixture;
        ObsBridge bridge;
        fixture.startAction = [&] { fixture.beginConnecting(); };
        int failed = 0, started = 0;
        std::vector<bool> stopped;
        QObject::connect(&bridge, &ObsBridge::connectionFailed, [&](const QString &) { ++failed; });
        QObject::connect(&bridge, &ObsBridge::started, [&] { ++started; });
        QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) { stopped.push_back(intentional); });
        QString error;
        check(bridge.start(session(), &error), "asynchronous RTMP connection accepted");
        check(bridge.busy() && failed == 0 && started == 0 && !fixture.output.active,
              "inactive output during RTMP handshake remains Connecting, not failure or idle");
        check(!bridge.start(session(), &error) && fixture.starts == 1,
              "connecting state prevents a second output attempt");
        bridge.stop();
        drain();
        check(fixture.forceStops == 1 && bridge.busy() && stopped.empty(),
              "connecting cancel uses force_stop and waits for terminal evidence");
        fixture.finishStop(OBS_OUTPUT_SUCCESS);
        drain();
        check(stopped.size() == 1 && stopped[0] && !bridge.busy(),
              "cancelled handshake completes exactly once after actual STOPPED");
        bridge.restoreService();
    }
    {
        FakeObs fixture;
        ObsBridge bridge;
        fixture.startAction = [&] { fixture.beginConnecting(); };
        int failed = 0, started = 0;
        QObject::connect(&bridge, &ObsBridge::connectionFailed, [&](const QString &) { ++failed; });
        QObject::connect(&bridge, &ObsBridge::started, [&] { ++started; });
        QString error;
        bridge.start(session(), &error);
        fixture.completeConnection();
        check(started == 1 && failed == 0 && bridge.busy(), "delayed handshake becomes Live normally");
        fixture.finishStop(OBS_OUTPUT_SUCCESS);
        bridge.restoreService();
    }
}

void nestedStopDuringStart()
{
    FakeObs fixture;
    ObsBridge bridge;
    std::vector<bool> stopped;
    QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) { stopped.push_back(intentional); });
    fixture.startAction = [&] {
        fixture.order.push_back(QStringLiteral("setup.enter"));
        bridge.stop(); // Simulate a click dispatched by OBS's modal nested event loop.
        drain();
        check(fixture.stops == 0 && stopped.empty() && bridge.busy(),
              "nested stop is latched until synchronous setup returns");
        fixture.becomeLive();
        fixture.order.push_back(QStringLiteral("setup.return"));
    };
    QString error;
    check(bridge.start(session(), &error), "nested-stop start attempt accepted");
    check(fixture.stops == 1 && fixture.order.indexOf(QStringLiteral("frontend.stop")) >
                                   fixture.order.indexOf(QStringLiteral("setup.return")),
          "latched stop reaches frontend only after setup returns");
    check(stopped.empty(), "latched stop still waits for media completion");
    fixture.finishStop(OBS_OUTPUT_SUCCESS);
    drain();
    check(stopped.size() == 1 && stopped[0], "nested stop completes intentionally once");
    bridge.restoreService();
}

void outputFailureAndNativeStop()
{
    FakeObs fixture;
    ObsBridge bridge;
    QString error;
    std::vector<bool> stopped;
    QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) { stopped.push_back(intentional); });
    bridge.start(session(), &error);
    fixture.finishStop(OBS_OUTPUT_DISCONNECTED);
    check(stopped.size() == 1 && !stopped.back(), "network output failure is recoverable, not intentional");
    check(bridge.ownsService() && !bridge.busy(), "failed output keeps current session available for retry");
    bridge.start(session(), &error);
    fixture.finishStop(OBS_OUTPUT_SUCCESS); // User clicked the native OBS Stop button.
    check(stopped.size() == 2 && stopped.back(), "native OBS user stop is intentional");
    bridge.restoreService();
}

void synchronousStopCompletesOnce()
{
    FakeObs fixture;
    ObsBridge bridge;
    fixture.synchronousStop = true;
    int stopped = 0;
    QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) {
        check(intentional, "synchronous requested stop is intentional");
        ++stopped;
    });
    QString error;
    bridge.start(session(), &error);
    bridge.stop();
    drain();
    check(stopped == 1, "synchronous STOPPED event and fallback must not emit twice");
    bridge.restoreService();
}

void stopLatchRejectsNativeRestart()
{
    FakeObs fixture;
    ObsBridge bridge;
    QString error;
    int started = 0, reconnected = 0;
    std::vector<bool> stopped;
    QObject::connect(&bridge, &ObsBridge::started, [&] { ++started; });
    QObject::connect(&bridge, &ObsBridge::reconnected, [&] { ++reconnected; });
    QObject::connect(&bridge, &ObsBridge::stopped, [&](bool intentional) { stopped.push_back(intentional); });
    bridge.start(session(), &error);
    auto *temporary = fixture.current;
    bridge.stop();
    fixture.finishStop(OBS_OUTPUT_SUCCESS);
    check(stopped.size() == 1 && stopped[0], "initial stop starts remote stop workflow once");

    // The remote stop HTTP response has not returned, but native OBS controls
    // already allow another Start. It must not resurrect the same live scene.
    fixture.becomeLive();
    fixture.outputSignal("reconnect_success");
    drain();
    check(started == 1 && reconnected == 0, "rejected native restart cannot report Live or Reconnected");
    check(fixture.stops == 2 && bridge.busy(), "native restart is stopped after its setup completes");
    bridge.restoreService(); // Remote stop succeeded while media is still stopping.
    check(fixture.current == temporary, "restore waits until rejected restart actually stops");
    fixture.finishStop(OBS_OUTPUT_SUCCESS);
    drain();
    check(stopped.size() == 1, "rejected native restart cannot trigger a duplicate remote stop");
    check(fixture.current == fixture.original && temporary->references == 0,
          "pending restoration completes after rejected native restart stops");

    check(bridge.start(session(), &error) && started == 2, "explicit new start clears intentional-stop latch");
    fixture.finishStop(OBS_OUTPUT_DISCONNECTED);
    check(stopped.size() == 2 && !stopped.back(), "new scene output failure remains recoverable");
    fixture.becomeLive();
    drain();
    check(started == 3 && fixture.stops == 2, "native retry after unexpected failure remains allowed");
    fixture.finishStop(OBS_OUTPUT_SUCCESS);
    bridge.restoreService();
}

void stopAfterDisconnectAlsoLatches()
{
    FakeObs fixture;
    ObsBridge bridge;
    QString error;
    int started = 0, stopped = 0;
    QObject::connect(&bridge, &ObsBridge::started, [&] { ++started; });
    QObject::connect(&bridge, &ObsBridge::stopped, [&](bool) { ++stopped; });
    bridge.start(session(), &error);
    fixture.finishStop(OBS_OUTPUT_DISCONNECTED);
    check(!bridge.busy() && stopped == 1, "disconnected media has already notified controller");
    bridge.stop(); // Controller must do this even when outputBusy is false.
    drain();
    check(stopped == 1, "intentional stop after disconnect does not duplicate prior terminal event");
    fixture.becomeLive();
    drain();
    check(started == 1 && fixture.stops == 2, "stop after disconnect still blocks native scene resurrection");
    fixture.finishStop(OBS_OUTPUT_SUCCESS);
    bridge.restoreService();
}

void exitInvalidatesFrontend()
{
    FakeObs fixture;
    {
        ObsBridge bridge;
        QString error;
        int shutdowns = 0;
        QObject::connect(&bridge, &ObsBridge::shuttingDown, [&] { ++shutdowns; });
        bridge.start(session(), &error);
        fixture.exit();
        check(shutdowns == 1 && !bridge.busy() && !bridge.ownsService(), "EXIT disables frontend-dependent bridge state");
        bridge.stop();
        bridge.restoreService();
        check(!bridge.start(session(), &error) && !error.isEmpty(), "start rejected after EXIT");
        drain();
        check(fixture.frontendCallsAfterExit == 0, "no frontend calls after EXIT while object remains alive");
    }
    check(fixture.frontendCallsAfterExit == 0, "destructor does not touch invalidated frontend");
}

void exitDuringNestedStart()
{
    FakeObs fixture;
    {
        ObsBridge bridge;
        fixture.startAction = [&] { fixture.exit(); };
        QString error;
        bridge.start(session(), &error);
        check(fixture.frontendCallsAfterExit == 0,
              "start does not probe output after EXIT dispatched by a nested event loop");
    }
    check(fixture.frontendCallsAfterExit == 0, "nested EXIT leaves destructor safe");
}
} // namespace

// The following functions implement only public API contracts consumed by the
// production bridge; no production bridge method is replaced by the test.
extern "C" {
void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *data)
{
    fake->frontend("frontend.subscribe");
    fake->callbacks.push_back({callback, data});
}
void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *data)
{
    fake->frontend("frontend.unsubscribe");
    std::erase_if(fake->callbacks, [&](const auto &item) { return item.callback == callback && item.data == data; });
}
bool obs_frontend_streaming_active(void)
{
    fake->frontend("frontend.active");
    return fake->streaming;
}
obs_output_t *obs_frontend_get_streaming_output(void)
{
    fake->frontend("frontend.output");
    ++fake->output.references;
    return &fake->output;
}
obs_service_t *obs_frontend_get_streaming_service(void)
{
    fake->frontend("frontend.service");
    return fake->current; // Borrowed, as documented by OBS's frontend wrapper.
}
config_t *obs_frontend_get_profile_config(void)
{
    fake->frontend("frontend.profile");
    return &fake->config;
}
void obs_frontend_set_streaming_service(obs_service_t *service)
{
    fake->frontend("frontend.set-service");
    if (!service)
        return;
    auto *old = fake->current;
    fake->current = obs_service_get_ref(service);
    obs_service_release(old);
}
void obs_frontend_streaming_start(void)
{
    fake->frontend("frontend.start");
    ++fake->starts;
    fake->startAction();
}
void obs_frontend_streaming_stop(void)
{
    fake->frontend("frontend.stop");
    ++fake->stops;
    if (fake->synchronousStop && fake->output.active)
        fake->finishStop(OBS_OUTPUT_SUCCESS);
}
obs_service_t *obs_service_get_ref(obs_service_t *service)
{
    if (service) {
        check(service->references > 0, "cannot retain a released service");
        ++service->references;
    }
    return service;
}
obs_service_t *obs_service_create(const char *id, const char *, obs_data_t *settings, obs_data_t *)
{
    return fake->createFails ? nullptr : fake->makeService(id, *settings);
}
void obs_service_release(obs_service_t *service)
{
    if (service) {
        check(service->references > 0, "service is not released twice");
        --service->references;
    }
}
obs_data_t *obs_data_create(void)
{
    ++fake->settingsAlive;
    return new obs_data;
}
void obs_data_set_string(obs_data_t *data, const char *name, const char *value)
{
    data->strings[name] = value;
}
void obs_data_set_bool(obs_data_t *data, const char *name, bool value)
{
    data->booleans[name] = value;
}
void obs_data_release(obs_data_t *data)
{
    --fake->settingsAlive;
    delete data;
}
bool config_get_bool(config_t *config, const char *section, const char *name)
{
    return std::strcmp(section, "Stream1") == 0 && std::strcmp(name, "EnableMultitrackVideo") == 0 && config->multitrack;
}
const char *config_get_string(config_t *config, const char *section, const char *name)
{
    return std::strcmp(section, "Auth") == 0 && std::strcmp(name, "Type") == 0 ? config->authType.c_str() : "";
}
bool obs_output_active(const obs_output_t *output)
{
    return output && output->active;
}
void obs_output_release(obs_output_t *output)
{
    if (output) {
        check(output->references > 1, "bridge cannot release frontend's output owner reference");
        --output->references;
    }
}
void obs_output_force_stop(obs_output_t *output)
{
    check(output == &fake->output, "force stop targets only the managed output");
    fake->order.push_back(QStringLiteral("output.force-stop"));
    ++fake->forceStops;
    if (fake->synchronousForceStop)
        fake->finishStop(OBS_OUTPUT_SUCCESS);
}
signal_handler_t *obs_output_get_signal_handler(const obs_output_t *output)
{
    return const_cast<signal_handler_t *>(&output->handler);
}
void signal_handler_connect(signal_handler_t *handler, const char *name, signal_callback_t callback, void *data)
{
    handler->subscriptions.push_back({name, callback, data});
}
void signal_handler_disconnect(signal_handler_t *handler, const char *name, signal_callback_t callback, void *data)
{
    std::erase_if(handler->subscriptions, [&](const auto &item) {
        return item.name == name && item.callback == callback && item.data == data;
    });
}
bool calldata_get_data(const calldata_t *data, const char *name, void *out, size_t size)
{
    if (std::strcmp(name, "code") != 0 || size != sizeof(long long) || !data || data->size != sizeof(long long))
        return false;
    std::memcpy(out, data->stack, size);
    return true;
}
} // extern "C"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    normalStartAndRestore();
    setupAndCreationFailure();
    delayedConnectionAndCancellation();
    nestedStopDuringStart();
    outputFailureAndNativeStop();
    synchronousStopCompletesOnce();
    stopLatchRejectsNativeRestart();
    stopAfterDisconnectAlsoLatches();
    exitInvalidatesFrontend();
    exitDuringNestedStart();
    if (failures) {
        std::cerr << failures << " OBS bridge checks failed\n";
        return 1;
    }
    std::cout << "OBS bridge offline checks passed\n";
    return 0;
}
