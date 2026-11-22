#include "connection-indicator.hpp"
#include <QVariant>

namespace livehime {
ConnectionIndicator::ConnectionIndicator(RoomRealtime *service, QWidget *parent) : QLabel(parent) {
    setObjectName("realtimeConnectionIndicator");
    setAccessibleName(QStringLiteral("弹幕连接状态"));
    setWordWrap(true);
    setTextFormat(Qt::PlainText);
    countdown_.setSingleShot(true);
    countdown_.setTimerType(Qt::PreciseTimer);
    connect(&countdown_, &QTimer::timeout, this, &ConnectionIndicator::updateText);
    if (service) {
        connect(service, &RoomRealtime::connectionChanged, this, &ConnectionIndicator::display);
        display(service->connection());
    } else {
        display({});
    }
}
void ConnectionIndicator::display(const RealtimeConnection &connection) {
    using Phase = RealtimeConnection::Phase;
    connection_ = connection;
    QString state, style;
    switch (connection.phase) {
    case Phase::Idle: state = QStringLiteral("idle"); break;
    case Phase::Connecting: state = QStringLiteral("connecting"); break;
    case Phase::Connected:
        state = QStringLiteral("connected");
        style = QStringLiteral("background-color:#21563b;color:white;padding:3px 6px;border-radius:3px;");
        break;
    case Phase::Reconnecting:
        state = QStringLiteral("reconnecting");
        style = QStringLiteral("background-color:#a32634;color:white;font-weight:bold;padding:4px 6px;border-radius:3px;");
        break;
    }
    setProperty("connectionState", state);
    if (styleSheet() != style)
        setStyleSheet(style);
    setToolTip(QStringLiteral("<qt>") + connection.detail.toHtmlEscaped() + QStringLiteral("</qt>"));
    updateText();
}
void ConnectionIndicator::updateText() {
    countdown_.stop();
    if (connection_.phase != RealtimeConnection::Phase::Reconnecting) {
        setText(connection_.detail);
        return;
    }
    const auto remaining = connection_.retryAt.remainingTime();
    QString activity;
    if (connection_.retryAt.isForever()) {
        activity = QStringLiteral("正在进行第 %1 次重连").arg(connection_.attempt);
    } else if (remaining > 0) {
        activity = QStringLiteral("%1 秒后进行第 %2 次重连").arg((remaining + 999) / 1000).arg(connection_.attempt);
        if (isVisible())
            countdown_.start(int((remaining - 1) % 1000 + 1));
    } else {
        activity = QStringLiteral("即将进行第 %1 次重连").arg(connection_.attempt);
    }
    setText(QStringLiteral("弹幕连接中断 · ") + activity);
}
void ConnectionIndicator::showEvent(QShowEvent *event) {
    QLabel::showEvent(event);
    updateText();
}
void ConnectionIndicator::hideEvent(QHideEvent *event) {
    countdown_.stop();
    QLabel::hideEvent(event);
}
} // namespace livehime
