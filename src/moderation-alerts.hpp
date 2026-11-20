#pragma once
#include "moderation-notice.hpp"
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>

class QWidget;
namespace livehime {
class RoomRealtime;
class ModerationDialog;

// Owned by the room service. The dialog belongs to the OBS main window even when its docks are hidden.
class ModerationAlerts final : public QObject {
  public:
    ModerationAlerts(RoomRealtime *service, QWidget *mainWindow);
    ~ModerationAlerts() override;

  private:
    void receive(const ModerationNotice &notice);
    void showNextWarning();
    void clear();
    QPointer<ModerationDialog> warning_, structured_;
    QQueue<ModerationNotice> pendingWarnings_;
    QStringList recentIds_;
};
} // namespace livehime
