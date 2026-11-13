#include "room-dock.hpp"
#include "cover-crop-dialog.hpp"
#include "login-dialog.hpp"
#include "native-stream-controls.hpp"
#include <QBuffer>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace livehime {
class CoverButton final : public QPushButton {
  public:
    explicit CoverButton(QWidget *parent) : QPushButton(parent) {
        setText(QStringLiteral("选择图片"));
        setAccessibleName(QStringLiteral("选择直播封面"));
        setObjectName(QStringLiteral("cover-preview"));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setFixedHeight(190);
        // OBS themes cap ordinary QPushButton height. This image surface has
        // its own geometry and painting, so clear the inherited box metrics.
        setStyleSheet(QStringLiteral("#cover-preview { height: 190px; min-height: 190px; max-height: 190px; "
                                     "margin: 0; padding: 0; border: 0; }"));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setPreview(const QPixmap &preview) {
        preview_ = preview;
        update();
    }
    void setMessage(const QString &message) {
        message_ = message;
        update();
    }

  protected:
    bool event(QEvent *event) override {
        const bool result = QPushButton::event(event);
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
            event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
            event->type() == QEvent::EnabledChange)
            update();
        return result;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath clip;
        clip.addRoundedRect(bounds, 7, 7);
        painter.setClipPath(clip);
        painter.fillRect(bounds, palette().color(QPalette::Base));
        if (!preview_.isNull()) {
            const QPixmap scaled =
                preview_.scaled(bounds.size().toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            painter.drawPixmap((width() - scaled.width()) / 2, (height() - scaled.height()) / 2, scaled);
        } else {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(bounds.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, message_);
        }
        if (isEnabled() && (underMouse() || hasFocus())) {
            painter.fillRect(bounds, QColor(0, 0, 0, 125));
            const QRectF label(width() / 2.0 - 58, height() / 2.0 - 21, 116, 42);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 235));
            painter.drawRoundedRect(label, 6, 6);
            painter.setPen(QColor(30, 33, 39));
            painter.drawText(label, Qt::AlignCenter, QStringLiteral("选择图片"));
        }
        painter.setClipping(false);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(
            QPen(hasFocus() ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid),
                 hasFocus() ? 2 : 1));
        painter.drawRoundedRect(bounds, 7, 7);
    }

  private:
    QPixmap preview_;
    QString message_;
};

