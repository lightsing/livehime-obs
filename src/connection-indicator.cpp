#include "connection-indicator.hpp"
#include "panel-palette.hpp"
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
    QString state;
    switch (connection.phase) {
    case Phase::Idle: state = QStringLiteral("idle"); break;
    case Phase::Connecting: state = QStringLiteral("connecting"); break;
    case Phase::Connected:
        state = QStringLiteral("connected");
        break;
    case Phase::Reconnecting:
        state = QStringLiteral("reconnecting");
        break;
    }
    setProperty("connectionState", state);
    refreshColors();
    setToolTip(QStringLiteral("<qt>") + connection.detail.toHtmlEscaped() + QStringLiteral("</qt>"));
    updateText();
}
void ConnectionIndicator::refreshColors() {
    using Phase = RealtimeConnection::Phase;
    const auto &c = PanelPalette::current();
    const bool warning = connection_.phase == Phase::Reconnecting;
    QString style;
    if (warning || connection_.phase == Phase::Connected) {
        const auto fill = warning ? c.retrying : c.connected;
        style = QStringLiteral("background-color:%1;color:%2;padding:3px 6px;border-radius:3px;font-weight:%3;")
            .arg(fill.name(), c.badgeText(fill).name(), warning ? QStringLiteral("bold") : QStringLiteral("normal"));
    }
    if (styleSheet() != style)
        setStyleSheet(style);
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
