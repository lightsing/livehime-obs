#include "update-controller.hpp"
#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QSignalBlocker>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <windows.h>
#include <array>
#include <chrono>

namespace livehime {
namespace {
using State = PluginUpdater::State;

QString stageHelper(const QString &source, const QDir &cache) {
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 4 * 1024 * 1024 || !cache.mkpath("."))
        return {};
    const auto image = file.readAll();
    if (image.size() != file.size()) return {};
    const auto sha = QCryptographicHash::hash(image, QCryptographicHash::Sha256);
    const QString target = cache.filePath("helper-" + QString::fromLatin1(sha.toHex()) + ".exe");
    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly)) {
        if (existing.size() == image.size() && existing.readAll() == image)
            return target;
        existing.close();
    }
    QSaveFile copy(target);
    if (!copy.open(QIODevice::WriteOnly) || copy.write(image) != image.size() || !copy.commit())
        return {};
    return target;
}

QString registeredSharedHost(const QDir &data) {
    const auto stateFile = QDir::toNativeSeparators(data.filePath("install-state.ini")).toStdWString();
    if (!GetPrivateProfileIntW(L"Install", L"SharedBundle", 0, stateFile.c_str()))
        return {};
    std::array<wchar_t, 32768> host{};
    const auto length = GetPrivateProfileStringW(L"Install", L"ObsRoot", L"", host.data(), DWORD(host.size()), stateFile.c_str());
    if (!length || length >= host.size() - 1) return {};
    const auto root = QString::fromWCharArray(host.data(), int(length));
    return QFileInfo::exists(QDir(root).filePath("bin/64bit/obs64.exe")) ? root : QString{};
}
} // namespace

UpdateController::UpdateController(const QString &configDir, const QString &cacheDir, const QString &helper, QObject *parent)
    : QObject(parent), updater_(QStringLiteral(LIVEHIME_OBS_VERSION), cacheDir, this),
      settings_(QDir(configDir).filePath("updates.ini"), QSettings::IniFormat),
      cacheDir_(cacheDir), helper_(helper), automatic_(settings_.value("automatic", true).toBool()),
      betaUpdates_(settings_.value("channel", QStringLiteral(LIVEHIME_OBS_VERSION).contains("-beta.") ? "beta" : "stable") == "beta"),
      installAtExit_(automatic_) {
    if (betaUpdates_ && !settings_.contains("channel")) settings_.setValue("channel", "beta");
    updater_.setChannel(betaUpdates_ ? update::Channel::Beta : update::Channel::Stable);
    connect(&updater_, &PluginUpdater::changed, this, &UpdateController::onUpdateChanged);
    connect(&updater_, &PluginUpdater::updateFailed, this, &UpdateController::updateFailed);
    // The deferred installer starts during OBS shutdown. Report handoff failures on the next launch.
    if (settings_.contains("exitFailure")) {
        QTimer::singleShot(0, this, [this] {
            const auto reason = settings_.value("exitFailure").toString();
            settings_.remove("exitFailure");
            if (reason == "layout")
                emit updateFailed(QStringLiteral("上次退出时无法识别插件安装位置，更新未启动。"));
            else if (reason == "helper")
                emit updateFailed(QStringLiteral("上次退出时无法启动更新助手，请重新检查更新。"));
        });
    }
    auto backgroundCheck = [this] {
        if (!automatic_ || exiting_) return;
        const auto state = updater_.state();
        if (state != State::Checking && state != State::Downloading && state != State::Ready)
            check();
    };
    QTimer::singleShot(std::chrono::seconds{5}, this, backgroundCheck);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, backgroundCheck);
    timer->start(std::chrono::hours{6});
}

void UpdateController::check() {
    downloadAttempted_ = false;
    updater_.check();
}

void UpdateController::onUpdateChanged() {
    if (automatic_ && !exiting_ && updater_.state() == State::Available && !downloadAttempted_) {
        downloadAttempted_ = true;
        updater_.download();
    }
    emit changed();
}

bool UpdateController::installOnExit() const {
    return installAtExit_ && updater_.state() == State::Ready;
}

