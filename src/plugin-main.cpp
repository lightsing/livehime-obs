#include "beta-diagnostics.hpp"
#include "error-reports.hpp"
#include "classic-pk-window.hpp"
#include "dock-focus.hpp"
#include "floating-windows.hpp"
#include "moderation-alerts.hpp"
#include "protocol.hpp"
#include "room-dock.hpp"
#include "room-realtime.hpp"
#include "update-controller.hpp"
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QMenu>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <obs-module.h>
#include <util/bmem.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("livehime-obs", "en-US")
MODULE_EXPORT const char *obs_module_description(void) {
    return "Bilibili room management and direct OBS streaming for Windows";
}
MODULE_EXPORT const char *obs_module_name(void) { return "Livehime OBS"; }
MODULE_EXPORT const char *livehime_obs_version(void) { return LIVEHIME_OBS_VERSION; }
MODULE_EXPORT const char *obs_module_author(void) { return "Livehime OBS contributors"; }

namespace {
using livehime::FloatingWindows;
using livehime::PluginIssue;
QPointer<livehime::RoomDock> dock;
QPointer<QWidget> dockContainer;
QPointer<QLabel> loadingLabel;
QPointer<livehime::LiveController> controller;
QPointer<livehime::RoomRealtime> realtime;
QPointer<FloatingWindows> windows;
QPointer<livehime::UpdateController> updates;
QPointer<livehime::ErrorReports> errors;
QPointer<QAction> pkAction;
QPointer<livehime::ClassicPkWindow> pkWindow;
std::array<QPointer<QDockWidget>, FloatingWindows::DockCount> realtimeDocks;
bool waitingForFrontend = false;
bool frontendValid = true;
void createRoomDock();
void openClassicPk(QWidget *window) {
    using namespace livehime;
    if (!controller || !realtime || !realtime->classicPkUnlocked()) return;
    if (!pkWindow) {
        auto *pk = new ClassicPkController([](ClassicPkOp op, qint64 uid, qint64 room, qint64 id,
                                              bool continuous, Callback<QJsonObject> done) {
            controller->client()->classicPk(op, uid, room, id, continuous, std::move(done));
        });
        pkWindow = new ClassicPkWindow(pk, window);
        pk->setParent(pkWindow);
        QObject::connect(pk, &ClassicPkController::problem, errors, &ErrorReports::report);
        auto sync = [pk] {
            const bool valid = controller && controller->authenticated() && controller->roomMatchesAccount();
            const auto room = valid && controller->hasRemoteSession() ? controller->room().roomId : 0;
            pk->setContext(controller ? controller->accountEpoch() : 0, valid ? controller->user().uid : 0,
                           room, room && controller->state() != LiveController::State::Stopping);
        };
        QObject::connect(controller, &LiveController::changed, pk, sync);
        QObject::connect(controller, &LiveController::roomChanged, pk, sync);
        QObject::connect(realtime, &RoomRealtime::events, pk, &ClassicPkController::receive);
        QObject::connect(realtime, &RoomRealtime::connectionChanged, pk, [pk](const RealtimeConnection &c) {
            if (c.phase == RealtimeConnection::Phase::Connected) pk->refresh();
        });
        QObject::connect(realtime, &RoomRealtime::stopped, pk, [pk] { pk->setContext(0, 0, 0, false); });
        sync();
    }
    if (pkWindow->isMinimized()) pkWindow->showNormal();
    else pkWindow->show();
    pkWindow->raise();
    pkWindow->activateWindow();
}
void removeClassicPk() {
    delete pkAction.data();
    delete pkWindow.data();
}
void removeRealtimeDocks() {
    if (!frontendValid)
        return;
    for (int i = 0; i < FloatingWindows::DockCount; ++i)
        if (realtimeDocks[i])
            obs_frontend_remove_dock(FloatingWindows::dockId(i));
}
void frontendReady(obs_frontend_event event, void *) {
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        // Remove our persistent listener while this EXIT callback still owns a
        // valid frontend. No frontend API is used after the callback returns.
        obs_frontend_remove_event_callback(frontendReady, nullptr);
        waitingForFrontend = false; // Frontend APIs are invalid after this event.
        removeClassicPk();
        delete errors.data();
        if (updates) {
            updates->onObsExit();
            delete updates.data();
        }
        if (windows)
            windows->shutdown();
        removeRealtimeDocks();
        if (windows)
            delete windows.data();
        if (realtime) {
            realtime->stop();
            delete realtime.data();
        }
        frontendValid = false;
        return;
    }
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING || !waitingForFrontend)
        return;
    waitingForFrontend = false;
    createRoomDock();
}
} // namespace