RoomDock::RoomDock(const QString &storageDirectory, QWidget *parent)
    : RoomDock(new LiveController(storageDirectory), parent) {
    controller_->setParent(this);
    QTimer::singleShot(0, controller_, &LiveController::initialize);
}
RoomDock::RoomDock(LiveController *controller, QWidget *parent)
    : QWidget(parent), controller_(controller), nativeControls_(new NativeStreamControls(this)) {
    setMinimumWidth(300);
    auto *layout = new QVBoxLayout(this);
    account_ = new QLabel(QStringLiteral("未登录"), this);
    account_->setTextFormat(Qt::PlainText);
    layout->addWidget(account_);
    auto *accountButtons = new QHBoxLayout;
    login_ = new QPushButton(QStringLiteral("扫码登录"), this);
    logout_ = new QPushButton(QStringLiteral("退出登录"), this);
    refresh_ = new QPushButton(QStringLiteral("刷新状态"), this);
    accountButtons->addWidget(login_);
    accountButtons->addWidget(logout_);
    accountButtons->addWidget(refresh_);
    layout->addLayout(accountButtons);
    roomLabel_ = new QLabel(QStringLiteral("房间尚未加载"), this);
    roomLabel_->setTextFormat(Qt::PlainText);
    layout->addWidget(roomLabel_);
    auto *form = new QFormLayout;
    title_ = new QLineEdit(this);
    title_->setObjectName(QStringLiteral("title-edit"));
    title_->setPlaceholderText(QStringLiteral("直播标题"));
    title_->setToolTip(QStringLiteral("停止输入约 1 秒、离开输入框或点击开播时自动同步"));
    title_->setMaxLength(80);
    form->addRow(QStringLiteral("标题"), title_);
    titleHint_ = new QLabel(this);
    titleHint_->setTextFormat(Qt::PlainText);
    titleHint_->setWordWrap(true);
    titleHint_->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), titleHint_);
    parentArea_ = new QComboBox(this);
    parentArea_->setObjectName(QStringLiteral("parent-area-combo"));
    parentArea_->setPlaceholderText(QStringLiteral("选择父分类"));
    parentArea_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    parentArea_->setMinimumContentsLength(12);
    form->addRow(QStringLiteral("父分类"), parentArea_);
    area_ = new QComboBox(this);
    area_->setObjectName(QStringLiteral("area-combo"));
    area_->setEditable(true);
    area_->setInsertPolicy(QComboBox::NoInsert);
    area_->setMaxVisibleItems(18);
    area_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    area_->setMinimumContentsLength(12);
    area_->lineEdit()->setPlaceholderText(QStringLiteral("输入名称搜索子分区"));
    area_->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    area_->completer()->setCompletionMode(QCompleter::PopupCompletion);
    area_->completer()->setFilterMode(Qt::MatchContains);
    form->addRow(QStringLiteral("子分区"), area_);
    layout->addLayout(form);
    areaHint_ = new QLabel(this);
    areaHint_->setWordWrap(true);
    areaHint_->setTextFormat(Qt::PlainText);
    layout->addWidget(areaHint_);
    coverPreview_ = new CoverButton(this);
    layout->addWidget(coverPreview_);
    review_ = new QLabel(this);
    review_->setTextFormat(Qt::PlainText);
    review_->setWordWrap(true);
    layout->addWidget(review_);
    retry_ = new QPushButton(QStringLiteral("重试 OBS 连接（当前场次）"), this);
    retry_->setObjectName(QStringLiteral("retry-connection-button"));
    layout->addWidget(retry_);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);
    verification_ = new QPushButton(QStringLiteral("打开平台核验页面"), this);
    layout->addWidget(verification_);
    auto *roomButton = new QPushButton(QStringLiteral("打开我的直播间"), this);
    layout->addWidget(roomButton);
    auto *realtimeButtons = new QHBoxLayout;
    const QStringList names{QStringLiteral("直播互动"), QStringLiteral("房间观众"), QStringLiteral("大航海")};
    for (int i = 0; i < names.size(); ++i) {
        auto *button = new QPushButton(names.at(i), this);
        button->setObjectName(QStringLiteral("realtime-window-%1").arg(i));
        connect(button, &QPushButton::clicked, this, [this, i] { emit openRealtimeWindow(i); });
        realtimeButtons->addWidget(button);
    }
    layout->addLayout(realtimeButtons);
    auto *unlockInteraction = new QPushButton(QStringLiteral("解除互动穿透"), this);
    unlockInteraction->setObjectName(QStringLiteral("realtime-unlock-interaction"));
    unlockInteraction->setToolTip(QStringLiteral("恢复直播互动窗口的鼠标操作并显示窗口"));
    connect(unlockInteraction, &QPushButton::clicked, this, &RoomDock::unlockRealtimeInteraction);
    layout->addWidget(unlockInteraction);
    layout->addStretch();

    connect(login_, &QPushButton::clicked, this, &RoomDock::login);
    connect(logout_, &QPushButton::clicked, controller_, &LiveController::logout);
    connect(refresh_, &QPushButton::clicked, controller_, &LiveController::refresh);
    connect(coverPreview_, &QPushButton::clicked, this, &RoomDock::selectCover);
    connect(nativeControls_, &NativeStreamControls::startRequested, this, &RoomDock::nativeStart);
    connect(nativeControls_, &NativeStreamControls::stopRequested, this, &RoomDock::nativeStop);
    connect(retry_, &QPushButton::clicked, controller_, &LiveController::retryConnection);
    connect(title_, &QLineEdit::textChanged, this, &RoomDock::updateView);
    connect(title_, &QLineEdit::textEdited, controller_, &LiveController::setTitleDraft);
    connect(title_, &QLineEdit::editingFinished, controller_, &LiveController::flushTitle);
    connect(parentArea_, &QComboBox::currentIndexChanged, this, [this] {
        areaEdited_ = true;
        updateChildAreas();
        updateView();
    });
    connect(area_, &QComboBox::activated, this, [this] {
        areaEdited_ = true;
        updateView();
    });
    connect(area_->lineEdit(), &QLineEdit::textEdited, this, [this] {
        areaEdited_ = true;
        updateView();
    });
    connect(area_->completer(), qOverload<const QString &>(&QCompleter::activated), this,
            [this](const QString &text) {
                const int index = area_->findText(text);
                if (index >= 0)
                    area_->setCurrentIndex(index);
                areaEdited_ = true;
                updateView();
            });
    connect(verification_, &QPushButton::clicked, this, [this] {
        const QUrl url = protocol::safeUserUrl(controller_->verificationUrl().toString());
        if (!url.isEmpty())
            QDesktopServices::openUrl(url);
    });
    connect(roomButton, &QPushButton::clicked, this, [this] {
        if (controller_->room().roomId > 0)
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://live.bilibili.com/%1").arg(controller_->room().roomId)));
    });
    connect(controller_, &LiveController::changed, this, &RoomDock::updateView);
    connect(controller_, &LiveController::roomChanged, this, &RoomDock::updateRoom);
    connect(controller_, &LiveController::areasChanged, this, &RoomDock::updateAreas);
    connect(controller_, &LiveController::coverChanged, this, &RoomDock::updateCover);
    connect(controller_, &LiveController::titleSyncChanged, this, &RoomDock::updateTitle);
    updateTitle();
    showCover();
    updateView();
}

