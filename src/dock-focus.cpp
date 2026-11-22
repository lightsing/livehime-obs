#include "dock-focus.hpp"
#include <QDockWidget>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
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
#ifdef Q_OS_WIN
        case QEvent::MouseButtonPress:
        case QEvent::NonClientAreaMouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && dock_ && dock_->isFloating() && host_ &&
                QGuiApplication::platformName() == QStringLiteral("windows") && !hostInFront()) {
                // QDockWidget samples Ctrl on the initial title-bar press for both
                // client and native drags. Qt then moves freely for the entire drag.
                mouse->setModifiers(mouse->modifiers() | Qt::ControlModifier);
            }
            break;
        }
#endif
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

#ifdef Q_OS_WIN
    bool hostInFront() const {
        const auto host = reinterpret_cast<HWND>(host_->internalWinId());
        if (!host || !IsWindowVisible(host) || IsIconic(host))
            return false;
        const auto foreground = GetForegroundWindow();
        if (foreground == host)
            return true;
        if (foreground != reinterpret_cast<HWND>(dock_->internalWinId()))
            return false;
        // Windows can activate the floating tool before delivering the press.
        // Look beneath floating tools for the foremost application window.
        for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
            if (w == host) return true;
            if (w == foreground || !IsWindowVisible(w) || IsIconic(w) ||
                (GetWindowLongPtrW(w, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)))
                continue;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!cloaked) return false;
        }
        return false;
    }
#endif

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