bool obs_module_load(void) {
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

void obs_module_post_load(void) {
    // post_load precedes OBS's ResetOutputs(). Neither room initialization nor
    // native button state may query the output handler until the frontend is ready.
    // Register only the empty shell here so OBS can restore its saved dock layout.
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!window)
        return;
    waitingForFrontend = true;
    obs_frontend_add_event_callback(frontendReady, nullptr);
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
    livehime::installDockFocusPolicy(qobject_cast<QDockWidget *>(dockContainer->parentWidget()),
                                     qobject_cast<QMainWindow *>(window));
    // Register native OBS-owned shells before OBS restores QMainWindow::DockState.
    // Their data services and output-dependent controls are still deferred.
    char *configPath = obs_module_config_path("");
    const QString storage = configPath ? QString::fromUtf8(configPath) : QString{};
    bfree(configPath);
    bool registered = true;
    for (int i = 0; i < FloatingWindows::DockCount; ++i) {
        auto *shell = new QWidget;
        auto *shellLayout = new QVBoxLayout(shell);
        shellLayout->addWidget(new QLabel(QStringLiteral("正在加载实时面板…"), shell));
        const auto title = FloatingWindows::dockTitle(i).toUtf8();
        if (!obs_frontend_add_dock_by_id(FloatingWindows::dockId(i), title.constData(), shell)) {
            delete shell;
            registered = false;
            break;
        }
        realtimeDocks[i] = qobject_cast<QDockWidget *>(shell->parentWidget());
        if (!realtimeDocks[i]) {
            obs_frontend_remove_dock(FloatingWindows::dockId(i));
            registered = false;
            break;
        }
        realtimeDocks[i]->setAllowedAreas(Qt::AllDockWidgetAreas);
        realtimeDocks[i]->resize(i == 0 ? 420 : 370, 580);
        if (!storage.isEmpty()) {
            QSettings settings(QDir(storage).filePath("floating-windows.ini"), QSettings::IniFormat);
            if (!settings.value("nativeDocks", false).toBool())
                realtimeDocks[i]->restoreGeometry(settings.value(QString("geometry%1").arg(i)).toByteArray());
        }
    }
    if (!registered) {
        removeRealtimeDocks();
        blog(LOG_ERROR, "[livehime-obs] Cannot register realtime docks; check for duplicate plugin copies");
    }
}

