#include "danmaku-follow.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTimer>
#include <QWheelEvent>
#include <iostream>

namespace {
constexpr int SequenceRole = Qt::UserRole + 1;
int failures = 0;
void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        ++failures;
    }
}
void settle() {
    QEventLoop loop;
    QTimer::singleShot(40, &loop, &QEventLoop::quit);
    loop.exec();
}
void append(QStandardItemModel &model, int first, int last) {
    for (int sequence = first; sequence <= last; ++sequence) {
        auto *item = new QStandardItem(QString::number(sequence));
        item->setData(sequence, SequenceRole);
        item->setSizeHint(QSize(100, 25));
        model.appendRow(item);
    }
}
struct Anchor {
    int sequence;
    int y;
    bool operator==(const Anchor &) const = default;
};
Anchor anchor(const QListView &view) {
    auto i = view.indexAt(QPoint(2, 0));
    return {i.data(SequenceRole).toInt(), view.visualRect(i).top()};
}
void checkClippedViewport() {
    QStandardItemModel model;
    QScrollArea outer;
    auto *content = new QWidget;
    content->setFixedSize(340, 600);
    auto *view = new QListView(content);
    view->setGeometry(8, 55, 300, 250);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setModel(&model);
    append(model, 1, 40);
    outer.setWidget(content);
    outer.resize(348, 190);
    livehime::DanmakuFollow follow(view, SequenceRole);
    auto *bubble = view->viewport()->findChild<QPushButton *>("danmakuUnread");
    outer.show();
    settle();
    const QPointF cursor(20, 20);
    QWheelEvent up(cursor, view->viewport()->mapToGlobal(cursor), {}, QPoint(0, 120),
                   Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &up);
    follow.messagesArrived(3);
    settle();
    auto bubbleOnscreen = [&] {
        const QPoint position = bubble->mapTo(outer.viewport(), QPoint());
        return outer.viewport()->rect().contains(QRect(position, bubble->size()));
    };
    check(!follow.isFollowing() && bubble->isVisible() && bubbleOnscreen(),
          "wheel scrolling a partially clipped chat list leaves the unread control inside the visible region");
    const auto reading = anchor(*view);
    outer.verticalScrollBar()->setValue(outer.verticalScrollBar()->maximum());
    settle();
    check(bubble->isHidden() && follow.unreadCount() == 3 && anchor(*view) == reading,
          "scrolling the outer dock past the list never clears the unread count or moves the inner history");
    outer.verticalScrollBar()->setValue(130);
    settle();
    check(bubble->isVisible() && bubbleOnscreen(), "the control returns as soon as the list is visible again");
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    checkClippedViewport();
    QStandardItemModel model;
    QListView view;
    view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view.resize(360, 160);
    view.setModel(&model);
    append(model, 1, 40);
    livehime::DanmakuFollow follow(&view, SequenceRole);
    auto *bubble = view.viewport()->findChild<QPushButton *>("danmakuUnread");
    check(bubble != nullptr, "the unread control belongs to the scrolling viewport");
    if (!bubble) return 1;
    view.show();
    settle();
    auto *bar = view.verticalScrollBar();
    check(follow.isFollowing() && bar->maximum() > 0 && bar->value() == bar->maximum() && bubble->isHidden(),
          "opening a populated list follows the latest message");

    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
    settle();
    const auto reading = anchor(view);
    check(!follow.isFollowing() && reading.sequence > 10, "scrolling away starts reading previous messages");
    follow.messagesArrived(3);
    follow.beginRebuild();
    model.clear();
    append(model, 1, 43);
    follow.endRebuild();
    settle();
    check(follow.unreadCount() == 3 && bubble->isVisible() && bubble->text().contains("3") && anchor(view) == reading,
          "receipts are counted once and a full rebuild preserves the same row and pixel offset");
    check(qAbs(bubble->geometry().center().x() - view.viewport()->rect().center().x()) <= 1 &&
          bubble->geometry().bottom() >= view.viewport()->height() - 10,
          "the unread button sits at the bottom center of the viewport");

    follow.beginRebuild(); // Presentation-only rebuild: no new receipt.
    model.clear();
    append(model, 8, 43);
    follow.endRebuild();
    settle();
    check(follow.unreadCount() == 3 && anchor(view) == reading, "capped history anchors the same sequence after eviction");
    model.item(0)->setSizeHint(QSize(100, 110)); // A previously loaded image changes an earlier row height.
    view.doItemsLayout();
    settle();
    check(follow.unreadCount() == 3 && anchor(view) == reading,
          "asynchronous row heights preserve the reader position without inventing unread messages");

    follow.messagesArrived(2);
    follow.messagesArrived(0);
    view.resize(268, 130);
    settle();
    check(follow.unreadCount() == 5 && anchor(view) == reading &&
          qAbs(bubble->geometry().center().x() - view.viewport()->rect().center().x()) <= 1,
          "additional receipts accumulate and the control follows viewport resizes");
    follow.setPaused(true);
    bool requested = false;
    QObject::connect(&follow, &livehime::DanmakuFollow::latestRequested, &view, [&] {
        requested = true;
        follow.beginRebuild();
        const auto last = model.index(model.rowCount() - 1, 0).data(SequenceRole).toInt();
        append(model, last + 1, last + 2);
        follow.endRebuild();
    });
    bubble->click();
    settle();
    check(requested && !follow.unreadCount() && bubble->isHidden() && bar->value() == bar->maximum(),
          "clicking clears unread and reaches even rows synchronously flushed from a hovered list");
    const auto hovered = anchor(view);
    model.item(0)->setSizeHint(QSize(100, 35));
    append(model, 46, 47);
    settle();
    check(anchor(view) == hovered, "hover pause holds the reader while the model changes");
    follow.setPaused(false);
    settle();
    check(follow.isFollowing() && bar->value() == bar->maximum(), "leaving hover resumes automatic following");
    model.item(0)->setSizeHint(QSize(100, 180));
    view.doItemsLayout();
    settle();
    check(follow.isFollowing() && !follow.unreadCount() && bar->value() == bar->maximum(),
          "a following list stays at the bottom when an asynchronous image expands");

    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    follow.messagesArrived(7);
    follow.setPaused(true);
    requested = false;
    bar->triggerAction(QAbstractSlider::SliderToMaximum);
    settle();
    check(requested && follow.isFollowing() && bubble->isHidden() && !follow.unreadCount() &&
          bar->value() == bar->maximum(), "scrolling back to the bottom marks messages read and flushes held arrivals");
    follow.setPaused(false);
    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    follow.messagesArrived(1);
    follow.reset();
    model.clear();
    append(model, 1, 12);
    settle();
    check(follow.isFollowing() && bubble->isHidden() && bar->value() == bar->maximum(),
          "a new room or a cleared feed starts at the bottom with no unread count");

    QListView extraView;
    extraView.setModel(&model);
    auto *temporary = new livehime::DanmakuFollow(&extraView, SequenceRole);
    QPointer<QPushButton> detachedButton = extraView.viewport()->findChild<QPushButton *>("danmakuUnread");
    delete temporary;
    check(detachedButton.isNull(), "removing the follow controller removes its overlay from a surviving viewport");
    auto *temporaryView = new QListView;
    temporaryView->setModel(&model);
    QPointer<livehime::DanmakuFollow> attached = new livehime::DanmakuFollow(temporaryView, SequenceRole);
    delete temporaryView;
    settle();
    check(attached.isNull(), "destroying the viewport with a pending restoration also destroys the controller");
    return failures ? 1 : 0;
}
