// Real update controls and timers with an in-memory transport. No installer or helper is present.
#include "update-controller.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <utility>

using namespace livehime;
namespace {
int failures = 0, checks = 0, downloads = 0, helperAttempts = 0;
bool failCheck = false, failDownload = false;
QtMessageHandler previousHandler = nullptr;

void require(bool ok, const char *name) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}
bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 1500) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}
void captureHelperAttempt(QtMsgType kind, const QMessageLogContext &context, const QString &message) {
    // Missing helper short-circuits before QProcess::startDetached in onObsExit().
    if (kind == QtWarningMsg && message == "Livehime OBS: could not start the deferred update helper") {
        ++helperAttempts;
    } else if (previousHandler) {
        previousHandler(kind, context, message);
    }
}
struct Controls {
    QWidget host;
    QWidget *panel;
    QCheckBox *automatic;
    QPushButton *action;
    QLabel *status;
    explicit Controls(UpdateController &controller)
        : panel(controller.createControls(&host)), automatic(panel->findChild<QCheckBox *>()),
          action(panel->findChild<QPushButton *>()), status(panel->findChild<QLabel *>()) {
        if (!automatic || !action || !status) qFatal("Update controls are incomplete");
    }
};
} // namespace

namespace livehime {
PluginUpdater::PluginUpdater(QString currentVersion, QString cacheDirectory, QObject *parent)
    : QObject(parent), currentVersion_(std::move(currentVersion)), cacheDirectory_(std::move(cacheDirectory)) {}
PluginUpdater::~PluginUpdater() = default;
QNetworkReply *PluginUpdater::get(const QNetworkRequest &) {
    qFatal("Unexpected network request");
    return nullptr;
}
void PluginUpdater::setState(State next, const QString &message) {
    state_ = next;
    status_ = message;
    emit changed();
}
void PluginUpdater::check() {
    ++checks;
    setState(State::Checking, QStringLiteral("检查中"));
    QTimer::singleShot(0, this, [this] {
        if (failCheck) {
            setState(State::Error, QStringLiteral("模拟检查失败"));
        } else {
            release_ = update::Release{QStringLiteral("9.0.0"), {}, {}};
            setState(State::Available, QStringLiteral("可下载"));
        }
    });
}
void PluginUpdater::download() {
    if (++downloads > 12) qFatal("Unbounded automatic retry");
    require(state_ == State::Available, "download requires an available release");
    setState(State::Downloading, QStringLiteral("下载中"));
    QTimer::singleShot(0, this, [this] {
        // The real transport returns to Available after all download sources fail.
        if (failDownload) setState(State::Available, QStringLiteral("模拟下载失败"));
        else setState(State::Ready, QStringLiteral("下载完成"));
    });
}
} // namespace livehime

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QDir root(temporary.path());
    const auto manualSettings = root.filePath("manual");
    const auto autoSettings = root.filePath("automatic");
    const auto cache = root.filePath("cache");
    const auto helper = root.filePath("livehime-obs/data/missing.exe");
    if (!root.mkpath("livehime-obs/data") || !root.mkpath("manual") || !root.mkpath("automatic")) return 1;
    previousHandler = qInstallMessageHandler(captureHelperAttempt);
    {
        UpdateController controller(manualSettings, cache, helper);
        Controls ui(controller);
        require(ui.automatic->isChecked(), "automatic updates default on");
        ui.automatic->click();
    }
    {
        UpdateController manual(manualSettings, cache, helper);
        Controls controls(manual);
        require(!controls.automatic->isChecked(), "disabled update preference survives restart");

        failDownload = true;
        QElapsedTimer elapsed;
        elapsed.start();
        UpdateController automatic(autoSettings, cache, helper);
        Controls background(automatic);
        require(checks == 0 && downloads == 0, "construction does not start a network check");
        require(waitFor([&] { return background.status->text().contains(QStringLiteral("模拟下载失败")); }, 8000),
                "startup check automatically downloads an available update");
        require(elapsed.elapsed() >= 3000 && checks == 1 && downloads == 1,
                "automatic startup is delayed and disabled controller stays idle");
        require(background.action->isEnabled() && background.action->text() == QStringLiteral("下载更新"),
                "failed auto-download remains available for manual retry");
        failDownload = false;
        background.action->click();
        require(!background.action->isEnabled(), "one download pending");
        require(waitFor([&] { return background.action->text() == QStringLiteral("本次跳过"); }) && downloads == 2,
                "manual retry readies the auto-downloaded installer");
        background.action->click();
        require(background.action->text() == QStringLiteral("退出后安装"), "skip applies to the current exit");
        automatic.onObsExit();
        require(helperAttempts == 0, "skipped update does not launch the helper");

        failCheck = true;
        controls.action->click();
        require(!controls.action->isEnabled(), "manual check disables duplicate clicks");
        require(waitFor([&] { return controls.action->isEnabled(); }) &&
                    controls.status->text().contains(QStringLiteral("模拟检查失败")),
                "failed manual check offers an explicit retry");
        failCheck = false;
        controls.action->click();
        require(waitFor([&] { return controls.action->text() == QStringLiteral("下载更新"); }) &&
                    checks == 3 && downloads == 2, "manual check alone does not download");
        controls.action->click();
        require(waitFor([&] { return controls.action->text() == QStringLiteral("退出后安装"); }) &&
                    downloads == 3 && controls.status->text().contains("9.0.0"), "manual download completed");
        controls.action->click();
        require(controls.action->text() == QStringLiteral("本次跳过") && !controls.automatic->isChecked(),
                "manual install-on-exit is separate from the persistent preference");
        manual.onObsExit();
        manual.onObsExit();
        require(helperAttempts == 1, "selected install is handed off only once on exit");
    }
    qInstallMessageHandler(previousHandler);
    return failures ? 1 : 0;
}