namespace {
void createRoomDock() {
    auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!window)
        return;
    errors = new livehime::ErrorReports(QString::fromUtf8(obs_get_version_string()), window);
    if (!dockContainer) {
        errors->report(PluginIssue::local(QStringLiteral("插件初始化"), QStringLiteral("无法注册 B站直播面板，请检查是否安装了多份插件。")));
        return;
    }
    char *configPath = obs_module_config_path("");
    if (!configPath) {
        if (loadingLabel)
            loadingLabel->setText(QStringLiteral("无法读取插件配置目录。"));
        errors->report(PluginIssue::local(QStringLiteral("插件初始化"), QStringLiteral("无法读取插件配置目录。")));
        return;
    }
    const QString storage = QString::fromUtf8(configPath);
    bfree(configPath);
    if (!QDir().mkpath(storage)) {
        blog(LOG_ERROR, "[livehime-obs] Cannot create plugin storage directory");
        if (loadingLabel)
            loadingLabel->setText(QStringLiteral("无法创建插件配置目录。"));
        errors->report(PluginIssue::local(QStringLiteral("插件初始化"), QStringLiteral("无法创建插件配置目录。")));
        return;
    }
    livehime::diagnostics::start(storage);
    controller = new livehime::LiveController(storage, dockContainer);
    realtime = new livehime::RoomRealtime(controller, dockContainer);
    QObject::connect(controller, &livehime::LiveController::problem, errors, &livehime::ErrorReports::report);
    QObject::connect(realtime, &livehime::RoomRealtime::problem, errors, &livehime::ErrorReports::report);
    if (auto *menu = window->findChild<QMenu *>(QStringLiteral("menuDocks"))) {
        pkAction = menu->addAction(QStringLiteral("B站 · 双人经典 PK"));
        pkAction->setVisible(realtime->classicPkUnlocked());
        QObject::connect(realtime, &livehime::RoomRealtime::classicPkAvailable, pkAction, [] {
            pkAction->setVisible(true);
        });
        QObject::connect(pkAction, &QAction::triggered, window, [window] { openClassicPk(window); });
    }
    new livehime::ModerationAlerts(realtime, window);
    std::array<QDockWidget *, FloatingWindows::DockCount> registeredDocks;
    std::ranges::copy(realtimeDocks, registeredDocks.begin());
    if (std::ranges::find(registeredDocks, nullptr) == registeredDocks.end())
        windows = new FloatingWindows(realtime, storage, window, registeredDocks);
    dock = new livehime::RoomDock(controller, dockContainer);
    QObject::connect(dock, &livehime::RoomDock::problem, errors, &livehime::ErrorReports::report);
    if (windows) {
        QObject::connect(dock, &livehime::RoomDock::openRealtimeWindow, windows,
                         &FloatingWindows::showWindow);
        QObject::connect(dock, &livehime::RoomDock::unlockRealtimeInteraction, windows, [] {
            if (windows)
                windows->showWindow(0);
        });
    } else {
        dock->setRealtimeAvailable(false);
        errors->report(PluginIssue::local(QStringLiteral("初始化实时面板"), QStringLiteral("部分实时面板注册失败，请检查是否安装了多份插件。")));
    }
    for (int i = 0; i < FloatingWindows::DockCount; ++i) {
        const auto label = (QStringLiteral("B站 · ") + FloatingWindows::dockTitle(i)).toUtf8();
        auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(label.constData()));
        if (action)
            action->setEnabled(bool(windows));
        if (action && windows)
            QObject::connect(action, &QAction::triggered, windows, [i] {
                if (windows)
                    windows->showWindow(i);
            });
    }
    auto *unlock =
        static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("B站 · 解锁直播互动（取消鼠标穿透）"));
    if (unlock)
        unlock->setEnabled(bool(windows));
    if (unlock && windows)
        QObject::connect(unlock, &QAction::triggered, windows, [] {
            if (windows)
                windows->showWindow(0);
        });
    QString controlsError;
    if (!dock->attachNativeControls(window, &controlsError)) {
        blog(LOG_ERROR, "[livehime-obs] Cannot attach native streaming controls: %s",
             controlsError.toUtf8().constData());
        if (loadingLabel)
            loadingLabel->setText(controlsError);
        errors->report(PluginIssue::local(QStringLiteral("连接 OBS 控制按钮"), controlsError));
        delete dock.data();
        return;
    }
    delete loadingLabel.data();
    dockContainer->layout()->addWidget(dock);
    char *helperPath = obs_module_file("livehime-update-helper.exe");
    const auto cacheBase = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (helperPath && QFileInfo::exists(QString::fromUtf8(helperPath)) && !cacheBase.isEmpty()) {
        updates = new livehime::UpdateController(storage, QDir(cacheBase).filePath("livehime-obs/updates"),
                                                 QFileInfo(QString::fromUtf8(helperPath)).absoluteFilePath());
        QObject::connect(updates, &livehime::UpdateController::updateFailed, errors, [](const QString &detail) {
            errors->report(PluginIssue::local(QStringLiteral("更新插件"), detail));
        });
        dock->addUpdateControls(updates->createControls(dock));
    } else {
        errors->report(PluginIssue::local(QStringLiteral("更新插件"), QStringLiteral("无法初始化更新组件，请检查插件安装和缓存目录。")));
    }
    bfree(helperPath);
    auto *exportButton = new QPushButton(QStringLiteral("导出诊断包…"), dock);
    QObject::connect(exportButton, &QPushButton::clicked, errors, &livehime::ErrorReports::chooseDestination);
    dock->addUpdateControls(exportButton);
    dock->addUpdateControls(errors->createIssueButton(dock));
    controller->initialize();
    blog(LOG_INFO,
         "[livehime-obs] Loaded version " LIVEHIME_OBS_VERSION
         " (protocol profile %s); shared realtime windows and native streaming controls attached",
         livehime::protocol::ClientVersion);
}
} // namespace

void obs_module_unload(void) {
    removeClassicPk();
    delete errors.data();
    delete updates.data();
    if (frontendValid) {
        obs_frontend_remove_event_callback(frontendReady, nullptr);
        waitingForFrontend = false;
    }
    if (windows)
        windows->shutdown();
    removeRealtimeDocks();
    if (windows)
        delete windows.data();
    if (realtime) {
        realtime->stop();
        delete realtime.data();
    }
    if (dockContainer) {
        if (frontendValid)
            obs_frontend_remove_dock("livehime-obs-room");
        delete dockContainer.data();
    }
}
