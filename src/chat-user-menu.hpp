#pragma once
#include <QPoint>
#include <QString>

class QWidget;
namespace livehime {
class RoomRealtime;
void showChatUserMenu(QWidget *view, RoomRealtime *room, qint64 uid, const QString &name,
                      const QString &message, QPoint globalPos);
} // namespace livehime