QWidget *UpdateController::createControls(QWidget *parent) {
    auto *panel = new QWidget(parent);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *row = new QHBoxLayout;
    auto *automatic = new QCheckBox(QStringLiteral("自动更新插件"), panel);
    automatic->setToolTip(QStringLiteral("自动检查并下载；退出所有 OBS 窗口后打开安装向导。"));
    automatic->setChecked(automatic_);
    auto *action = new QPushButton(panel);
    row->addWidget(automatic);
    row->addStretch();
    row->addWidget(action);
    layout->addLayout(row);
    auto *beta = new QCheckBox(QStringLiteral("加入测试版更新"), panel);
    beta->setObjectName("betaUpdates");
    beta->setToolTip(QStringLiteral("测试版会在本地记录诊断日志。取消后可切回最新正式版。"));
    layout->addWidget(beta);
#if LIVEHIME_BETA_BUILD
    auto *openLog = new QPushButton(QStringLiteral("打开诊断目录"), panel);
    openLog->setObjectName("openDiagnostics");
    layout->addWidget(openLog);
    connect(openLog, &QPushButton::clicked, this, [this] {
        const QDir config(QFileInfo(settings_.fileName()).absolutePath());
        QDesktopServices::openUrl(QUrl::fromLocalFile(config.filePath("diagnostics")));
    });
#endif
    auto *status = new QLabel(panel);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    const auto render = [this, automatic, beta, action, status] {
        const QSignalBlocker blockAuto(automatic), blockBeta(beta);
        automatic->setChecked(automatic_);
        beta->setChecked(betaUpdates_);
        auto state = updater_.state();
        action->setEnabled(state != State::Checking && state != State::Downloading);
        action->setText(state == State::Available ? QStringLiteral("下载更新") :
                        state == State::Ready ? (installOnExit() ? QStringLiteral("本次跳过") : QStringLiteral("退出后安装")) :
                        QStringLiteral("检查更新"));
        const auto detail = state == State::Ready ? (installOnExit() ? QStringLiteral("退出所有 OBS 后打开安装向导")
                                                                     : QStringLiteral("已下载，本次退出时不安装"))
                                                  : updater_.status();
        const auto version = state == State::Ready ? updater_.version() : QString{};
        const auto prefix = version.isEmpty() ? QStringLiteral("当前 v") + QStringLiteral(LIVEHIME_OBS_VERSION)
                                              : QStringLiteral("待安装 v") + version;
        status->setText(detail.isEmpty() ? prefix : prefix + QStringLiteral(" · ") + detail);
    };
    connect(this, &UpdateController::changed, panel, render);
    connect(automatic, &QCheckBox::toggled, this, [this](bool enabled) {
        automatic_ = installAtExit_ = enabled;
        settings_.setValue("automatic", enabled);
        if (enabled && updater_.state() != State::Ready && updater_.state() != State::Downloading && updater_.state() != State::Checking)
            check();
        emit changed();
    });
    connect(beta, &QCheckBox::toggled, this, [this](bool enabled) {
        betaUpdates_ = enabled;
        settings_.setValue("channel", enabled ? "beta" : "stable");
        settings_.sync();
        installAtExit_ = automatic_;
        updater_.setChannel(enabled ? update::Channel::Beta : update::Channel::Stable);
        check();
    });
    connect(action, &QPushButton::clicked, this, [this] {
        switch (updater_.state()) {
        case State::Available:
            downloadAttempted_ = true;
            updater_.download();
            break;
        case State::Ready:
            installAtExit_ = !installAtExit_;
            emit changed();
            break;
        default:
            check();
            break;
        }
    });
    render();
    return panel;
}

void UpdateController::onObsExit() {
    if (exiting_) return;
    exiting_ = true;
    if (!installOnExit()) return;
    QDir data(QFileInfo(helper_).absolutePath());
    QDir bundle(data);
    QString obsRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString legacyData = QDir(obsRoot).filePath("data/obs-plugins/livehime-obs");
    if (data.absolutePath().compare(legacyData, Qt::CaseInsensitive) == 0) {
        bundle = QDir(obsRoot);
    } else if (data.dirName().compare("data", Qt::CaseInsensitive) == 0 && bundle.cdUp() &&
               bundle.dirName().compare("livehime-obs", Qt::CaseInsensitive) == 0) {
        const auto registered = registeredSharedHost(data);
        if (!registered.isEmpty()) obsRoot = registered;
    } else {
        settings_.setValue("exitFailure", "layout");
        settings_.sync();
        qWarning("Livehime OBS: cannot identify plugin layout for update");
        return;
    }
    const auto helper = stageHelper(helper_, QDir(cacheDir_).filePath("helpers"));
    const QStringList args{"--pid", QString::number(QCoreApplication::applicationPid()),
                           "--installer", QDir::toNativeSeparators(updater_.installerPath()),
                           "--sha256", QString::fromLatin1(updater_.installerSha256()),
                           "--obs-dir", QDir::toNativeSeparators(obsRoot),
                           "--bundle-dir", QDir::toNativeSeparators(bundle.absolutePath())};
    if (helper.isEmpty() || !QProcess::startDetached(helper, args, cacheDir_)) {
        settings_.setValue("exitFailure", "helper");
        settings_.sync();
        qWarning("Livehime OBS: could not start the deferred update helper");
    }
}
} // namespace livehime
