#include "classic-pk-window.hpp"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace livehime {
namespace {
QLabel *label(QWidget *parent, bool centered = false)
{
    auto *text = new QLabel(parent);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    if (centered) text->setAlignment(Qt::AlignCenter);
    return text;
}
}

ClassicPkWindow::ClassicPkWindow(ClassicPkController *pk, QWidget *parent)
    : QDialog(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                      Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint), pk_(pk)
{
    setWindowTitle(QStringLiteral("双人经典 PK"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(460, 310);
    auto *layout = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    phase_ = label(this);
    clock_ = label(this);
    header->addWidget(phase_, 1);
    header->addWidget(clock_);
    layout->addLayout(header);
    auto *scores = new QHBoxLayout;
    auto addSide = [&](const QString &title, QLabel *&name, QLabel *&votes) {
        auto *box = new QGroupBox(title, this);
        auto *column = new QVBoxLayout(box);
        name = label(box, true);
        votes = label(box, true);
        auto font = votes->font();
        font.setPixelSize(32);
        font.setBold(true);
        votes->setFont(font);
        column->addWidget(name);
        column->addWidget(votes);
        scores->addWidget(box, 1);
    };
    addSide(QStringLiteral("本方"), home_, homeVotes_);
    addSide(QStringLiteral("对手"), away_, awayVotes_);
    layout->addLayout(scores, 1);
    message_ = label(this);
    layout->addWidget(message_);
    continuous_ = new QCheckBox(QStringLiteral("连续匹配"), this);
    layout->addWidget(continuous_);
    auto *actions = new QHBoxLayout;
    refresh_ = new QPushButton(QStringLiteral("刷新状态"), this);
    back_ = new QPushButton(QStringLiteral("返回"), this);
    primary_ = new QPushButton(this);
    actions->addWidget(refresh_);
    actions->addStretch();
    actions->addWidget(back_);
    actions->addWidget(primary_);
    layout->addLayout(actions);
    connect(refresh_, &QPushButton::clicked, pk_, &ClassicPkController::refresh);
    connect(continuous_, &QCheckBox::clicked, this, [this](bool checked) {
        pk_->setContinuous(checked);
        updateView();
    });
    connect(back_, &QPushButton::clicked, this, [this] {
        exitId_ = 0;
        pk_->confirmPrecheck(false);
        updateView();
    });
    connect(primary_, &QPushButton::clicked, this, [this] {
        if (!pk_->precheckText().isEmpty()) pk_->confirmPrecheck(true);
        else if (pk_->canStart()) pk_->start();
        else if (pk_->phase() == ClassicPkController::Phase::Matching) pk_->cancelMatch();
        else if (pk_->canExit()) {
            if (exitId_ == pk_->battleId() && exitContext_ == pk_->contextVersion()) {
                exitId_ = 0;
                pk_->exitBattle();
            } else {
                exitId_ = pk_->battleId();
                exitContext_ = pk_->contextVersion();
            }
            updateView();
        }
    });
    connect(pk_, &ClassicPkController::changed, this, &ClassicPkWindow::updateView);
    clockTimer_.setInterval(250);
    connect(&clockTimer_, &QTimer::timeout, this, &ClassicPkWindow::updateClock);
    updateView();
}

void ClassicPkWindow::updateClock()
{
    const auto secs = pk_->secondsLeft();
    clock_->setText(secs >= 0 ? QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0')) : QString{});
}

void ClassicPkWindow::updateView()
{
    if (!pk_->canExit() || pk_->battleId() != exitId_ || pk_->contextVersion() != exitContext_) exitId_ = 0;
    phase_->setText(pk_->phaseLabel());
    auto show = [](const PkSide &side, QLabel *name, QLabel *votes) {
        name->setText(side.name);
        votes->setText(side.votes ? QString::number(*side.votes) : QStringLiteral("—"));
    };
    show(pk_->home(), home_, homeVotes_);
    show(pk_->away(), away_, awayVotes_);
    const bool prompt = !pk_->precheckText().isEmpty();
    message_->setText(prompt ? pk_->precheckText() : exitId_ ? QStringLiteral("确定提前结束本场 PK？") : pk_->message());
    message_->setVisible(!message_->text().isEmpty());
    back_->setVisible(prompt || exitId_);
    const bool matching = pk_->phase() == ClassicPkController::Phase::Matching;
    const bool inBattle = pk_->battleId() && !matching;
    QString action = QStringLiteral("开启匹配");
    if (prompt) action = pk_->confirmText().isEmpty() ? QStringLiteral("继续匹配") : pk_->confirmText();
    else if (matching) action = QStringLiteral("取消匹配");
    else if (exitId_) action = QStringLiteral("确认结束");
    else if (inBattle) action = QStringLiteral("结束 PK");
    primary_->setText(action);
    primary_->setEnabled(prompt || pk_->canStart() || (matching && pk_->actionable()) || pk_->canExit());
    refresh_->setEnabled(pk_->online());
    const QSignalBlocker block(continuous_);
    continuous_->setChecked(pk_->continuous());
    continuous_->setEnabled(pk_->canSetContinuous());
    continuous_->setToolTip(pk_->switchHint());
    updateClock();
}
void ClassicPkWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    clockTimer_.start();
    updateView();
    pk_->refresh();
}
void ClassicPkWindow::hideEvent(QHideEvent *event)
{
    clockTimer_.stop();
    exitId_ = 0;
    pk_->confirmPrecheck(false);
    QDialog::hideEvent(event);
}
} // namespace livehime
