// Production widgets, synthetic controller/login/output doubles, no platform I/O.
#include "room-dock.hpp"
#include "cover-crop-dialog.hpp"
#include "login-dialog.hpp"
#include "native-controls-fixture.hpp"

#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <iostream>
#include <utility>

using namespace livehime;
namespace {
int failures = 0;
int outputQueries = 0;
struct UiTrace {
    QStringList calls;
    QString sentTitle;
    QString startTitle;
    bool metadataBusy = false;
    bool startSawPendingTitle = false;
    int starts = 0;
    int startArea = 0;
    int covers = 0;
    bool deferredCoverReadable = false;
    std::optional<LiveController::State> nextState;
    bool cancelablePreparation = false;
    bool outputBusy = false;
    bool ownsOutput = false;
    int stops = 0;
    int retries = 0;
};
QHash<LiveController *, UiTrace> traces;

void check(bool value, const char *description)
{
    if (!value) { std::cerr << "FAIL: " << description << '\n'; ++failures; }
}
void drain()
{
    for (int i = 0; i != 12; ++i) QCoreApplication::processEvents();
}
template<class T> T *child(QWidget &parent, const char *name)
{
    auto *found = parent.findChild<T *>(QString::fromLatin1(name));
    check(found != nullptr, name);
    return found;
}
void show(QWidget &widget)
{
    widget.show();
    widget.activateWindow();
    drain();
}
void mouse(QWidget *widget, QEvent::Type type, const QPointF &position,
           Qt::MouseButton button, Qt::MouseButtons held)
{
    QMouseEvent event(type, position, widget->mapToGlobal(position.toPoint()), button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &event);
}
void click(QPushButton *button)
{
    button->setFocus(Qt::MouseFocusReason);
    drain();
    const QPointF center = button->rect().center();
    mouse(button, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(button, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    drain();
}
void edit(QLineEdit *line, const QString &value)
{
    line->setFocus(Qt::OtherFocusReason);
    drain();
    const QWidget *focus = QApplication::focusWidget();
    check(focus == line || (focus == line->parentWidget() && qobject_cast<const QComboBox *>(focus)),
          "text input or its editable combo receives focus");
    line->selectAll();
    QKeyEvent event(QEvent::KeyPress, Qt::Key_unknown, Qt::NoModifier, value);
    QCoreApplication::sendEvent(line, &event);
    drain();
}
void drag(QWidget *canvas, const QPointF &delta)
{
    const QPointF start = canvas->rect().center();
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, start + delta, Qt::LeftButton, Qt::NoButton);
    drain();
}
QImage quadrants(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(210, 170, 30));
    QPainter painter(&image);
    painter.fillRect(0, 0, width / 2, height / 2, QColor(210, 40, 40));
    painter.fillRect(width / 2, 0, width - width / 2, height / 2, QColor(40, 170, 70));
    painter.fillRect(0, height / 2, width / 2, height - height / 2, QColor(40, 70, 210));
    return image;
}
bool noEmptyEdges(const QImage &image)
{
    if (image.size() != QSize(720, 540)) return false;
    const auto filled = [&](int x, int y) {
        const QColor color = image.pixelColor(x, y);
        return color.alpha() == 255 && color != QColor(Qt::white);
    };
    for (int x = 0; x < image.width(); x += 17)
        if (!filled(x, 0) || !filled(x, image.height() - 1)) return false;
    for (int y = 0; y < image.height(); y += 17)
        if (!filled(0, y) || !filled(image.width() - 1, y)) return false;
    return true;
}
QImage acceptCrop(CoverCropDialog &dialog)
{
    auto *save = child<QPushButton>(dialog, "coverCropSave");
    if (!save) return {};
    check(save->isEnabled(), "valid crop can be saved");
    save->click();
    check(dialog.result() == QDialog::Accepted, "save accepts crop dialog");
    return dialog.croppedImage();
}
void cropGeometry()
{
    const QImage colors = quadrants(1200, 900);
    {
        CoverCropDialog dialog(colors);
        show(dialog);
        check(dialog.croppedImage().isNull(), "crop result is empty before confirmation");
        const QImage output = acceptCrop(dialog);
        check(output.size() == QSize(720, 540), "crop output is 720 by 540");
        check(output.pixelColor(180, 135) == QColor(210, 40, 40) &&
              output.pixelColor(540, 135) == QColor(40, 170, 70) &&
              output.pixelColor(180, 405) == QColor(40, 70, 210), "default 4:3 crop preserves composition");
    }
    for (const int zoom : {0, 1000}) {
        for (const QPointF delta : {QPointF(-10000, -10000), QPointF(10000, -10000),
                                    QPointF(-10000, 10000), QPointF(10000, 10000)}) {
            CoverCropDialog dialog(colors);
            show(dialog);
            auto *slider = child<QSlider>(dialog, "coverZoomSlider");
            auto *canvas = child<QWidget>(dialog, "coverCropCanvas");
            if (!slider || !canvas) continue;
            slider->setValue(zoom);
            drag(canvas, delta);
            check(noEmptyEdges(acceptCrop(dialog)), "min/max zoom and four-corner dragging leave no blank edges");
        }
    }
    {
        CoverCropDialog dialog(colors);
        show(dialog);
        auto *canvas = child<QWidget>(dialog, "coverCropCanvas");
        auto *slider = child<QSlider>(dialog, "coverZoomSlider");
        auto *reset = child<QPushButton>(dialog, "coverCropReset");
        if (canvas && slider && reset) {
            const QPointF position = canvas->rect().center();
            QWheelEvent wheel(position, canvas->mapToGlobal(position.toPoint()), {}, QPoint(0, 240),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(canvas, &wheel);
            check(slider->value() > slider->minimum(), "wheel changes zoom slider");
            drag(canvas, QPointF(200, 100));
            reset->click();
            check(slider->value() == slider->minimum(), "reset restores minimum zoom");
            check(noEmptyEdges(acceptCrop(dialog)), "wheel and reset preserve a fully filled crop");
        }
    }
    for (const QSize size : {QSize(8, 6), QSize(40, 3000), QSize(3000, 40)}) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(QColor(180, 40, 170));
        CoverCropDialog dialog(image);
        show(dialog);
        check(noEmptyEdges(acceptCrop(dialog)), "tiny, portrait, and panorama inputs fill the output");
    }
    {
        QImage bands(1200, 900, QImage::Format_RGB32);
        QPainter painter(&bands);
        const QColor palette[] = {QColor(210, 40, 40), QColor(40, 170, 70), QColor(40, 70, 210), QColor(210, 170, 30)};
        for (int i = 0; i < 4; ++i) painter.fillRect(i * 300, 0, 300, 900, palette[i]);
        painter.end();
        for (const int direction : {-1, 1}) {
            CoverCropDialog dialog(bands);
            show(dialog);
            auto *slider = child<QSlider>(dialog, "coverZoomSlider");
            auto *canvas = child<QWidget>(dialog, "coverCropCanvas");
            if (!slider || !canvas) continue;
            slider->setValue(slider->maximum());
            drag(canvas, QPointF(direction * 10000, 0));
            const QImage output = acceptCrop(dialog);
            check(output.pixelColor(360, 270) == palette[direction > 0 ? 0 : 3], "drag direction selects the expected source edge");
        }
    }
    {
        QImage transparent(800, 600, QImage::Format_ARGB32);
        transparent.fill(Qt::transparent);
        CoverCropDialog dialog(transparent);
        show(dialog);
        const QImage output = acceptCrop(dialog);
        check(output.pixelColor(360, 270) == QColor(Qt::white), "transparent input flattens onto opaque white");
    }
    {
        QImage wideGamut = colors;
        wideGamut.setColorSpace(QColorSpace::DisplayP3);
        CoverCropDialog dialog(wideGamut);
        show(dialog);
        check(acceptCrop(dialog).colorSpace() == QColorSpace(QColorSpace::SRgb), "export converts color profile to sRGB");
    }
    {
        CoverCropDialog dialog(colors);
        show(dialog);
        if (auto *cancel = child<QPushButton>(dialog, "coverCropCancel")) cancel->click();
        check(dialog.result() == QDialog::Rejected && dialog.croppedImage().isNull(), "cancel returns no crop");
    }
    for (const QImage invalid : {QImage(), QImage(32769, 1, QImage::Format_RGB32)}) {
        CoverCropDialog dialog(invalid);
        show(dialog);
        auto *save = child<QPushButton>(dialog, "coverCropSave");
        check(save && !save->isEnabled() && dialog.croppedImage().isNull(), "null and oversize inputs cannot be saved");
        dialog.reject();
    }
}
void titleEvents()
{
    QTemporaryDir directory;
    test_support::NativeControlsWindow window;
    const int queriesBeforeConstruction = outputQueries;
    RoomDock dock(directory.path(), &window);
    check(outputQueries == queriesBeforeConstruction, "dock construction never queries uninitialized OBS output");
    window.layout()->addWidget(&dock);
    QString controlsError;
    check(dock.attachNativeControls(&window, &controlsError), "dock attaches real native controls adapter");
    show(window);
    auto *controller = dock.findChild<LiveController *>();
    auto *title = child<QLineEdit>(dock, "title-edit");
    auto *start = window.streamButton;
    auto *cover = child<QPushButton>(dock, "cover-preview");
    if (!controller || !title || !start || !cover) return;
    check(traces[controller].calls.isEmpty(), "programmatic initial room title does not trigger a save");
    edit(title, QStringLiteral("Title A"));
    cover->setFocus(Qt::MouseFocusReason);
    drain();
    check(controller->metadataBusy() && title->isEnabled() && cover->isEnabled() && start->isEnabled(),
          "metadata pending keeps title, cover, and start usable");
    edit(title, QStringLiteral("Title B"));
    controller->refresh(); // Fake returns the older in-flight title A, preserving draft B.
    drain();
    check(title->text() == QStringLiteral("Title B"), "older room response does not replace the newer visible title");
    click(start);
    const auto trace = traces.value(controller);
    check(trace.starts == 1 && trace.startTitle == QStringLiteral("Title B"), "focus loss then one mouse click starts with the latest title");
    check(trace.startArea == 1, "start uses the current room's selected area ID");
    check(trace.startSawPendingTitle, "start reaches controller even when editingFinished flushed metadata first");
    check(window.originalClicks == 0 && window.originalStarts == 0,
          "native mouse click is consumed before OBS can start an unprepared service");
    click(start);
    check(traces.value(controller).starts == 1, "disabled platform request cannot be started twice");
    check(trace.calls.indexOf(QStringLiteral("flush")) >= 0 &&
          trace.calls.indexOf(QStringLiteral("flush")) < trace.calls.indexOf(QStringLiteral("start")),
          "focus-triggered flush precedes start intent");
    for (auto *button : dock.findChildren<QPushButton *>())
        check(button->text() != QStringLiteral("保存标题和封面") && button->text() != QStringLiteral("取消选择") &&
              button->text() != QStringLiteral("选择封面…") && button->text() != QStringLiteral("一键开播") &&
              button->text() != QStringLiteral("下播"), "obsolete metadata and streaming buttons are removed");
    check(!dock.findChild<QPushButton *>(QStringLiteral("start-button")), "dock contains no separate start button");
}
void linkedAreas()
{
    QTemporaryDir directory;
    test_support::NativeControlsWindow window;
    RoomDock dock(directory.path(), &window);
    window.layout()->addWidget(&dock);
    QString controlsError;
    check(dock.attachNativeControls(&window, &controlsError), "linked area fixture attaches native controls");
    show(window);
    auto *controller = dock.findChild<LiveController *>();
    auto *parent = child<QComboBox>(dock, "parent-area-combo");
    auto *area = child<QComboBox>(dock, "area-combo");
    auto *start = window.streamButton;
    if (!controller || !parent || !area || !start) return;
    check(!parent->isEditable() && area->isEditable(), "parent is a dropdown and child keeps name search");
    check(parent->count() == 2 && parent->currentData().toInt() == 10 && area->currentData().toInt() == 1,
          "initial parent and child match the room even when its child is not first");
    check(area->count() == 2 && area->findData(3) < 0 && area->currentText() == QStringLiteral("Current area"),
          "child dropdown shows only names in the selected parent");
    const auto activate = [](QComboBox *combo, int index) {
        combo->setCurrentIndex(index);
        QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
        drain();
    };
    activate(parent, parent->findData(20));
    check(area->count() == 2 && area->currentData().toInt() == 3 && area->findData(1) < 0,
          "changing parent selects its first child and excludes the old parent");
    edit(area->lineEdit(), QStringLiteral("Second"));
    check(!start->isEnabled(), "unfinished child search cannot submit an unrelated area ID");
    activate(area, area->findData(4));
    controller->refresh();
    emit controller->areasChanged();
    drain();
    check(parent->currentData().toInt() == 20 && area->currentData().toInt() == 4 && start->isEnabled(),
          "room and area refresh preserve the user's linked selection");
    click(start);
    check(traces.value(controller).startArea == 4, "start passes the selected child ID rather than the parent ID");
}
void nativeControllerRouting()
{
    QTemporaryDir directory;
    test_support::NativeControlsWindow window;
    RoomDock dock(directory.path(), &window);
    window.layout()->addWidget(&dock);
    QString error;
    check(dock.attachNativeControls(&window, &error), "routing fixture attaches native controls");
    show(window);
    auto *controller = dock.findChild<LiveController *>();
    auto *retry = child<QPushButton>(dock, "retry-connection-button");
    if (!controller || !retry) return;
    const auto state = [&](LiveController::State value, bool cancelable = false) {
        traces[controller].nextState = value;
        traces[controller].cancelablePreparation = cancelable;
        controller->refresh();
        drain();
    };
    controller->logout();
    check(!window.streamButton->isEnabled(), "signed-out native start cannot use stale OBS service");
    controller->acceptCookies({});
    check(window.streamButton->isEnabled(), "login and valid room enable native start");
    state(LiveController::State::Preparing, true);
    check(window.streamButton->text() == QStringLiteral("取消开播") && window.streamButton->isEnabled(),
          "queued preparation exposes cancellation through native button");
    window.streamButton->click();
    check(traces[controller].stops == 1 && controller->state() == LiveController::State::Ready,
          "native cancel reaches the existing controller stop path");
    state(LiveController::State::Preparing);
    window.streamButton->click();
    check(!window.streamButton->isEnabled() && traces[controller].stops == 1 && traces[controller].starts == 0,
          "in-flight platform start cannot be cancelled or duplicated by native button");
    state(LiveController::State::Reconnecting);
    check(window.streamButton->isEnabled() && window.streamButton->text() == QStringLiteral("停止直播") &&
          traces[controller].starts == 0 && traces[controller].stops == 1,
          "reconnect only updates native control state without a platform operation");
    state(LiveController::State::Live);
    window.streamButton->click();
    drain();
    check(traces[controller].stops == 2 && !window.streamButton->isEnabled(),
          "native stop reaches controller once and waits for remote stop completion");
    window.streamButton->click();
    check(traces[controller].stops == 2, "repeated click during stop cannot request another stop");
    state(LiveController::State::Uncertain);
    check(window.streamButton->isEnabled(), "uncertain remote result keeps native stop retry available");
    window.streamButton->click();
    check(traces[controller].stops == 3, "uncertain native action retries stopping rather than starting");
    state(LiveController::State::Recoverable);
    check(retry->isVisible() && retry->isEnabled(), "recoverable stream retains the separate connection retry");
    retry->click();
    check(traces[controller].retries == 1 && traces[controller].starts == 0,
          "connection retry remains distinct from a new platform start");
    check(window.originalClicks == 0 && window.originalStarts == 0 && window.originalStops == 0,
          "managed native buttons never run the unprepared original click route");
}
void obsStyledCoverGeometry()
{
    const QString oldStyle = qApp->styleSheet();
    // OBS Yami applies these box metrics to every QPushButton. A private
    // thumbnail must keep its height when the application stylesheet polishes it.
    qApp->setStyleSheet(QStringLiteral(
        "QPushButton { height: 20px; max-height: 20px; margin-top: 3px; "
        "margin-bottom: 3px; padding: 4px 16px; border: 1px solid #555; }"));
    {
        QTemporaryDir directory;
        RoomDock dock(directory.path());
        dock.resize(420, 700);
        show(dock);
        auto *cover = child<QPushButton>(dock, "cover-preview");
        if (cover) {
            const auto checkLayout = [&] {
                check(cover->height() == 190 && cover->minimumHeight() == 190 && cover->maximumHeight() == 190,
                      "OBS button stylesheet cannot shrink or displace cover preview");
                check(dock.contentsRect().contains(cover->geometry()), "cover preview stays within dock bounds");
                for (auto *widget : dock.findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
                    if (widget != cover && widget->isVisible())
                        check(!cover->geometry().intersects(widget->geometry()),
                              "styled cover preview does not overlap neighboring controls");
            };
            checkLayout();
            cover->setFocus(Qt::TabFocusReason);
            dock.resize(600, 850);
            drain();
            checkLayout();
        }
    }
    qApp->setStyleSheet(oldStyle);
}
void coverFlow(const QString &fixture, bool chooseFile, bool acceptCropDialog)
{
    QTemporaryDir directory;
    RoomDock dock(directory.path());
    show(dock);
    auto *controller = dock.findChild<LiveController *>();
    auto *cover = child<QPushButton>(dock, "cover-preview");
    if (!controller || !cover) return;
    bool pickerSeen = false, cropSeen = false, timedOut = false;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dock, [&] {
        if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
            if (pickerSeen) return;
            pickerSeen = true;
            if (!chooseFile) picker->reject();
            else {
                // The QFileSystemModel populates asynchronously on the headless
                // platform. Enter the absolute filename as a user would, without
                // depending on a directory selection that may not exist yet.
                auto *filename = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
                check(filename != nullptr, "non-native file picker exposes filename input");
                if (filename) filename->setText(QDir::toNativeSeparators(fixture));
                else picker->selectFile(fixture);
                QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
            }
        } else if (auto *crop = qobject_cast<CoverCropDialog *>(QApplication::activeModalWidget())) {
            if (cropSeen) return;
            cropSeen = true;
            if (auto *button = child<QPushButton>(*crop, acceptCropDialog ? "coverCropSave" : "coverCropCancel")) button->click();
        }
    });
    QTimer::singleShot(3000, &poll, [&] {
        timedOut = true;
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    poll.start(1);
    cover->click();
    poll.stop();
    drain();
    check(!timedOut && pickerSeen, "cover button opens a cancellable image picker");
    check(cropSeen == chooseFile, "only a selected image opens the crop dialog");
    const auto trace = traces.value(controller);
    check(trace.covers == (chooseFile && acceptCropDialog ? 1 : 0), "only crop confirmation submits one fake cover operation");
    if (chooseFile && acceptCropDialog)
        check(trace.deferredCoverReadable, "cropped temporary file survives asynchronous upload handoff");
}
} // namespace

namespace livehime {
SessionStore::SessionStore(QString directory) : directory_(std::move(directory)) {}
QString SessionStore::browserDataDirectory() const { return QDir(directory_).filePath(QStringLiteral("unused-fake-browser")); }

ObsBridge::ObsBridge(QObject *parent) : QObject(parent) {}
ObsBridge::~ObsBridge() = default;
bool ObsBridge::busy() const { ++outputQueries; return traces.value(qobject_cast<LiveController *>(parent())).outputBusy; }
bool ObsBridge::ownsService() const { return traces.value(qobject_cast<LiveController *>(parent())).ownsOutput; }

LiveController::LiveController(QString storageDirectory, QObject *parent)
    : QObject(parent), store_(std::move(storageDirectory)), client_(nullptr), bridge_(new ObsBridge(this))
{ traces.insert(this, {}); }
LiveController::~LiveController() { traces.remove(this); }
void LiveController::initialize()
{
    authenticated_ = true; state_ = State::Ready;
    user_ = {123, QStringLiteral("Synthetic account")};
    room_.roomId = 456; room_.areaId = 1; room_.title = QStringLiteral("Original"); room_.liveStatus = 0;
    room_.parentAreaId = 10; room_.areaName = QStringLiteral("Current area"); room_.parentAreaName = QStringLiteral("Group A");
    titleDraft_ = room_.title; confirmedTitle_ = room_.title;
    areas_ = {{2, 10, QStringLiteral("First area"), QStringLiteral("Group A")},
              {1, 10, QStringLiteral("Current area"), QStringLiteral("Group A")},
              {3, 20, QStringLiteral("First child"), QStringLiteral("Group B")},
              {4, 20, QStringLiteral("Second child"), QStringLiteral("Group B")}};
    emit changed(); emit titleSyncChanged(); emit roomChanged(); emit areasChanged();
}
void LiveController::acceptCookies(const QList<QNetworkCookie> &) { initialize(); }
void LiveController::logout() { authenticated_ = false; state_ = State::SignedOut; emit changed(); }
void LiveController::refresh()
{
    auto &trace = traces[this];
    if (trace.nextState) {
        state_ = *trace.nextState;
        trace.nextState.reset();
        const bool remote = state_ == State::Connecting || state_ == State::Live || state_ == State::Reconnecting ||
            state_ == State::Recoverable || state_ == State::Uncertain || state_ == State::Stopping;
        remoteRoomId_ = remote ? room_.roomId : 0;
        session_ = remote ? std::optional<LiveSession>(LiveSession{room_.roomId, {}, {}}) : std::nullopt;
        working_ = state_ == State::Preparing || state_ == State::Stopping;
        pendingStartArea_ = state_ == State::Preparing && trace.cancelablePreparation ?
            std::optional<int>(room_.areaId) : std::nullopt;
        trace.outputBusy = state_ == State::Connecting || state_ == State::Live || state_ == State::Reconnecting || state_ == State::Stopping;
        trace.ownsOutput = remote;
        emit changed();
    }
    if (!trace.sentTitle.isEmpty()) room_.title = trace.sentTitle;
    emit roomChanged(); emit titleSyncChanged();
}
void LiveController::setTitleDraft(const QString &title)
{ titleDraft_ = title; emit titleSyncChanged(); }
void LiveController::flushTitle()
{
    if (titleDraft_.trimmed().isEmpty() || titleDraft_ == confirmedTitle_) return;
    auto &trace = traces[this];
    trace.calls.append(QStringLiteral("flush"));
    if (!trace.metadataBusy) trace.sentTitle = titleDraft_;
    trace.metadataBusy = true;
    emit titleSyncChanged(); emit changed();
}
QString LiveController::titleSyncStatus() const
{ return traces.value(const_cast<LiveController *>(this)).metadataBusy ? QStringLiteral("正在保存标题…") : QStringLiteral("标题已同步"); }
bool LiveController::metadataBusy() const { return traces.value(const_cast<LiveController *>(this)).metadataBusy; }
void LiveController::saveCover(const QString &path)
{
    ++traces[this].covers;
    QTimer::singleShot(0, this, [this, path] { traces[this].deferredCoverReadable = QImage(path).size() == QSize(720, 540); });
}
void LiveController::saveMetadata(const QString &title, const QString &coverFile)
{ setTitleDraft(title); flushTitle(); if (!coverFile.isEmpty()) saveCover(coverFile); }
void LiveController::start(const QString &title, const QString &, int areaId)
{
    auto &trace = traces[this];
    ++trace.starts; trace.startTitle = title; trace.startSawPendingTitle = trace.metadataBusy;
    trace.calls.append(QStringLiteral("start"));
    trace.startArea = areaId;
    state_ = State::Preparing; working_ = true; emit changed();
}
void LiveController::retryConnection()
{
    ++traces[this].retries;
    state_ = State::Connecting;
    traces[this].outputBusy = true;
    emit changed();
}
void LiveController::stop()
{
    ++traces[this].stops;
    if (canCancelStart()) { pendingStartArea_.reset(); working_ = false; state_ = State::Ready; }
    else { working_ = true; state_ = State::Stopping; }
    emit changed();
}

struct LoginDialog::Impl {};
LoginDialog::LoginDialog(QString, QString, QWidget *parent) : QDialog(parent), impl_(std::make_unique<Impl>()) {}
LoginDialog::~LoginDialog() = default;
void LoginDialog::showEvent(QShowEvent *event) { QDialog::showEvent(event); }
void LoginDialog::resizeEvent(QResizeEvent *event) { QDialog::resizeEvent(event); }
void LoginDialog::done(int result) { QDialog::done(result); }
bool LoginDialog::eventFilter(QObject *object, QEvent *event) { return QDialog::eventFilter(object, event); }
} // namespace livehime

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    {
        QTemporaryDir recoveryDirectory;
        RoomDock recoveryDock(recoveryDirectory.path());
        bool requested = false;
        QObject::connect(&recoveryDock, &RoomDock::unlockRealtimeInteraction, &recoveryDock, [&] { requested = true; });
        auto *recovery = recoveryDock.findChild<QPushButton *>(QStringLiteral("realtime-unlock-interaction"));
        check(recovery && recovery->isEnabled(), "main panel recovery entry remains available while signed out");
        if (recovery) recovery->click();
        check(requested, "main panel recovery button reaches the external unlock route");
        recoveryDock.setRealtimeAvailable(false);
        check(recovery && !recovery->isEnabled(), "failed native dock registration disables unavailable recovery entry");
    }
    const QString fixture = app.arguments().size() > 1 ? app.arguments().at(1) : QDir::current().filePath(QStringLiteral("crop-fixture.png"));
    QDir().mkpath(QFileInfo(fixture).absolutePath());
    QImage image = quadrants(1600, 1000);
    {
        QPainter painter(&image);
        painter.setPen(QPen(QColor(20, 25, 30), 3));
        for (int x = 0; x < image.width(); x += 100) painter.drawLine(x, 0, x, image.height());
        for (int y = 0; y < image.height(); y += 100) painter.drawLine(0, y, image.width(), y);
    }
    check(image.save(fixture, "PNG"), "write synthetic manual-crop fixture");
    cropGeometry();
    obsStyledCoverGeometry();
    titleEvents();
    linkedAreas();
    nativeControllerRouting();
    coverFlow(fixture, false, false);
    coverFlow(fixture, true, false);
    coverFlow(fixture, true, true);
    if (failures) { std::cerr << failures << " offline UI checks failed\n"; return 1; }
    std::cout << "Offline UI and crop checks passed; fixture=" << fixture.toStdString() << '\n';
    return 0;
}