void RoomDock::setRealtimeAvailable(bool available) {
    for (int i = 0; i < 3; ++i) {
        if (auto *button = findChild<QPushButton *>(QStringLiteral("realtime-window-%1").arg(i))) {
            button->setEnabled(available);
            if (!available)
                button->setToolTip(
                    QStringLiteral("实时停靠面板注册失败，请检查是否存在另一份同名插件后重启 OBS。"));
        }
    }
    if (auto *button = findChild<QPushButton *>(QStringLiteral("realtime-unlock-interaction")))
        button->setEnabled(available);
}
void RoomDock::updateView() {
    const bool working = controller_->working(), auth = controller_->authenticated();
    const bool remote = controller_->hasRemoteSession();
    const auto state = controller_->state();
    account_->setText(
        auth ? QStringLiteral("%1 · UID %2").arg(controller_->user().name).arg(controller_->user().uid)
             : QStringLiteral("未登录或登录待验证"));
    login_->setEnabled(!working && (!auth || (!remote && !controller_->outputBusy())));
    logout_->setEnabled(auth && !working && !remote && !controller_->outputBusy());
    refresh_->setEnabled(!working);
    const bool editable =
        auth && (!working || controller_->metadataBusy()) && state != LiveController::State::Stopping &&
        state != LiveController::State::Connecting && state != LiveController::State::Preparing;
    title_->setEnabled(editable);
    parentArea_->setEnabled(editable && !remote && parentArea_->count() > 0);
    area_->setEnabled(editable && !remote && parentArea_->currentIndex() >= 0 && area_->count() > 0);
    coverPreview_->setEnabled(editable && controller_->room().roomId > 0);
    retry_->setVisible(controller_->hasStreamCredentials() && state == LiveController::State::Recoverable);
    retry_->setEnabled(auth && !working && !controller_->outputBusy());
    status_->setText(controller_->status());
    verification_->setVisible(!controller_->verificationUrl().isEmpty());
    updateNativeControls();
}

bool RoomDock::attachNativeControls(QWidget *mainWindow, QString *error) {
    if (!nativeControls_->attach(mainWindow, error))
        return false;
    nativeControlsAttached_ = true;
    updateNativeControls();
    return true;
}

bool RoomDock::canStart() const {
    return controller_->authenticated() && controller_->state() == LiveController::State::Ready &&
           (!controller_->working() || controller_->metadataBusy()) && !controller_->hasRemoteSession() &&
           !controller_->outputBusy() && controller_->room().roomId > 0 &&
           !title_->text().trimmed().isEmpty() && selectedAreaId() > 0;
}

void RoomDock::nativeStart() {
    if (canStart())
        controller_->start(title_->text(), {}, selectedAreaId());
    updateNativeControls();
}

void RoomDock::nativeStop() {
    if (controller_->canStop())
        controller_->stop();
    updateNativeControls();
}

void RoomDock::updateNativeControls() {
    if (!nativeControlsAttached_)
        return;
    using Action = NativeStreamControls::Action;
    using State = LiveController::State;
    const auto state = controller_->state();
    if (controller_->outputBusy() && !controller_->ownsOutput()) {
        // Another service may have been started through a hotkey or frontend
        // API. Leave stopping that unrelated output to OBS itself.
        nativeControls_->setState(Action::PassThrough, true, {}, {});
    } else if (state == State::Stopping) {
        nativeControls_->setState(Action::Busy, false, QStringLiteral("正在停止直播…"),
                                  controller_->status());
    } else if (state == State::Preparing && !controller_->hasRemoteSession()) {
        const bool cancel = controller_->canCancelStart();
        nativeControls_->setState(cancel ? Action::CancelStart : Action::Busy, cancel,
                                  cancel ? QStringLiteral("取消开播") : QStringLiteral("正在申请开播…"),
                                  controller_->status());
    } else if (controller_->hasRemoteSession()) {
        const bool canStop = controller_->canStop();
        nativeControls_->setState(
            canStop ? Action::Stop : Action::Busy, canStop,
            state == State::Uncertain ? QStringLiteral("重试 / 确认停止直播") : QStringLiteral("停止直播"),
            QStringLiteral("停止 OBS 输出后，通知 B 站下播。\n") + controller_->status());
    } else {
        const QString hint = !controller_->authenticated() ? QStringLiteral("请先在“B站直播”面板扫码登录。")
                             : canStart() ? QStringLiteral("同步直播资料，向 B 站申请开播后启动 OBS 推流。")
                                          : QStringLiteral("请在“B站直播”面板检查状态、标题和子分区。\n") +
                                                controller_->status();
        nativeControls_->setState(Action::Start, canStart(), QStringLiteral("开始直播"), hint);
    }
}

