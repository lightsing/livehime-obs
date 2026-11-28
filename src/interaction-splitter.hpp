#pragma once
#include "panel-palette.hpp"

#include <QKeyEvent>
#include <QPainter>
#include <QSplitter>

namespace livehime {
class InteractionSplitter final : public QSplitter {
  public:
    explicit InteractionSplitter(QWidget *parent) : QSplitter(Qt::Vertical, parent) {
        setObjectName("interactionSplitter");
        setChildrenCollapsible(false);
        setHandleWidth(12);
        setOpaqueResize(true);
    }

  protected:
    QSplitterHandle *createHandle() override {
        class Handle final : public QSplitterHandle {
          public:
            explicit Handle(QSplitter *splitter) : QSplitterHandle(Qt::Vertical, splitter) {
                setAccessibleName(QStringLiteral("调整弹幕和进场区高度"));
                setToolTip(QStringLiteral("上下拖动调整弹幕和进场区高度"));
                setFocusPolicy(Qt::StrongFocus);
                setCursor(Qt::SplitVCursor);
            }
          protected:
            void paintEvent(QPaintEvent *) override {
                QPainter p(this);
                p.setRenderHint(QPainter::Antialiasing);
                const QPoint mid = rect().center();
                const auto &colors = PanelPalette::current();
                const bool active = hasFocus() || underMouse();
                p.fillRect(0, mid.y() - 1, width(), 2, active ? colors.accent : colors.divider);
                const QRect grip(mid.x() - 10, mid.y() - 4, 21, 9);
                p.setPen(Qt::NoPen);
                const auto gripColor = active ? colors.accent : colors.muted;
                p.setBrush(gripColor);
                p.drawRoundedRect(grip, 3, 3);
                p.setPen(QPen(colors.readable(colors.surface, gripColor, colors.text), 1));
                p.drawLine(mid.x() - 3, mid.y() - 1, mid.x() + 3, mid.y() - 1);
                p.drawLine(mid.x() - 3, mid.y() + 1, mid.x() + 3, mid.y() + 1);
            }
            void keyPressEvent(QKeyEvent *event) override {
                if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) {
                    moveSplitter(y() + (event->key() == Qt::Key_Up ? -12 : 12));
                    event->accept();
                    return;
                }
                QSplitterHandle::keyPressEvent(event);
            }
        };
        return new Handle(this);
    }
};
} // namespace livehime
