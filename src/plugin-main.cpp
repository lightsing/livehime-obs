#include "room-dock.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <obs-module.h>
#include <util/bmem.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("livehime-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{ return "Bilibili room management and direct OBS streaming for Windows"; }
MODULE_EXPORT const char *obs_module_name(void) { return "Livehime OBS"; }
MODULE_EXPORT const char *obs_module_author(void) { return "lightsing"; }

namespace {
QPointer<livehime::RoomDock> dock;
QPointer<QWidget> dockContainer;
QPointer<QLabel> loadingLabel;
bool waitingForFrontend = false;
void createRoomDock();
void frontendReady(obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        waitingForFrontend = false; // Frontend APIs are invalid after this event.
        return;
    }
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || !waitingForFrontend) return;
    waitingForFrontend = false;
    obs_frontend_remove_event_callback(frontendReady, nullptr);
    createRoomDock();
}
}

bool obs_module_load(void)
{
    if (QString::fromLatin1(qVersion()) != QStringLiteral(QT_VERSION_STR)) {
        blog(LOG_ERROR, "[livehime-obs] Qt runtime does not match the bundled TLS backend");
        return false;
    }
    // OBS does not ship a Qt TLS backend. Register our matching private
    // Schannel plugin before the dock creates any network manager.
    char *pluginPath = obs_module_file("qt-plugins");
    if (!pluginPath) {
        blog(LOG_ERROR, "[livehime-obs] Private Qt TLS plugins directory is missing");
        return false;
    }
    QCoreApplication::addLibraryPath(QString::fromUtf8(pluginPath));
    bfree(pluginPath);
    return true;
}

void obs_module_post_load(void)
{
    // post_load precedes OBS's ResetOutputs(). Neither room initialization nor
    // native button state may query the output handler until the frontend is ready.
    // Register only the empty shell here so OBS can restore its saved dock layout.
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!window) return;
    dockContainer = new QWidget(window);
    auto *layout = new QVBoxLayout(dockContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    loadingLabel = new QLabel(QStringLiteral("正在加载 B 站直播…"), dockContainer);
    loadingLabel->setWordWrap(true);
    layout->addWidget(loadingLabel);
    if (!obs_frontend_add_dock_by_id("livehime-obs-room", "B站直播", dockContainer)) {
        delete dockContainer.data();
        return;
    }
    waitingForFrontend = true;
    obs_frontend_add_event_callback(frontendReady, nullptr);
}

namespace {
void createRoomDock()
{
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!window || !dockContainer) return;
    char *configPath = obs_module_config_path("");
    if (!configPath) {
        if (loadingLabel) loadingLabel->setText(QStringLiteral("无法读取插件配置目录。"));
        return;
    }
    const QString storage = QString::fromUtf8(configPath);
    bfree(configPath);
    if (!QDir().mkpath(storage)) {
        blog(LOG_ERROR, "[livehime-obs] Cannot create plugin storage directory");
        if (loadingLabel) loadingLabel->setText(QStringLiteral("无法创建插件配置目录。"));
        return;
    }
    dock = new livehime::RoomDock(storage, dockContainer);
    QString controlsError;
    if (!dock->attachNativeControls(window, &controlsError)) {
        blog(LOG_ERROR, "[livehime-obs] Cannot attach native streaming controls: %s", controlsError.toUtf8().constData());
        if (loadingLabel) loadingLabel->setText(controlsError);
        delete dock.data();
        return;
    }
    delete loadingLabel.data();
    dockContainer->layout()->addWidget(dock);
    blog(LOG_INFO, "[livehime-obs] Loaded version %s (protocol profile 7.64.0.10819); native streaming controls attached", PLUGIN_VERSION);
}
}

void obs_module_unload(void)
{
    if (waitingForFrontend) {
        obs_frontend_remove_event_callback(frontendReady, nullptr);
        waitingForFrontend = false;
    }
    if (dockContainer) {
        obs_frontend_remove_dock("livehime-obs-room");
        delete dockContainer.data();
    }
}
