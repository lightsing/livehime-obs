#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

class QListView;
class QPushButton;

namespace livehime {
// Attach after setModel(). The sequence role identifies a received message across
// model resets. The visible rows are ordered by increasing sequence.
class DanmakuFollow final : public QObject {
    Q_OBJECT

  public:
    DanmakuFollow(QListView *view, int sequenceRole);
    ~DanmakuFollow() override;
    void messagesArrived(int count);
    void beginRebuild();
    void endRebuild();
    void setPaused(bool paused);
    void reset();

    qint64 unreadCount() const { return unread_; }
    bool isFollowing() const { return following_; }

  signals:
    void latestRequested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void scheduleRestore();
    void restorePosition();
    void rememberPosition();
    void scrollChanged();
    void updateBubble();
    void jumpToLatest();

    QListView *view_;
    QPointer<QPushButton> bubble_;
    const int sequenceRole_;
    QTimer restoreTimer_, inputTimer_;
    qint64 unread_ = 0, anchorSequence_ = 0;
    int anchorRow_ = -1, anchorTop_ = 0, rebuildDepth_ = 0;
    bool following_ = true, paused_ = false, restoring_ = false, forceBottom_ = false;
};
} // namespace livehime
