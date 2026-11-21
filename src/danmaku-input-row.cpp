#include "danmaku-input-row.hpp"
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegion>

namespace livehime {
DanmakuInputRow::DanmakuInputRow(QDockWidget *dock, QLineEdit *input, QPushButton *send, QWidget *parent)
    : QWidget(parent), dock_(dock), controls_(new QWidget(this)), input_(input), send_(send) {
    setObjectName("danmakuInputRow");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controls_->setObjectName("danmakuControls");
    controls_->setAttribute(Qt::WA_ShowWithoutActivating);
    auto *row = new QHBoxLayout(controls_);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(input_, 1);
    row->addWidget(send_);

    controls_->installEventFilter(this);
    input_->installEventFilter(this);
    send_->installEventFilter(this);
    // Qt can distribute the final control widths after the viewport resize event.
    // Take the final hit region once both children have received their geometry.
    layoutTimer_.setSingleShot(true);
    connect(&layoutTimer_, &QTimer::timeout, this, [this] {
        if (passthrough_)
            syncWindow();
    });
    for (auto *w = static_cast<QWidget *>(this); w; w = w->parentWidget()) {
        w->installEventFilter(this);
        if (w == dock)
            break;
    }
}

QSize DanmakuInputRow::sizeHint() const { return controls_->layout()->sizeHint(); }
QSize DanmakuInputRow::minimumSizeHint() const { return controls_->layout()->minimumSize(); }

void DanmakuInputRow::setPassthrough(bool enabled) {
    if (enabled != passthrough_) {
        passthrough_ = enabled;
        // Windows skips a transparent HWND including all its child widgets. Promote
        // the existing editor and button together so they retain their text and signals.
        if (enabled) {
            controls_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        } else {
            controls_->clearMask();
            controls_->setWindowOpacity(1);
            controls_->setWindowFlags(Qt::Widget);
            controls_->setGeometry(rect());
            controls_->show();
        }
    }
    if (enabled)
        syncWindow();
}

void DanmakuInputRow::syncWindow() {
    if (!isVisible() || !dock_ || !dock_->isFloating() || dock_->isMinimized()) {
        controls_->hide();
        return;
    }

    controls_->setGeometry(QRect(mapToGlobal(QPoint()), size()));
    controls_->layout()->activate();
    QRegion hitArea = QRegion(input_->geometry()) | send_->geometry();
    hitArea &= rect();
    // Scrolling moves the content widget. Only the part within the viewport
    // may receive input, even when the row is partly visible.
    for (auto *w = this->parentWidget(); w; w = w->parentWidget()) {
        hitArea &= QRect(-mapTo(w, QPoint()), w->size());
        if (w == dock_)
            break;
    }
    if (hitArea.isEmpty()) {
        controls_->hide();
        return;
    }
    if (controls_->mask() != hitArea)
        controls_->setMask(hitArea);
    if (!qFuzzyCompare(controls_->windowOpacity(), dock_->windowOpacity()))
        controls_->setWindowOpacity(dock_->windowOpacity());
    if (controls_->isHidden())
        controls_->show();
}

bool DanmakuInputRow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == input_ || watched == send_) {
        if (passthrough_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
            layoutTimer_.start(0);
    } else if (watched == controls_) {
        if (event->type() == QEvent::LayoutRequest) {
            updateGeometry();
            if (passthrough_)
                layoutTimer_.start(0);
        }
    } else if (passthrough_) {
        switch (event->type()) {
        case QEvent::Hide:
            controls_->hide();
            break;
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::WindowStateChange:
        case QEvent::DevicePixelRatioChange:
            syncWindow();
            break;
        default:
            break;
        }
    } else if (watched == this && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        controls_->setGeometry(rect());
    }
    return false;
}
} // namespace livehime
