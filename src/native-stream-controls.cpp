#include "native-stream-controls.hpp"

#include <QAction>
#include <QMenu>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QWidget>

namespace livehime {
namespace {
QMetaMethod method(QObject *object, const char *signature, bool signal = false)
{
    if (!object) return {};
    const auto *meta = object->metaObject();
    const int index = signal ? meta->indexOfSignal(signature) : meta->indexOfMethod(signature);
    return index < 0 ? QMetaMethod() : meta->method(index);
}
} // namespace

NativeStreamControls::NativeStreamControls(QObject *parent) : QObject(parent) {}
NativeStreamControls::~NativeStreamControls() { detach(); }

bool NativeStreamControls::attach(QWidget *mainWindow, QString *error)
{
    if (error) error->clear();
    if (attached_ && mainWindow_ == mainWindow && controls_ && button_) return true;
    detach();
    const auto fail = [this, error](const QString &message) {
        detach();
        if (error) *error = message;
        return false;
    };
    if (!mainWindow || mainWindow->thread() != thread() || QThread::currentThread() != thread())
        return fail(QStringLiteral("无法在 OBS 主界面连接直播按钮。"));

    mainWindow_ = mainWindow;
    controls_ = mainWindow->findChild<QWidget *>(QStringLiteral("OBSBasicControls"));
    if (controls_) button_ = controls_->findChild<QPushButton *>(QStringLiteral("streamButton"));
    if (!controls_ || !button_)
        return fail(QStringLiteral("找不到 OBS 原生直播按钮，请检查 OBS 版本。"));
    nativeUi_ = {button_->text(), button_->toolTip(), button_->isEnabled(), button_->menu()};

    const char *signalSignatures[] = {"StreamButtonClicked()", "StartStreamMenuActionClicked()", "StopStreamMenuActionClicked()"};
    const char *originals[] = {"StreamActionTriggered()", "StartStreaming()", "StopStreaming()"};
    const char *replacements[] = {"buttonTriggered()", "startMenuTriggered()", "stopMenuTriggered()"};
    for (size_t i = 0; i < bindings_.size(); ++i) {
        auto &binding = bindings_[i];
        binding.signal = method(controls_, signalSignatures[i], true);
        binding.original = method(mainWindow_, originals[i]);
        binding.replacement = method(this, replacements[i]);
        if (!binding.signal.isValid() || !binding.original.isValid() || !binding.replacement.isValid())
            return fail(QStringLiteral("OBS 直播控件接口不匹配，请使用兼容版本。"));
    }

    const char *stateSignals[] = {"StreamingPreparing()", "StreamingStarting(bool)", "StreamingStarted(bool)",
                                  "StreamingStopping()", "StreamingStopped(bool)"};
    const char *stateSlots[] = {"nativePreparing()", "nativeStarting(bool)", "nativeStarted(bool)",
                                "nativeStopping()", "nativeStopped(bool)"};
    std::array<QMetaMethod, 5> hostStates;
    std::array<QMetaMethod, 5> localStates;
    for (size_t i = 0; i < hostStates.size(); ++i) {
        hostStates[i] = method(mainWindow_, stateSignals[i], true);
        localStates[i] = method(this, stateSlots[i]);
        if (!hostStates[i].isValid() || !localStates[i].isValid())
            return fail(QStringLiteral("OBS 直播状态接口不匹配，请使用兼容版本。"));
    }
    for (auto &binding : bindings_) {
        binding.removed = QObject::disconnect(controls_, binding.signal, mainWindow_, binding.original);
        if (!binding.removed) {
            // OBS 33 uses typed connects, whose slot objects cannot be matched by
            // a QMetaMethod receiver index. Limit the fallback to this ONE signal
            // and this ONE receiver. Its sole built-in target was verified above.
            // Other receivers and ForceStop remain connected. A third-party
            // callback using mainWindow as context for the same signal is outside
            // this adapter's compatibility contract.
            binding.removed = QObject::disconnect(controls_, binding.signal, mainWindow_, QMetaMethod());
        }
        if (!binding.removed)
            return fail(QStringLiteral("OBS 直播按钮连接已变化，未接管原生按钮。"));
        binding.connection = QObject::connect(controls_, binding.signal, this, binding.replacement, Qt::DirectConnection);
        if (!binding.connection)
            return fail(QStringLiteral("无法接管 OBS 直播按钮，已恢复原连接。"));
    }

    for (size_t i = 0; i < hostStates.size(); ++i) {
        // OBS connected its controls first. Observe the native update immediately,
        // then reapply our presentation after all listeners have finished.
        stateConnections_[i] = QObject::connect(mainWindow_, hostStates[i], this, localStates[i], Qt::DirectConnection);
        if (!stateConnections_[i]) return fail(QStringLiteral("无法订阅 OBS 直播状态，已恢复原连接。"));
    }
    lifetimeConnections_[0] = connect(mainWindow_, &QObject::destroyed, this, [this] { detach(); });
    lifetimeConnections_[1] = connect(controls_, &QObject::destroyed, this, [this] { detach(); });
    lifetimeConnections_[2] = connect(button_, &QObject::destroyed, this, [this] { detach(); });
    attached_ = true;
    applyState();
    return true;
}

void NativeStreamControls::detach()
{
    attached_ = false;
    ++generation_;
    reapplyQueued_ = false;
    for (auto &connection : lifetimeConnections_) { QObject::disconnect(connection); connection = {}; }
    for (auto &connection : stateConnections_) { QObject::disconnect(connection); connection = {}; }
    for (auto &binding : bindings_) {
        QObject::disconnect(binding.connection);
        if (binding.removed && controls_ && mainWindow_)
            QObject::connect(controls_, binding.signal, mainWindow_, binding.original, Qt::UniqueConnection);
        binding = {};
    }
    if (button_) restoreNativeUi();
    if (managedMenu_) { managedMenu_->hide(); managedMenu_->deleteLater(); }
    managedMenu_.clear(); managedAction_.clear(); managedSeparator_.clear();
    button_.clear(); controls_.clear(); mainWindow_.clear();
    nativeUi_ = {};
    nativeActive_ = false;
    dispatching_ = false;
    applying_ = false;
}

void NativeStreamControls::setState(Action action, bool enabled, const QString &text, const QString &tooltip)
{
    action_ = action;
    enabled_ = enabled;
    text_ = text;
    tooltip_ = tooltip;
    applyState();
}

NativeStreamControls::Action NativeStreamControls::effectiveAction() const
{
    // A hotkey/API start can precede the output's active flag. Do not turn its
    // connecting button into a second Bilibili start while the native attempt is
    // still alive. Our own room already uses Stop/CancelStart at this point.
    return action_ == Action::Start && nativeActive_ ? Action::PassThrough : action_;
}

void NativeStreamControls::dispatch(size_t route)
{
    if (!attached_ || !mainWindow_ || dispatching_) return;
    const auto action = effectiveAction();
    if (action != Action::PassThrough &&
        ((route == 1 && action != Action::Start) ||
         (route == 2 && action != Action::Stop && action != Action::CancelStart))) return;
    if (action != Action::PassThrough && (!enabled_ || action == Action::Busy)) return;
    QPointer<NativeStreamControls> guard(this);
    dispatching_ = true;
    if (action == Action::PassThrough) bindings_[route].original.invoke(mainWindow_, Qt::DirectConnection);
    else if (action == Action::Start) emit startRequested();
    else emit stopRequested();
    if (guard) dispatching_ = false;
}

void NativeStreamControls::buttonTriggered() { dispatch(0); }
void NativeStreamControls::startMenuTriggered() { dispatch(1); }
void NativeStreamControls::stopMenuTriggered() { dispatch(2); }

void NativeStreamControls::restoreNativeUi()
{
    if (!button_) return;
    button_->setText(nativeUi_.text);
    button_->setToolTip(nativeUi_.tooltip);
    button_->setEnabled(nativeUi_.enabled);
    button_->setMenu(nativeUi_.menu);
}

bool NativeStreamControls::prepareManagedMenu()
{
    if (!button_ || !nativeUi_.menu) return false;
    const auto originalActions = nativeUi_.menu->actions();
    // OBSBasicControls creates start, stop, then force-stop in this order. Keep
    // the actual force QAction and any following third-party actions alive, with
    // their original connections/visibility/enabled state and ownership intact.
    if (originalActions.size() < 3 || originalActions[0]->isSeparator() ||
        originalActions[1]->isSeparator() || originalActions[2]->isSeparator()) return false;
    if (!managedMenu_) {
        managedMenu_ = new QMenu(button_);
        managedMenu_->setObjectName(QStringLiteral("livehime-native-stream-menu"));
        managedAction_ = managedMenu_->addAction(QString());
        managedAction_->setObjectName(QStringLiteral("livehime-native-stream-action"));
        managedSeparator_ = managedMenu_->addSeparator();
        connect(managedAction_, &QAction::triggered, this, &NativeStreamControls::buttonTriggered);
    }
    QList<QAction *> desired{managedAction_, managedSeparator_};
    for (qsizetype i = 2; i < originalActions.size(); ++i) desired.append(originalActions[i]);
    if (managedMenu_->actions() != desired) {
        for (auto *action : managedMenu_->actions()) managedMenu_->removeAction(action);
        for (auto *action : desired) managedMenu_->addAction(action);
    }
    managedAction_->setText(text_);
    managedAction_->setToolTip(tooltip_);
    managedAction_->setEnabled(enabled_ && action_ != Action::Busy);
    button_->setMenu(managedMenu_);
    // Stopping with a delay must still offer the original discard-delay action,
    // even though the ordinary managed action is currently busy/disabled.
    return originalActions[2]->isVisible() && originalActions[2]->isEnabled();
}

void NativeStreamControls::applyState()
{
    if (!attached_ || !button_ || applying_) return;
    applying_ = true;
    if (effectiveAction() == Action::PassThrough) restoreNativeUi();
    else {
        button_->setText(text_);
        button_->setToolTip(tooltip_);
        button_->setMenu(nativeUi_.menu);
        const bool forceStopAvailable = prepareManagedMenu();
        button_->setEnabled((enabled_ && action_ != Action::Busy) || forceStopAvailable);
    }
    applying_ = false;
}

void NativeStreamControls::captureNativeMenu()
{
    if (button_ && button_->menu() != managedMenu_) nativeUi_.menu = button_->menu();
}

void NativeStreamControls::scheduleReapply()
{
    if (!attached_ || reapplyQueued_) return;
    reapplyQueued_ = true;
    const auto generation = generation_;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != generation_) return;
        reapplyQueued_ = false;
        applyState();
    });
}

