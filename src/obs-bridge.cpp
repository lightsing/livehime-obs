#include "obs-bridge.hpp"
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <util/config-file.h>

namespace livehime {
ObsBridge::ObsBridge(QObject *parent) : QObject(parent)
{
    obs_frontend_add_event_callback(frontendEvent, this);
    startupTimer_.setInterval(45000);
    startupTimer_.setSingleShot(true);
    connect(&startupTimer_, &QTimer::timeout, this, [this] {
        if (!connecting_) return;
        if (startCallActive_) {
            emit connectionFailed(QStringLiteral("OBS 正在等待设置或错误提示处理，请先关闭 OBS 的提示窗口。"));
            return;
        }
        // RTMP connects on a worker thread. active() is still false here and
        // cannot prove that the connection has ended. Keep retry blocked.
        emit connectionFailed(QStringLiteral("OBS 尚未连接。请先下播停止本次连接，再重试。"));
    });
    stopRequestTimer_.setSingleShot(true);
    stopRequestTimer_.setInterval(0);
    connect(&stopRequestTimer_, &QTimer::timeout, this, [this] {
        if (!shuttingDown_ && requestedStop_) stop();
    });
    stopCompletionTimer_.setSingleShot(true);
    stopCompletionTimer_.setInterval(0);
    connect(&stopCompletionTimer_, &QTimer::timeout, this, [this] {
        if (!shuttingDown_ && !busy()) notifyStopped(true);
    });
}

ObsBridge::~ObsBridge()
{
    if (!shuttingDown_) obs_frontend_remove_event_callback(frontendEvent, this);
    unwatchOutput();
    restoreService();
    if (previous_) obs_service_release(previous_);
    if (managed_) obs_service_release(managed_);
}

bool ObsBridge::busy() const
{
    if (shuttingDown_) return false;
    if (connecting_ || startCallActive_ || obs_frontend_streaming_active()) return true;
    obs_output_t *output = obs_frontend_get_streaming_output();
    const bool active = output && obs_output_active(output);
    if (output) obs_output_release(output);
    return active;
}

bool ObsBridge::ownsService() const
{
    return !shuttingDown_ && managed_ && obs_frontend_get_streaming_service() == managed_;
}

bool ObsBridge::start(const LiveSession &session, QString *error)
{
    if (shuttingDown_) { *error = QStringLiteral("OBS 正在退出。"); return false; }
    if (busy()) { *error = QStringLiteral("OBS 当前已有推流或连接操作，请先停止。"); return false; }
    if (config_get_bool(obs_frontend_get_profile_config(), "Stream1", "EnableMultitrackVideo")) {
        *error = QStringLiteral("请在 OBS 推流设置中关闭增强广播 / 多轨视频，再连接本场直播。"); return false;
    }
    const char *authType = config_get_string(obs_frontend_get_profile_config(), "Auth", "Type");
    if (authType && QString::fromUtf8(authType).startsWith(QStringLiteral("YouTube"))) {
        *error = QStringLiteral("此 OBS 配置已绑定 YouTube 广播流程，请切换到独立的普通 RTMP 配置后重试。"); return false;
    }
    const QUrl server(session.server);
    if ((server.scheme() != "rtmp" && server.scheme() != "rtmps") || server.host().isEmpty() || session.key.isEmpty()) {
        *error = QStringLiteral("服务器未返回可用的 RTMP 推流信息。"); return false;
    }
    restoreService();
    // The frontend getter returns a borrowed reference; retain it explicitly.
    previous_ = obs_service_get_ref(obs_frontend_get_streaming_service());
    obs_data_t *settings = obs_data_create();
    const QByteArray address = session.server.toUtf8(), key = session.key.toUtf8();
    obs_data_set_string(settings, "server", address.constData());
    obs_data_set_string(settings, "key", key.constData());
    obs_data_set_bool(settings, "use_auth", false);
    managed_ = obs_service_create("rtmp_custom", "livehime-obs-session", settings, nullptr);
    obs_data_release(settings);
    if (!managed_) {
        restoreService();
        *error = QStringLiteral("OBS 无法创建 RTMP 服务，请检查 rtmp-services 插件。"); return false;
    }
    obs_frontend_set_streaming_service(managed_);
    // Do not save session credentials into the user's OBS profile.
    stopRequestTimer_.stop();
    stopCompletionTimer_.stop();
    requestedStop_ = false;
    stopNotified_ = false;
    stopCode_.store(-10000);
    outputAttemptActive_.store(false);
    connecting_ = true;
    startupTimer_.start();
    startCallActive_ = true;
    obs_frontend_streaming_start();
    startCallActive_ = false;
    if (shuttingDown_) {
        *error = QStringLiteral("OBS 正在退出。");
        return false;
    }
    if (requestedStop_) {
        stop();
    }
    return true;
}

void ObsBridge::stop()
{
    if (!ownsService()) return;
    requestedStop_ = true;
    // A synchronous OBS start can enter a modal dialog and process a nested
    // event loop. Latch stop until setup returns rather than stopping early.
    if (startCallActive_) return;
    stopRequestTimer_.stop();
    startupTimer_.stop();
    watchOutput();
    obs_output_t *output = obs_frontend_get_streaming_output();
    const bool cancelConnection = connecting_ && output && outputAttemptActive_.load() && !obs_output_active(output);
    obs_frontend_streaming_stop();
    // OBS's frontend stop checks obs_output_active(), which excludes the first
    // RTMP handshake. force_stop is the public API without that active guard.
    // Its RTMP implementation waits for the connect worker before stopping.
    if (!shuttingDown_ && cancelConnection && outputAttemptActive_.load())
        obs_output_force_stop(output);
    if (!shuttingDown_ && !outputAttemptActive_.load() && (!output || !obs_output_active(output))) {
        // No output "starting" signal ever occurred, or a low-level "stop"
        // already confirmed termination. This also covers setup rejection.
        connecting_ = false;
        if (!stopNotified_) stopCompletionTimer_.start();
    }
    if (output) obs_output_release(output);
}

void ObsBridge::restoreService()
{
    if (busy()) { restorePending_ = true; return; }
    restorePending_ = false;
    unwatchOutput();
    if (previous_ && ownsService()) obs_frontend_set_streaming_service(previous_);
    if (previous_) { obs_service_release(previous_); previous_ = nullptr; }
    if (managed_) { obs_service_release(managed_); managed_ = nullptr; }
}

void ObsBridge::frontendEvent(obs_frontend_event event, void *data)
{
    auto *self = static_cast<ObsBridge *>(data);
    if (QThread::currentThread() == self->thread()) self->onFrontendEvent(event);
    else QMetaObject::invokeMethod(self, [self, event] { self->onFrontendEvent(event); }, Qt::QueuedConnection);
}

void ObsBridge::onFrontendEvent(obs_frontend_event event)
{
    if (shuttingDown_) return;
    if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
        event == OBS_FRONTEND_EVENT_STREAMING_STOPPED || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
        emit availabilityChanged();
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        // OBS invalidates its frontend callbacks and destroys outputHandler
        // immediately after EXIT. Release references while libobs is alive.
        shuttingDown_ = true;
        startupTimer_.stop();
        stopRequestTimer_.stop();
        stopCompletionTimer_.stop();
        restorePending_ = false;
        unwatchOutput();
        if (previous_) { obs_service_release(previous_); previous_ = nullptr; }
        if (managed_) { obs_service_release(managed_); managed_ = nullptr; }
        emit shuttingDown();
        return;
    }
    if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGING) { restoreService(); return; }
    if (!ownsService()) return;
    switch (event) {
    case OBS_FRONTEND_EVENT_STREAMING_STARTING:
        connecting_ = true;
        watchOutput();
        if (requestedStop_) {
            // The native Start button can be clicked while the server-side
            // stop request is in flight. Keep this scene stopped until an
            // explicit bridge.start() establishes a new accepted attempt.
            stopRequestTimer_.start();
            break;
        }
        stopCompletionTimer_.stop();
        stopNotified_ = false;
        stopCode_.store(-10000);
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
        connecting_ = false;
        startupTimer_.stop();
        watchOutput();
        outputAttemptActive_.store(true);
        if (requestedStop_) { stopRequestTimer_.start(); break; }
        emit started();
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
        outputAttemptActive_.store(false);
        connecting_ = false;
        startupTimer_.stop();
        notifyStopped(requestedStop_ || stopCode_.load() == OBS_OUTPUT_SUCCESS);
        break;
    default: break;
    }
}