void RoomDock::updateRoom() {
    const auto &room = controller_->room();
    if (displayedRoom_ != room.roomId) {
        displayedRoom_ = room.roomId;
        areaEdited_ = false;
    }
    roomLabel_->setText(room.roomId ? QStringLiteral("直播间 %1").arg(room.roomId)
                                    : QStringLiteral("房间尚未加载"));
    updateTitle();
    updateAreas();
    const QString coverStatus = room.coverStatus == 0    ? QStringLiteral("审核中")
                                : room.coverStatus == -1 ? QStringLiteral("审核未通过")
                                : room.coverStatus == 1  ? QStringLiteral("已设置")
                                                         : QStringLiteral("状态待确认");
    review_->setText(room.roomId ? QStringLiteral("封面：%1%2")
                                       .arg(coverStatus, room.blockMessage.isEmpty()
                                                             ? QString()
                                                             : QStringLiteral("\n") + room.blockMessage)
                                 : QString());
    review_->setToolTip(
        QStringLiteral("平台原始状态：标题 %1，封面 %2").arg(room.titleStatus).arg(room.coverStatus));
    showCover();
    updateView();
}

void RoomDock::login() {
    if (loginDialog_) {
        loginDialog_->raise();
        loginDialog_->activateWindow();
        return;
    }
    auto *dialog = new LoginDialog(controller_->browserDataDirectory(), controller_->deviceId(), this);
    loginDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &LoginDialog::cookiesReady, controller_, &LiveController::acceptCookies);
    connect(dialog, &LoginDialog::loginFailed, this,
            [this](const QString &message) { status_->setText(message); });
    dialog->open();
}

void RoomDock::selectCover() {
    const qint64 account = controller_->user().uid;
    const qint64 room = controller_->room().roomId;
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择直播封面"), {},
                                                      QStringLiteral("图片 (*.jpg *.jpeg *.png *.webp)"));
    if (file.isEmpty())
        return;
    const QFileInfo info(file);
    if (info.size() <= 0 || info.size() > 30 * 1024 * 1024) {
        status_->setText(QStringLiteral("请选择小于 30 MB 的本地图片。"));
        return;
    }
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || static_cast<qint64>(size.width()) * size.height() > 80000000) {
        status_->setText(QStringLiteral("图片尺寸无效或超过 8000 万像素。"));
        return;
    }
    if (size.width() > 4096 || size.height() > 4096)
        reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
    const QImage source = reader.read();
    if (source.isNull()) {
        status_->setText(QStringLiteral("无法读取此图片。"));
        return;
    }
    CoverCropDialog crop(source, this);
    if (crop.exec() != QDialog::Accepted)
        return;
    if (!controller_->authenticated() || controller_->user().uid != account ||
        controller_->room().roomId != room) {
        status_->setText(QStringLiteral("账号或直播间已变化，请重新选择封面。"));
        return;
    }
    const QImage image = crop.croppedImage();
    if (!coverDirectory_.isValid() || image.isNull()) {
        status_->setText(QStringLiteral("无法生成裁切后的封面，请重新选择。"));
        return;
    }
    const QString path =
        coverDirectory_.filePath(QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".png"));
    if (!image.save(path, "PNG")) {
        status_->setText(QStringLiteral("无法保存裁切图片，请检查临时目录。"));
        return;
    }
    controller_->saveCover(path);
}

void RoomDock::updateTitle() {
    if (title_->text() != controller_->titleDraft()) {
        const QSignalBlocker blocker(title_);
        title_->setText(controller_->titleDraft());
    }
    titleHint_->setText(controller_->titleSyncStatus());
    titleHint_->setVisible(!titleHint_->text().isEmpty());
    updateView();
}

int RoomDock::selectedAreaId() const {
    const int index = area_->currentIndex();
    return index >= 0 && parentArea_->currentIndex() >= 0 && area_->currentText() == area_->itemText(index) &&
                   area_->itemData(index, Qt::UserRole + 1).toInt() == parentArea_->currentData().toInt()
               ? area_->itemData(index).toInt()
               : 0;
}

