#pragma once
#include "room-realtime.hpp"
#include <QLabel>

namespace livehime {
class ConnectionIndicator final : public QLabel {
  public:
    explicit ConnectionIndicator(RoomRealtime *service, QWidget *parent = nullptr);
  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
  private:
    void display(const RealtimeConnection &connection);
    void updateText();
    RealtimeConnection connection_;
    QTimer countdown_;
};
} // namespace livehime
