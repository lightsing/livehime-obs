#include "danmaku-follow.hpp"

#include <QAbstractItemModel>
#include <QEvent>
#include <QKeyEvent>
#include <QListView>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>

namespace livehime {
DanmakuFollow::DanmakuFollow(QListView *view, int sequenceRole)
    : QObject(view), view_(view), bubble_(new QPushButton(view->viewport())), sequenceRole_(sequenceRole) {
    bubble_->setObjectName(QStringLiteral("danmakuUnread"));
    bubble_->setFocusPolicy(Qt::NoFocus);
    bubble_->setCursor(Qt::PointingHandCursor);
    bubble_->setStyleSheet(QStringLiteral("QPushButton { background: palette(highlight); color: palette(highlighted-text);"
                                          " border: 1px solid palette(mid); border-radius: 10px; padding: 2px 9px; }"));
    bubble_->hide();
    connect(bubble_, &QPushButton::clicked, this, &DanmakuFollow::jumpToLatest);

    restoreTimer_.setSingleShot(true);
    inputTimer_.setSingleShot(true);
    connect(&restoreTimer_, &QTimer::timeout, this, &DanmakuFollow::restorePosition);
    auto *bar = view_->verticalScrollBar();
    connect(bar, &QScrollBar::rangeChanged, this, &DanmakuFollow::scheduleRestore);
    connect(bar, &QScrollBar::valueChanged, this, &DanmakuFollow::scrollChanged);
    connect(bar, &QScrollBar::actionTriggered, this, [this](int action) {
        inputTimer_.start(0);
        auto *bar = view_->verticalScrollBar();
        if (!following_ && bar->value() == bar->maximum() &&
            (action == QAbstractSlider::SliderToMaximum || action == QAbstractSlider::SliderSingleStepAdd ||
             action == QAbstractSlider::SliderPageStepAdd))
            scrollChanged();
    });
    view_->installEventFilter(this);
    view_->viewport()->installEventFilter(this);
    for (auto *parent = view_->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto *outer = qobject_cast<QAbstractScrollArea *>(parent)) {
            outer->viewport()->installEventFilter(this);
            connect(outer->verticalScrollBar(), &QScrollBar::valueChanged, this, &DanmakuFollow::updateBubble);
            connect(outer->horizontalScrollBar(), &QScrollBar::valueChanged, this, &DanmakuFollow::updateBubble);
        }
        if (parent->isWindow())
            break;
    }
    auto *model = view_->model();
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, &DanmakuFollow::beginRebuild);
    connect(model, &QAbstractItemModel::modelReset, this, &DanmakuFollow::endRebuild);
    connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this, &DanmakuFollow::scheduleRestore);
    connect(model, &QAbstractItemModel::layoutChanged, this, &DanmakuFollow::scheduleRestore);
    connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this, &DanmakuFollow::scheduleRestore);
    connect(model, &QAbstractItemModel::rowsInserted, this, &DanmakuFollow::scheduleRestore);
    connect(model, &QAbstractItemModel::dataChanged, this, &DanmakuFollow::scheduleRestore);
    scheduleRestore();
}

DanmakuFollow::~DanmakuFollow() {
    delete bubble_.data();
}

void DanmakuFollow::messagesArrived(int count) {
    if (count > 0 && !following_) {
        unread_ += count;
        updateBubble();
    }
}

void DanmakuFollow::beginRebuild() {
    ++rebuildDepth_;
}

void DanmakuFollow::endRebuild() {
    Q_ASSERT(rebuildDepth_ > 0);
    if (--rebuildDepth_ == 0)
        scheduleRestore();
}

void DanmakuFollow::setPaused(bool paused) {
    if (paused_ == paused)
        return;
    paused_ = paused;
    if (paused && following_) {
        const QScopedValueRollback guard(restoring_, true);
        rememberPosition();
    }
    scheduleRestore();
}

void DanmakuFollow::reset() {
    unread_ = 0;
    following_ = true;
    anchorRow_ = -1;
    anchorSequence_ = 0;
    forceBottom_ = true;
    updateBubble();
    scheduleRestore();
}

void DanmakuFollow::jumpToLatest() {
    reset();
    emit latestRequested();
}

void DanmakuFollow::scheduleRestore() {
    if (!restoring_ && rebuildDepth_ == 0 && !restoreTimer_.isActive())
        restoreTimer_.start(0);
}