void ObsBridge::notifyStopped(bool intentional)
{
    if (shuttingDown_) return;
    stopRequestTimer_.stop();
    stopCompletionTimer_.stop();
    if (intentional) requestedStop_ = true;
    QPointer<ObsBridge> guard(this);
    if (!stopNotified_) {
        stopNotified_ = true;
        emit stopped(intentional);
    }
    // A rejected native restart may still have been stopping when the remote
    // stop completed. Finish the previously requested restoration now.
    if (guard && restorePending_ && !busy()) restoreService();
}

void ObsBridge::watchOutput()
{
    obs_output_t *current = obs_frontend_get_streaming_output();
    if (current == output_) { if (current) obs_output_release(current); return; }
    unwatchOutput();
    output_ = current;
    outputAttemptActive_.store(false);
    if (!output_) return;
    signal_handler_t *handler = obs_output_get_signal_handler(output_);
    signal_handler_connect(handler, "starting", outputStarting, this);
    signal_handler_connect(handler, "stop", outputStopped, this);
    signal_handler_connect(handler, "reconnect", outputReconnecting, this);
    signal_handler_connect(handler, "reconnect_success", outputReconnected, this);
}

void ObsBridge::unwatchOutput()
{
    if (!output_) return;
    signal_handler_t *handler = obs_output_get_signal_handler(output_);
    signal_handler_disconnect(handler, "starting", outputStarting, this);
    signal_handler_disconnect(handler, "stop", outputStopped, this);
    signal_handler_disconnect(handler, "reconnect", outputReconnecting, this);
    signal_handler_disconnect(handler, "reconnect_success", outputReconnected, this);
    obs_output_release(output_);
    output_ = nullptr;
}
void ObsBridge::outputStarting(void *data, calldata_t *)
{ static_cast<ObsBridge *>(data)->outputAttemptActive_.store(true); }
void ObsBridge::outputStopped(void *data, calldata_t *params)
{
    auto *self = static_cast<ObsBridge *>(data);
    self->stopCode_.store(static_cast<int>(calldata_int(params, "code")));
    self->outputAttemptActive_.store(false);
}
void ObsBridge::outputReconnecting(void *data, calldata_t *)
{
    auto *self = static_cast<ObsBridge *>(data);
    QMetaObject::invokeMethod(self, [self] {
        if (!self->shuttingDown_ && !self->requestedStop_) emit self->reconnecting();
    }, Qt::QueuedConnection);
}
void ObsBridge::outputReconnected(void *data, calldata_t *)
{
    auto *self = static_cast<ObsBridge *>(data);
    QMetaObject::invokeMethod(self, [self] {
        if (!self->shuttingDown_ && !self->requestedStop_) emit self->reconnected();
    }, Qt::QueuedConnection);
}
}
