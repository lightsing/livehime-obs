#pragma once

#include <QMetaMethod>
#include <QObject>
#include <QPointer>
#include <QString>
#include <array>

class QAction;
class QMenu;
class QPushButton;
class QWidget;

namespace livehime {

// OBS 33 controls adapter. All calls and host objects must be on the UI thread.
// This deliberately does not intercept the frontend API or OBS hotkeys.
class NativeStreamControls final : public QObject {
    Q_OBJECT
public:
    enum class Action { PassThrough, Start, Stop, CancelStart, Busy };
    explicit NativeStreamControls(QObject *parent = nullptr);
    ~NativeStreamControls() override;

    bool attach(QWidget *mainWindow, QString *error);
    void detach();
    void setState(Action action, bool enabled, const QString &text, const QString &tooltip);

signals:
    void startRequested();
    void stopRequested();

private slots:
    void buttonTriggered();
    void startMenuTriggered();
    void stopMenuTriggered();
    void nativePreparing();
    void nativeStarting(bool broadcastAutoStart);
    void nativeStarted(bool withDelay);
    void nativeStopping();
    void nativeStopped(bool withDelay);

private:
    struct Binding {
        QMetaMethod signal;
        QMetaMethod original;
        QMetaMethod replacement;
        QMetaObject::Connection connection;
        bool removed = false;
    };
    struct NativeUi {
        QString text;
        QString tooltip;
        bool enabled = true;
        QPointer<QMenu> menu;
    };

    void dispatch(size_t route);
    Action effectiveAction() const;
    void scheduleReapply();
    void applyState();
    void restoreNativeUi();
    void captureNativeMenu();
    bool prepareManagedMenu();

    QPointer<QWidget> mainWindow_;
    QPointer<QWidget> controls_;
    QPointer<QPushButton> button_;
    QPointer<QMenu> managedMenu_;
    QPointer<QAction> managedAction_;
    QPointer<QAction> managedSeparator_;
    std::array<Binding, 3> bindings_;
    std::array<QMetaObject::Connection, 5> stateConnections_;
    std::array<QMetaObject::Connection, 3> lifetimeConnections_;
    NativeUi nativeUi_;
    Action action_ = Action::PassThrough;
    bool enabled_ = true;
    QString text_;
    QString tooltip_;
    bool attached_ = false;
    bool dispatching_ = false;
    bool applying_ = false;
    bool reapplyQueued_ = false;
    bool nativeActive_ = false;
    quint64 generation_ = 0;
};

} // namespace livehime