void RoomDock::updateAreas() {
    const auto &room = controller_->room();
    const bool keepSelection = areaEdited_ && parentArea_->currentIndex() >= 0;
    const int selection = keepSelection ? selectedAreaId() : room.areaId;
    int parentId = keepSelection ? parentArea_->currentData().toInt() : room.parentAreaId;
    const bool keepSearch = keepSelection && !selection;
    const QString search = keepSearch ? area_->currentText() : QString();
    for (const auto &item : controller_->areas())
        if (item.id == selection) {
            parentId = item.parentId;
            break;
        }

    const QSignalBlocker blocker(parentArea_);
    parentArea_->clear();
    for (const auto &item : controller_->areas()) {
        if (parentArea_->findData(item.parentId) < 0)
            parentArea_->addItem(item.parentName.isEmpty() ? QStringLiteral("未分类") : item.parentName,
                                 item.parentId);
    }
    if (room.areaId > 0 && parentArea_->findData(room.parentAreaId) < 0) {
        bool roomKnown = false;
        for (const auto &item : controller_->areas())
            if (item.id == room.areaId) {
                roomKnown = true;
                break;
            }
        if (!roomKnown)
            parentArea_->addItem(room.parentAreaName.isEmpty() ? QStringLiteral("当前分类（名称暂不可用）")
                                                               : room.parentAreaName,
                                 room.parentAreaId);
    }
    int index = parentArea_->findData(parentId);
    if (index < 0 && parentArea_->count() > 0)
        index = 0;
    parentArea_->setCurrentIndex(index);
    updateChildAreas(selection);
    if (keepSearch && parentArea_->currentData().toInt() == parentId)
        area_->setEditText(search);
    areaHint_->setText(controller_->areasLoading() ? QStringLiteral("正在加载分区名称…")
                       : controller_->areasError().isEmpty()
                           ? QString()
                           : QStringLiteral("分区列表暂不可用，可点“刷新状态”重试。"));
    areaHint_->setToolTip(controller_->areasError());
    areaHint_->setVisible(!areaHint_->text().isEmpty());
    updateView();
}

void RoomDock::updateChildAreas(int selection) {
    const QSignalBlocker blocker(area_);
    area_->clear();
    if (parentArea_->currentIndex() < 0)
        return;
    const int parentId = parentArea_->currentData().toInt();
    const auto add = [this, parentId](const QString &name, int id) {
        area_->addItem(name, id);
        area_->setItemData(area_->count() - 1, parentId, Qt::UserRole + 1);
    };
    bool roomKnown = false;
    const auto &room = controller_->room();
    for (const auto &item : controller_->areas()) {
        if (item.id == room.areaId)
            roomKnown = true;
        if (item.parentId == parentId)
            add(item.name, item.id);
    }
    if (!roomKnown && room.areaId > 0 && room.parentAreaId == parentId)
        add(room.areaName.isEmpty() ? QStringLiteral("当前分区（名称暂不可用）") : room.areaName,
            room.areaId);
    const int index = area_->findData(selection);
    area_->setCurrentIndex(index >= 0 ? index : area_->count() > 0 ? 0 : -1);
}

void RoomDock::updateCover() {
    currentCover_ = {};
    if (!controller_->coverBytes().isEmpty()) {
        QBuffer buffer;
        buffer.setData(controller_->coverBytes());
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && static_cast<qint64>(size.width()) * size.height() <= 80000000) {
            reader.setScaledSize(size.scaled(720, 540, Qt::KeepAspectRatio));
            currentCover_ = QPixmap::fromImage(reader.read());
        }
    }
    showCover();
}

void RoomDock::showCover() {
    coverPreview_->setToolTip(QStringLiteral("选择图片，裁切后保存并上传封面"));
    coverPreview_->setPreview(currentCover_);
    if (!currentCover_.isNull())
        return;
    if (controller_->coverLoading())
        coverPreview_->setMessage(QStringLiteral("正在加载当前封面…"));
    else if (!controller_->coverError().isEmpty()) {
        coverPreview_->setMessage(QStringLiteral("封面加载失败，请刷新重试"));
        coverPreview_->setToolTip(controller_->coverError());
    } else if (!controller_->coverBytes().isEmpty())
        coverPreview_->setMessage(QStringLiteral("无法显示此封面格式"));
    else
        coverPreview_->setMessage(controller_->room().roomId ? QStringLiteral("暂无封面")
                                                             : QStringLiteral("登录后显示当前封面"));
}

void RoomDock::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    showCover();
}
} // namespace livehime
