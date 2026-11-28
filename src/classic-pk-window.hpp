#pragma once
#include "classic-pk.hpp"
#include <QDialog>
#include <QTimer>

class QCheckBox;
class QLabel;
class QPushButton;
namespace livehime {
class ClassicPkWindow final : public QDialog {
public:
    explicit ClassicPkWindow(ClassicPkController *pk, QWidget *parent);
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void updateView();
    void updateClock();
    ClassicPkController *pk_;
    QLabel *phase_, *clock_, *home_, *away_, *homeVotes_, *awayVotes_, *message_;
    QPushButton *primary_, *back_, *refresh_;
    QCheckBox *continuous_;
    QTimer clockTimer_;
    quint64 exitContext_ = 0;
    qint64 exitId_ = 0;
};
} // namespace livehime
