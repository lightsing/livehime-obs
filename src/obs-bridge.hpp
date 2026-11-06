#pragma once

#include "protocol.hpp"
#include <QObject>
#include <QTimer>
#include <atomic>
#include <obs-frontend-api.h>

namespace livehime {
class ObsBridge final : public QObject {
    Q_OBJECT
public:
    explicit ObsBridge(QObject *parent = nullptr);
    ~ObsBridge() override;
    bool busy() const;
    bool ownsService() const;
    bool start(const LiveSession &session, QString *error);
    void stop();
    void restoreService();
signals:
    void availabilityChanged();
    void started();
    void stopped(bool intentional);
    void connectionFailed(const QString &message);
    void reconnecting();
    void reconnected();
    void shuttingDown();
private:
    static void frontendEvent(obs_frontend_event event, void *data);
    static void outputStarting(void *data, calldata_t *params);
    static void outputStopped(void *data, calldata_t *params);
    static void outputReconnecting(void *data, calldata_t *params);
    static void outputReconnected(void *data, calldata_t *params);
    void onFrontendEvent(obs_frontend_event event);
    void watchOutput();
    void unwatchOutput();
    void notifyStopped(bool intentional);
    obs_service_t *previous_ = nullptr;
    obs_service_t *managed_ = nullptr;
    obs_output_t *output_ = nullptr;
    std::atomic<int> stopCode_{-10000};
    std::atomic<bool> outputAttemptActive_{false};
    bool requestedStop_ = false;
    bool connecting_ = false;
    bool startCallActive_ = false;
    bool shuttingDown_ = false;
    bool stopNotified_ = true;
    bool restorePending_ = false;
    QTimer startupTimer_;
    QTimer stopRequestTimer_;
    QTimer stopCompletionTimer_;
};
}
