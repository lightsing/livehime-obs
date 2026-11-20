#include "moderation-alerts.hpp"
#include "room-realtime.hpp"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace livehime {
class ModerationDialog final : public QDialog {
  public:
    explicit ModerationDialog(QWidget *parent);
    void display(const ModerationNotice &notice);
    void clearNotice();
    ModerationNotice::Kind kind = ModerationNotice::Kind::Warning;
    qint64 timestamp = 0;
  private:
    QLabel *title_;
    QTextBrowser *body_;
    QDialogButtonBox *buttons_;
};

ModerationDialog::ModerationDialog(QWidget *parent) : QDialog(parent) {
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(28, 28));
    header->addWidget(icon);
    title_ = new QLabel(this);
    title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true);
    auto font = title_->font();
    font.setBold(true);
    title_->setFont(font);
    header->addWidget(title_, 1);
    layout->addLayout(header);

    body_ = new QTextBrowser(this);
    body_->setFrameShape(QFrame::NoFrame);
    body_->setOpenLinks(false);
    body_->setOpenExternalLinks(false);
    body_->setAccessibleName(QStringLiteral("通知内容"));
    connect(body_, &QTextBrowser::anchorClicked, this, &QDesktopServices::openUrl);
    layout->addWidget(body_, 1);
    buttons_ = new QDialogButtonBox(this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons_);
}

void ModerationDialog::clearNotice() {
    hide();
    setWindowTitle({});
    title_->clear();
    body_->clear();
    buttons_->clear();
}

void ModerationDialog::display(const ModerationNotice &notice) {
    kind = notice.kind;
    timestamp = notice.timestamp;
    setWindowTitle(notice.title);
    title_->setText(notice.title);
    body_->setPlainText(notice.body);
    QTextCursor cursor(body_->document());
    cursor.movePosition(QTextCursor::End);
    const auto palette = body_->palette();
    const auto foreground = palette.color(QPalette::Text);
    const auto background = palette.color(QPalette::Base);
    const auto readable = [&](QPalette::ColorRole role) {
        const auto color = palette.color(role);
        return qAbs(qGray(color.rgb()) - qGray(background.rgb())) >= 80 ? color : foreground;
    };
    for (const auto &line : notice.tips) {
        if (!body_->document()->isEmpty())
            cursor.insertBlock();
        for (const auto &part : line) {
            QTextCharFormat format;
            if (!part.url.isEmpty()) {
                format.setAnchor(true);
                format.setAnchorHref(part.url.toString());
                format.setForeground(readable(QPalette::Link));
                format.setFontUnderline(true);
            } else {
                format.setForeground(readable(QPalette::PlaceholderText));
            }
            cursor.insertText(part.text, format);
        }
    }
    buttons_->clear();
    bool hasDismiss = false;
    for (const auto &action : notice.actions) {
        if (action.url.isEmpty()) {
            buttons_->addButton(action.text, QDialogButtonBox::AcceptRole);
            hasDismiss = true;
        } else {
            auto *button = buttons_->addButton(action.text, QDialogButtonBox::ActionRole);
            connect(button, &QPushButton::clicked, this, [url = action.url] { QDesktopServices::openUrl(url); });
        }
    }
    if (!hasDismiss)
        buttons_->addButton(QStringLiteral("我知道了"), QDialogButtonBox::AcceptRole);
    const QSize available = parentWidget()->screen()->availableGeometry().size();
    const int width = qMin(440, available.width());
    body_->document()->setTextWidth(qMax(1, width - 60));
    const int height = qBound(150, qCeil(body_->document()->size().height()) + 124, 500);
    resize(width, qMin(available.height(), height));
    if (isHidden())
        open();
    QApplication::alert(parentWidget());
#ifdef Q_OS_WIN
    if (notice.kind == ModerationNotice::Kind::Warning && QApplication::platformName() == "windows")
        MessageBeep(MB_ICONWARNING);
#endif
}

ModerationAlerts::ModerationAlerts(RoomRealtime *service, QWidget *mainWindow)
    : QObject(service), warning_(new ModerationDialog(mainWindow)), structured_(new ModerationDialog(mainWindow)) {
    warning_->setObjectName(QStringLiteral("livehime-moderation-dialog"));
    structured_->setObjectName(QStringLiteral("livehime-structured-moderation-dialog"));
    connect(service, &RoomRealtime::moderationNotice, this, &ModerationAlerts::receive);
    connect(service, &RoomRealtime::contextChanged, this, &ModerationAlerts::clear);
    connect(service, &RoomRealtime::stopped, this, &ModerationAlerts::clear);
    connect(warning_, &QDialog::finished, this, [this] {
        QTimer::singleShot(0, this, &ModerationAlerts::showNextWarning);
    });
}

ModerationAlerts::~ModerationAlerts() {
    delete warning_.data();
    delete structured_.data();
}

void ModerationAlerts::clear() {
    pendingWarnings_.clear();
    recentIds_.clear();
    if (warning_)
        warning_->clearNotice();
    if (structured_)
        structured_->clearNotice();
}

void ModerationAlerts::receive(const ModerationNotice &notice) {
    if (!warning_ || !structured_)
        return;
    using Kind = ModerationNotice::Kind;
    const bool isStructured = notice.kind == Kind::Ecology || notice.kind == Kind::CutOffV2;
    if (isStructured && !structured_->isHidden()) {
        if (structured_->kind == Kind::CutOffV2 && notice.kind == Kind::Ecology)
            return;
        if (structured_->kind == notice.kind && notice.timestamp <= structured_->timestamp)
            return;
    }
    if (!notice.id.isEmpty()) {
        if (recentIds_.contains(notice.id))
            return;
        recentIds_.append(notice.id);
        if (recentIds_.size() > 128)
            recentIds_.removeFirst();
    }
    if (isStructured) {
        structured_->display(notice);
        return;
    }
    if (!structured_->isHidden() && structured_->kind == Kind::Ecology)
        structured_->clearNotice();
    if (pendingWarnings_.size() >= 32)
        pendingWarnings_.dequeue();
    pendingWarnings_.enqueue(notice);
    showNextWarning();
}

void ModerationAlerts::showNextWarning() {
    if (warning_ && warning_->isHidden() && !pendingWarnings_.isEmpty())
        warning_->display(pendingWarnings_.dequeue());
}
} // namespace livehime