void NativeStreamControls::nativePreparing()
{
    if (!button_) return;
    nativeActive_ = true;
    nativeUi_.text = button_->text();
    nativeUi_.enabled = false;
    scheduleReapply();
}

void NativeStreamControls::nativeStarting(bool)
{
    if (!button_) return;
    nativeActive_ = true;
    nativeUi_.text = button_->text();
    scheduleReapply();
}

void NativeStreamControls::nativeStarted(bool withDelay)
{
    if (!button_) return;
    nativeActive_ = true;
    nativeUi_.text = button_->text();
    nativeUi_.enabled = true;
    if (withDelay) captureNativeMenu();
    scheduleReapply();
}

void NativeStreamControls::nativeStopping()
{
    if (!button_) return;
    nativeActive_ = true;
    nativeUi_.text = button_->text();
    scheduleReapply();
}

void NativeStreamControls::nativeStopped(bool withDelay)
{
    if (!button_) return;
    // The with-delay signal means the UI stopped while buffered media is still
    // draining. Keep external attempts in passthrough until the terminal stop.
    nativeActive_ = withDelay;
    nativeUi_.text = button_->text();
    nativeUi_.enabled = true;
    if (withDelay) captureNativeMenu();
    else nativeUi_.menu.clear();
    scheduleReapply();
}

} // namespace livehime
