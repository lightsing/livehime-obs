#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

class QDockWidget;
class QLineEdit;
class QPushButton;

namespace livehime {
class DanmakuInputRow final : public QWidget {
  public:
    DanmakuInputRow(QDockWidget *dock, QLineEdit *input, QPushButton *send, QWidget *parent);
    void setPassthrough(bool enabled);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void syncWindow();

    QPointer<QDockWidget> dock_;
    QWidget *controls_;
    QLineEdit *input_;
    QPushButton *send_;
    QTimer layoutTimer_;
    bool passthrough_ = false;
};
} // namespace livehime
