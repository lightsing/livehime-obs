#include "dock-focus.hpp"
#include <QDockWidget>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QPointer>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace livehime {
namespace {
class DockFocusPolicy final : public QObject {
  public:
    DockFocusPolicy(QDockWidget *dock, QMainWindow *host) : QObject(dock->widget()), dock_(dock), host_(host) {
        dock_->installEventFilter(this);
        connect(dock_, &QDockWidget::topLevelChanged, this, [this] { detachFloatingOwner(); });
        detachFloatingOwner();
    }

  private:
    bool eventFilter(QObject *, QEvent *event) override {
        // Qt can recreate the native window when its pin or click-through flag changes.
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::ShowToParent:
        case QEvent::WinIdChange:
            detachFloatingOwner();
            break;
        default:
            break;
        }
        return false;
    }

    void detachFloatingOwner() {
        if (!dock_ || !host_ || !dock_->isFloating())
            return;
        auto *window = dock_->windowHandle();
        auto *hostWindow = host_->windowHandle();
        if (!window || !hostWindow || window == hostWindow || !window->isTopLevel())
            return;
        if (window->transientParent() == hostWindow)
            window->setTransientParent(nullptr);
#ifdef Q_OS_WIN
        // Changing QWindow::transientParent affects the next show. A visible restored
        // dock already has an HWND owner which Windows raises when the dock is activated.
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            const auto hwnd = reinterpret_cast<HWND>(dock_->internalWinId());
            const auto hostHwnd = reinterpret_cast<HWND>(host_->internalWinId());
            if (hwnd && hostHwnd && hwnd != hostHwnd && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) &&
                GetWindow(hwnd, GW_OWNER) == hostHwnd)
                SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
        }
#endif
    }

    QPointer<QDockWidget> dock_;
    QPointer<QMainWindow> host_;
};
} // namespace

void installDockFocusPolicy(QDockWidget *dock, QMainWindow *host) {
    if (dock && host && dock->widget())
        new DockFocusPolicy(dock, host);
}
} // namespace livehime