void DanmakuFollow::rememberPosition() {
    auto *viewport = view_->viewport();
    if (!view_->model() || !view_->model()->rowCount() || viewport->width() == 0)
        return;
    // There can be spacing between items. Keep the first visible row's offset,
    // including partial scrolling into a wrapped line.
    for (int y = 0; y < viewport->height(); ++y) {
        const auto index = view_->indexAt(QPoint(viewport->width() / 2, y));
        if (index.isValid()) {
            anchorRow_ = index.row();
            anchorSequence_ = index.data(sequenceRole_).toLongLong();
            anchorTop_ = view_->visualRect(index).top();
            return;
        }
    }
}

void DanmakuFollow::restorePosition() {
    if (rebuildDepth_ || restoring_ || !view_->isVisible())
        return;
    const QScopedValueRollback guard(restoring_, true);
    if (following_ && (!paused_ || forceBottom_)) {
        view_->scrollToBottom();
        if (paused_)
            rememberPosition();
        forceBottom_ = false;
        return;
    }
    auto *model = view_->model();
    const int count = model ? model->rowCount() : 0;
    if (!count || anchorRow_ < 0)
        return;
    int row = qMin(anchorRow_, count - 1);
    if (anchorSequence_ > 0 && model->index(row, 0).data(sequenceRole_).toLongLong() != anchorSequence_) {
        // If the capped history has evicted the anchor, keep the closest newer row.
        int lo = 0, hi = count;
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (model->index(mid, 0).data(sequenceRole_).toLongLong() < anchorSequence_)
                lo = mid + 1;
            else
                hi = mid;
        }
        row = qMin(lo, count - 1);
        const auto sequence = model->index(row, 0).data(sequenceRole_).toLongLong();
        if (sequence != anchorSequence_) {
            anchorSequence_ = sequence;
            anchorTop_ = 0;
        }
    }
    const auto rect = view_->visualRect(model->index(row, 0));
    if (rect.isValid()) {
        auto *bar = view_->verticalScrollBar();
        bar->setValue(bar->value() + rect.top() - anchorTop_);
    }
}

void DanmakuFollow::scrollChanged() {
    auto *bar = view_->verticalScrollBar();
    if (!view_->isVisible() || rebuildDepth_ || restoring_ ||
        (restoreTimer_.isActive() && !inputTimer_.isActive() && !bar->isSliderDown()))
        return;
    following_ = bar->value() == bar->maximum();
    forceBottom_ = false;
    if (following_ && unread_) {
        unread_ = 0;
        updateBubble();
        // The real scrollbar can reach the end of the old model while hover
        // has held back newly arrived rows. Flush after Qt finishes scrolling.
        QTimer::singleShot(0, this, [this] {
            if (following_) {
                forceBottom_ = true;
                scheduleRestore();
                emit latestRequested();
            }
        });
    }
    if (!following_ || paused_)
        rememberPosition();
}

void DanmakuFollow::updateBubble() {
    if (!bubble_)
        return;
    if (!unread_) {
        bubble_->hide();
        return;
    }
    const auto label = QStringLiteral("↓ %1 条新弹幕").arg(unread_);
    bubble_->setText(label);
    bubble_->setAccessibleName(QStringLiteral("查看 %1 条新弹幕，返回底部").arg(unread_));
    auto *viewport = view_->viewport();
    QRect visible = viewport->rect();
    // A short dock's outer scroll area can partially clip this list without
    // resizing it. Keep the button inside the portion the reader can click.
    for (auto *parent = viewport->parentWidget(); parent; parent = parent->parentWidget()) {
        visible &= QRect(viewport->mapFrom(parent, QPoint()), parent->size());
        if (parent->isWindow())
            break;
    }
    const auto desired = bubble_->sizeHint();
    if (visible.width() < desired.width() || visible.height() < desired.height()) {
        bubble_->hide();
        return;
    }
    bubble_->resize(desired);
    bubble_->move(visible.x() + (visible.width() - bubble_->width()) / 2,
                  visible.y() + qMax(0, visible.height() - bubble_->height() - 5));
    bubble_->show();
    bubble_->raise();
}

bool DanmakuFollow::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Show:
    case QEvent::Move:
        updateBubble();
        if (watched == view_->viewport())
            scheduleRestore();
        break;
    case QEvent::Wheel:
    case QEvent::Scroll:
        if (watched == view_ || watched == view_->viewport())
            inputTimer_.start(0);
        break;
    case QEvent::KeyPress: {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if ((watched == view_ || watched == view_->viewport()) &&
            (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Home || key == Qt::Key_End ||
             key == Qt::Key_PageUp || key == Qt::Key_PageDown))
            inputTimer_.start(0);
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
} // namespace livehime
