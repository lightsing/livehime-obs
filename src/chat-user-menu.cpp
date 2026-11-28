#include "chat-user-menu.hpp"
#include "room-realtime.hpp"
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QUrl>

namespace livehime {
void showChatUserMenu(QWidget *view, RoomRealtime *room, qint64 uid, const QString &name,
                      const QString &message, QPoint globalPos) {
    if (!room || uid <= 0)
        return;
    const QPointer<RoomRealtime> session(room);
    const auto epoch = room->contextEpoch();
    auto *menu = new QMenu(view);
    menu->setObjectName("chatUserMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(room, &RoomRealtime::contextChanged, menu, &QWidget::close);
    QObject::connect(room, &RoomRealtime::stopped, menu, &QWidget::close);
    auto manage = [&](QMenu *items, const QString &label, UserModeration operation, const QString &question,
                      int durationHours = 2) {
        auto *action = items->addAction(operation == UserModeration::Mute ? QStringLiteral("%1 小时").arg(durationHours) : label);
        action->setEnabled(room->canModerate(uid));
        QObject::connect(action, &QAction::triggered, view, [=] {
            if (!session || session->contextEpoch() != epoch || !session->canModerate(uid))
                return;
            auto *confirm = new QMessageBox(QMessageBox::Question, label, question.arg(name, QString::number(uid)),
                                            QMessageBox::Yes | QMessageBox::Cancel, view);
            confirm->setTextFormat(Qt::PlainText);
            confirm->setAttribute(Qt::WA_DeleteOnClose);
            confirm->setDefaultButton(QMessageBox::Cancel);
            confirm->setObjectName("chatUserConfirm");
            QObject::connect(session, &RoomRealtime::contextChanged, confirm, &QDialog::reject);
            QObject::connect(session, &RoomRealtime::stopped, confirm, &QDialog::reject);
            QObject::connect(confirm, &QDialog::finished, view, [=](int result) {
                if (result != QMessageBox::Yes || !session || session->contextEpoch() != epoch || !session->canModerate(uid))
                    return;
                auto *pending = new QObject(view);
                QObject::connect(session, &RoomRealtime::contextChanged, pending, &QObject::deleteLater);
                QObject::connect(session, &RoomRealtime::stopped, pending, &QObject::deleteLater);
                QObject::connect(session, &RoomRealtime::moderationFinished, pending, [=](bool ok, const QString &text) {
                    pending->deleteLater();
                    if (!ok || !session || session->contextEpoch() != epoch || !view->isVisible())
                        return;
                    auto *feedback = new QMessageBox(QMessageBox::Information,
                        label, QStringLiteral("%1（UID %2）\n%3").arg(name, QString::number(uid), text),
                        QMessageBox::Ok, view);
                    feedback->setTextFormat(Qt::PlainText);
                    feedback->setAttribute(Qt::WA_DeleteOnClose);
                    feedback->open();
                }, Qt::SingleShotConnection);
                session->moderateUser(operation, uid, epoch, message, durationHours);
            });
            confirm->open();
        });
    };
    manage(menu, QStringLiteral("拉黑"), UserModeration::Block,
           QStringLiteral("将「%1」（UID %2）加入黑名单？平台会解除关系，对方将无法与你互动，包括发言和送礼。"));
    auto *mute = menu->addMenu(QStringLiteral("禁言"));
    mute->menuAction()->setEnabled(room->canModerate(uid));
    for (int hours : {2, 4, 24}) {
        const QString duration = QStringLiteral("%1 小时").arg(hours);
        manage(mute, QStringLiteral("禁言 ") + duration, UserModeration::Mute,
               QStringLiteral("向平台申请在本直播间禁言「%1」（UID %2）") + duration + QStringLiteral("？"), hours);
    }
    manage(menu, QStringLiteral("设置房管"), UserModeration::AppointAdmin,
           QStringLiteral("将「%1」（UID %2）设为本直播间的普通房管？对方将能管理禁言和屏蔽词。"));
    menu->addSeparator();
    menu->addAction(QStringLiteral("访问空间"), view, [uid] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://space.bilibili.com/%1").arg(uid)));
    });
    menu->popup(globalPos);
}
} // namespace livehime
