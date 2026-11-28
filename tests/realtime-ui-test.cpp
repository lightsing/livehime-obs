// Real presentation widgets with a main-thread, no-network realtime service double.
#include "dock-focus.hpp"
#include "display-asset-cache.hpp"
#include "audience-rank.hpp"
#include "floating-windows.hpp"
#include "moderation-alerts.hpp"
#include "room-realtime.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabBar>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QStyledItemDelegate>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextLayout>
#include <QTextBrowser>
#include <QtMath>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#ifdef Q_OS_WIN
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif
#include <algorithm>
#include <iostream>

namespace livehime {
static QStringList fixtureRankFaces;
static QList<RoomEntry> fixtureEntries = {{QStringLiteral("早到的观众"), 1700000000, {}, 1},
                                          {QStringLiteral("新到的观众"), 1700000001, {}, 2}};
SessionStore::SessionStore(QString p) : directory_(std::move(p)) {}
LiveController::LiveController(QString p, QObject *o)
    : QObject(o), store_(std::move(p)), client_(nullptr), bridge_(nullptr) {}
LiveController::~LiveController() = default;
RoomRealtime::RoomRealtime(LiveController *c, QObject *p) : QObject(p), controller_(c), goim_(nullptr) {
    entryHistory_ = fixtureEntries;
    connect(this, &RoomRealtime::entryHistoryChanged, this, [this] { entryHistory_ = fixtureEntries; });
    if (c) {
        uid_ = 123;
        roomId_ = 456;
    }
    connect(this, &RoomRealtime::danmakuSendFinished, this, [this] { sendingDanmaku_ = false; });
    connection_.detail = QStringLiteral("离线合成数据 · 布局验证");
    audienceStatus_ = guardStatus_ = QStringLiteral("离线合成数据");
    const QStringList viewers{QStringLiteral("听海的人"), QStringLiteral("青柠_Tsing"), QStringLiteral("小岛电台"),
                              QStringLiteral("空明AvA"), QStringLiteral("暮色星宇"), QStringLiteral("白忆寒_DreamWalker"),
                              QStringLiteral("晚星"), QStringLiteral("银河车站")};
    QJsonArray ranks;
    const int scores[]{1380, 30, 10, 6, 1, 1, 0, 0};
    for (int i = 0; i < viewers.size(); ++i) {
        const QJsonObject medal{{"name", "Healli"}, {"level", 30 - i},
                                {"v2_medal_color_start", i < 3 ? "#327d95" : "#78576e"}};
        QJsonObject uinfo{{"uid", i + 101}, {"base", QJsonObject{{"name", viewers[i]}, {"face", fixtureRankFaces.value(i)}}},
                          {"wealth", QJsonObject{{"level", 42 - i}}}, {"medal", medal}};
        if (i == viewers.size() - 1)
            uinfo.insert("is_mystery", 1);
        ranks.append(QJsonObject{{"rank", i + 1}, {"score", scores[i]}, {"uinfo", uinfo}});
    }
    audience_ = QJsonObject{{"count", viewers.size()}, {"item", ranks},
                            {"config", QJsonObject{{"value_text", QStringLiteral("贡献值")}}}};
    guard_ = QJsonObject{
        {"rows",
         QJsonArray{QJsonObject{{"rank", 1},
                                {"name", QStringLiteral("离线合成舰长")},
                                {"guard_level", 3},
                                {"accompany", 99}},
                    QJsonObject{{"rank", 2}, {"name", "secret"}, {"is_mystery", 1}, {"accompany", 50}}}}};
}
RoomRealtime::~RoomRealtime() = default;
static QString lastType, lastSort;
static int lastGuard = 0;
static QStringList postedMessages;
struct FixtureModeration { UserModeration action; qint64 uid; quint64 epoch; QString message; int durationHours; };
static QList<FixtureModeration> fixtureModeration;
void RoomRealtime::moderateUser(UserModeration action, qint64 uid, quint64 epoch, const QString &message, int durationHours) {
    fixtureModeration.append({action, uid, epoch, message, durationHours});
}
void RoomRealtime::sendDanmaku(const QString &text) {
    postedMessages.append(text);
    sendingDanmaku_ = true;
}
void RoomRealtime::setVisible(int, bool) {}
void RoomRealtime::selectAudience(const QString &t, const QString &s) {
    lastType = t;
    lastSort = s;
}
void RoomRealtime::selectGuard(int t) { lastGuard = t; }
void RoomRealtime::refreshAudience() {}
void RoomRealtime::refreshGuard() {}
void RoomRealtime::loadMoreGuard() {}
void RoomRealtime::stop() {}
} // namespace livehime
using namespace livehime;
static int failures = 0;
static void settle(int milliseconds = 80) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
static void check(bool ok, const char *m) {
    if (!ok) {
        std::cerr << m << '\n';
        ++failures;
    }
}
static QWidget *window(int k) {
    for (auto w : QApplication::allWidgets())
        if (w->objectName() == QStringLiteral("livehimeFloating%1").arg(k))
            return w;
    return nullptr;
}
static std::array<QDockWidget *, FloatingWindows::DockCount> registerShells(QMainWindow &host) {
    std::array<QDockWidget *, FloatingWindows::DockCount> docks{};
    const QStringList titles={QStringLiteral("直播互动"),QStringLiteral("房间观众"),QStringLiteral("大航海")};
    for(int k=0;k<FloatingWindows::DockCount;++k){
        docks[k]=new QDockWidget(titles[k],&host);
        docks[k]->setObjectName(QString::fromLatin1(FloatingWindows::dockId(k)));
        docks[k]->setWidget(new QWidget);
        host.addDockWidget(Qt::RightDockWidgetArea,docks[k]);
        docks[k]->hide();
    }
    return docks;
}

static void checkIndependentFloat(QDockWidget *dock, QMainWindow &host) {
    auto *window = dock->windowHandle();
    check(dock->isFloating() && window && !window->transientParent(),
          "floating panel has an independent Qt window without a transient owner");
    check(!dock->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus),
          "independent floating panel still accepts keyboard focus");
    check(host.findChild<QDockWidget *>(dock->objectName()) == dock,
          "independent floating panel still belongs to the original host widget tree");
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows" && window) {
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        check(IsWindow(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr,
              "floating panel has no native owner that could raise OBS alongside it");
        check((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) == 0,
              "native floating panel remains activatable");
    }
#endif
}

static void checkFloatingEditor(QDockWidget *dock, QLineEdit *editor) {
    check(editor && dock->isAncestorOf(editor), "floating panel retains its editor");
    if (!editor) return;
    dock->activateWindow();
    settle();
    if (QGuiApplication::platformName() == "minimal") {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(dock);
        QT_WARNING_POP
    }
    editor->setFocus(Qt::TabFocusReason);
    check(QApplication::focusWidget() == editor, "floating panel editor can take keyboard focus");
    const auto initial = editor->text();
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
    if (dock->windowHandle())
        QApplication::sendEvent(dock->windowHandle(), &key);
    check(editor->text() == initial + "a", "key dispatch through the floating window reaches its focused editor");
}

static void checkDockFocus(RoomRealtime &service) {
    QTemporaryDir settings;
    QMainWindow host;
    host.setCentralWidget(new QWidget);
    host.resize(1000, 620);
    auto *room = new QDockWidget(QStringLiteral("B站直播"), &host);
    room->setObjectName("livehime-obs-room");
    auto *title = new QLineEdit;
    room->setWidget(title);
    host.addDockWidget(Qt::LeftDockWidgetArea, room);
    auto shells = registerShells(host);
    host.show();
    installDockFocusPolicy(room, &host);
    FloatingWindows windows(&service, settings.path(), &host, shells);
    const auto hostFlags = host.windowFlags();

    room->setFloating(true);
    {
        const QSignalBlocker block(room);
        room->hide();
        room->show();
    }
    settle();
    checkIndependentFloat(room, host);
    checkFloatingEditor(room, title);
    const auto layout = host.saveState(41);
    room->setFloating(false);
    check(room->window() == &host && host.dockWidgetArea(room) == Qt::LeftDockWidgetArea,
          "room panel redocks into its original host area");
    check(host.restoreState(layout, 41), "host restores the native floating room layout");
    settle();
    checkIndependentFloat(room, host);
    for (int kind = 0; kind < FloatingWindows::DockCount; ++kind) {
        auto *dock = shells[kind];
        windows.floatWindow(kind);
        settle();
        checkIndependentFloat(dock, host);
        auto *pin = dock->findChild<QCheckBox *>(QString("window-pin-%1").arg(kind));
        check(pin && pin->isEnabled(), "floating realtime panel exposes its pin control");
        if (pin) {
            for (bool pinned : {false, true}) {
                pin->setChecked(pinned);
                settle();
                check(dock->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == pinned,
                      "floating realtime panel applies its own pin setting");
                checkIndependentFloat(dock, host);
            }
        }
        if (kind == 0)
            checkFloatingEditor(dock, dock->findChild<QLineEdit *>("danmakuInput"));
        if (kind == 1) {
            auto *lock = dock->findChild<QPushButton *>("audience-lock");
            check(lock && lock->isEnabled(), "floating audience panel offers click-through");
            if (lock) {
                lock->click();
                check(lock->isChecked() && dock->windowFlags().testFlag(Qt::WindowTransparentForInput) &&
                      !windows.interactionLocked(), "audience click-through is independent of chat");
                windows.showWindow(1);
                check(!lock->isChecked() && !dock->windowFlags().testFlag(Qt::WindowTransparentForInput),
                      "reopening audience restores mouse input");
                lock->click();
            }
        }
        windows.dockWindow(kind);
        if (kind == 1)
            check(!dock->windowFlags().testFlag(Qt::WindowTransparentForInput), "redocking audience clears click-through");
        check(dock->window() == &host && host.windowFlags() == hostFlags,
              "redocking keeps the original main window and its flags");
    }
    host.showMinimized();
    settle();
    windows.floatWindow(0);
    settle();
    check(host.isMinimized() && shells[0]->isVisible() && !shells[0]->isMinimized(),
          "opening an independent panel keeps the OBS host minimized");
    checkIndependentFloat(shells[0], host);
#ifdef Q_OS_WIN
    if (QApplication::platformName() == "windows") {
        auto *floating = shells[0];
        auto freeDrag = [&](QEvent::Type type) {
            const QPoint p(-100, -100); // Probe the drag policy without clicking a control.
            QMouseEvent press(type, QPointF(p), QPointF(floating->mapToGlobal(p)), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(floating, &press);
            return press.modifiers().testFlag(Qt::ControlModifier);
        };
        for (auto type : {QEvent::MouseButtonPress, QEvent::NonClientAreaMouseButtonPress})
            check(freeDrag(type), "minimized OBS forces a free floating drag");
        host.showNormal();
        QWidget otherApp;
        otherApp.setGeometry(host.frameGeometry());
        otherApp.show();
        otherApp.raise();
        otherApp.activateWindow();
        settle();
        check(freeDrag(QEvent::NonClientAreaMouseButtonPress), "another window covering OBS prevents docking");
        floating->raise();
        floating->activateWindow();
        settle();
        check(freeDrag(QEvent::NonClientAreaMouseButtonPress), "activating a float above another app keeps docking disabled");
        otherApp.hide();
        host.raise();
        host.activateWindow();
        settle();
        if (GetForegroundWindow() == reinterpret_cast<HWND>(host.winId()))
            check(!freeDrag(QEvent::NonClientAreaMouseButtonPress), "foreground OBS retains ordinary drag docking");
    }
#endif
    host.showNormal();
    windows.shutdown();
}
static void checkUnlocked(FloatingWindows &windows,const char *reason) {
    auto dock=windows.dockWidget(0);
    auto button=windows.panelWidget(0)->findChild<QPushButton *>("interaction-lock");
    check(dock&&!dock->windowFlags().testFlag(Qt::WindowTransparentForInput),reason);
    check(!windows.interactionLocked(),"interaction reports unlocked");
    check(button&&!button->isChecked(),"lock button reflects unlocked state");
#ifdef Q_OS_WIN
    if(QApplication::platformName()=="windows"&&dock&&dock->isFloating())
        check((GetWindowLongPtrW(reinterpret_cast<HWND>(dock->winId()),GWL_EXSTYLE)&WS_EX_TRANSPARENT)==0,
              "native Windows passthrough style is cleared");
#endif
}

static QRect globalRect(QWidget *widget) {
    return QRect(widget->mapToGlobal(QPoint()), widget->size());
}

static QRegion visibleToolRegion(QWidget *tool) {
    if (!tool->isVisible()) return {};
    const auto mask = tool->mask();
    return (mask.isEmpty() ? QRegion(tool->rect()) : mask).translated(tool->mapToGlobal(QPoint()));
}

static void checkInputClipping(QDockWidget *dock, QScrollArea *scroll, QWidget *row, QWidget *controls,
                               QLineEdit *input, QPushButton *send) {
    auto *viewport = scroll->viewport();
    auto *bar = scroll->verticalScrollBar();
    auto correctlyClipped = [&] {
        const auto visibleRow = QRegion(globalRect(row).intersected(globalRect(viewport)));
        const auto controlFaces = QRegion(globalRect(input)).united(globalRect(send)).intersected(visibleRow);
        const auto exposed = visibleToolRegion(controls);
        return !controlFaces.isEmpty() && exposed == controlFaces;
    };
    dock->resize(420, 160);
    settle();
    scroll->ensureWidgetVisible(row, 0, 0);
    settle();
    const auto rowRect = globalRect(row);
    const bool fullyAligned = globalRect(viewport).contains(rowRect) && correctlyClipped();
    if (!fullyAligned) {
        QString values;
        {
            QDebug out(&values);
            out << "Initial locked input" << "row" << rowRect << "viewport" << globalRect(viewport)
                << "controls" << globalRect(controls) << "exposed" << visibleToolRegion(controls)
                << "controlFaces" << QRegion(globalRect(input)).united(globalRect(send))
                << "mask" << controls->mask() << "scroll" << bar->value() << "/" << bar->maximum();
        }
        std::cerr << values.toStdString() << '\n';
    }
    check(fullyAligned,
          "the locked input surface fits its row and exposes the editor and send button");
    dock->move(dock->pos() + QPoint(21, 13));
    settle();
    check(globalRect(row).topLeft() != rowRect.topLeft() &&
          correctlyClipped(),
          "moving the locked interaction dock moves its input surface with the row");
    const int half = row->height() / 2;
    const int contentY = bar->value() + row->mapTo(viewport, QPoint()).y();
    bar->setValue(contentY + half - viewport->height());
    settle();
    const auto cropped = globalRect(row).intersected(globalRect(viewport));
    check(cropped.height() > 0 && cropped.height() < row->height() && correctlyClipped(),
          "a partially scrolled input surface is clipped to the visible viewport intersection");
    bar->setValue(bar->minimum());
    settle();
    check(!globalRect(row).intersects(globalRect(viewport)) && !controls->isVisible(),
          "scrolling the entire input row out of view leaves no floating input surface");
    scroll->ensureWidgetVisible(row, 0, 0);
    settle();
    check(globalRect(viewport).contains(globalRect(row)) && correctlyClipped(),
          "scrolling the draft back into view restores the same input surface");
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows" && controls->windowHandle()) {
        const auto hwnd = reinterpret_cast<HWND>(controls->windowHandle()->winId());
        check(IsWindow(hwnd) && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)),
              "the native editor tool accepts clicks and activation independently of the transparent dock");
        for (QWidget *control : {static_cast<QWidget *>(input), static_cast<QWidget *>(send)}) {
            const auto pos = control->mapTo(controls, control->rect().center());
            const auto ratio = controls->devicePixelRatioF();
            POINT screenPoint{qRound(pos.x() * ratio), qRound(pos.y() * ratio)};
            const bool mapped = ClientToScreen(hwnd, &screenPoint);
            const auto hit = mapped ? WindowFromPoint(screenPoint) : nullptr;
            check(hit && GetAncestor(hit, GA_ROOT) == hwnd,
                  "native hit testing reaches the locked editor and send button");
        }
    }
#endif
}

static void checkLockedDanmaku(RoomRealtime &service) {
    QTemporaryDir settings;
    QMainWindow host;
    host.setCentralWidget(new QWidget);
    auto shells = registerShells(host);
    host.resize(900, 650);
    host.show();
    FloatingWindows windows(&service, settings.path(), &host, shells);
    auto *dock = shells[0];
    auto *lock = dock->findChild<QPushButton *>("interaction-lock");
    auto *scroll = dock->findChild<QScrollArea *>();
    auto *row = dock->findChild<QWidget *>("danmakuInputRow");
    auto *controls = dock->findChild<QWidget *>("danmakuControls");
    auto *input = dock->findChild<QLineEdit *>("danmakuInput");
    auto *send = dock->findChild<QPushButton *>("danmakuSend");
    check(lock && scroll && row && controls && input && send, "interaction exposes one real danmaku input row");
    if (!lock || !scroll || !row || !controls || !input || !send) return;
    auto checkSameControls = [&] {
        int editors = 0, buttons = 0;
        for (auto *widget : QApplication::allWidgets()) {
            if (widget->objectName() == "danmakuInput") ++editors;
            if (widget->objectName() == "danmakuSend") ++buttons;
        }
        check(editors == 1 && buttons == 1 && controls->parentWidget() == row &&
              controls->findChild<QLineEdit *>("danmakuInput") == input &&
              controls->findChild<QPushButton *>("danmakuSend") == send,
              "lock transitions keep the original editor and button with no duplicates");
    };
    windows.floatWindow(0);
    dock->move(32, 32);
    dock->resize(420, 650);
    settle();
    checkSameControls();
    check(!controls->isWindow() && input->isEnabled() && !send->isEnabled(), "empty draft starts in its normal inline editor");
    input->setText(QStringLiteral("主播"));
    lock->click();
    settle();
    checkSameControls();
    check(windows.interactionLocked() && controls->isWindow() && controls->windowType() == Qt::Tool &&
          !controls->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
          !controls->windowFlags().testFlag(Qt::WindowTransparentForInput) && input->text() == QStringLiteral("主播"),
          "locked interaction keeps a focusable input tool and its original draft");
    checkInputClipping(dock, scroll, row, controls, input, send);
    controls->activateWindow();
    settle();
    if (QApplication::platformName() == "minimal") {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(controls);
        QT_WARNING_POP
    }
    input->setFocus(Qt::TabFocusReason);
    check(QApplication::focusWidget() == input, "the locked panel's real editor receives keyboard focus");
    postedMessages.clear();
    QInputMethodEvent composition("ni", {});
    QApplication::sendEvent(input, &composition);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
    check(postedMessages.isEmpty(), "IME candidate confirmation does not send the locked draft");
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("你好"));
    QApplication::sendEvent(input, &commit);
    QKeyEvent held(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), true);
    QApplication::sendEvent(input, &held);
    const auto draft = QStringLiteral("主播你好");
    check(input->text() == draft && postedMessages.isEmpty(), "committed IME text survives without auto-repeat posting it");
    QApplication::sendEvent(input, &enter);
    QApplication::sendEvent(input, &enter);
    send->click();
    check(postedMessages == QStringList{draft} && input->isReadOnly() && !send->isEnabled(),
          "Return posts once and duplicate submits are blocked while awaiting its acknowledgement");
    windows.unlockInteraction();
    settle();
    service.danmakuSendFinished(false, QStringLiteral("已禁言"));
    check(!controls->isWindow() && input->text() == draft && !input->isReadOnly() && send->isEnabled(),
          "an error received after unlocking keeps the same editable draft for retry");
    lock->click();
    settle();
    checkSameControls();
    send->click();
    send->click();
    check(postedMessages == QStringList{draft, draft}, "the locked send button posts one retry using the original editor");
    service.danmakuSendFinished(true, QStringLiteral("已发送"));
    check(input->text().isEmpty() && !send->isEnabled(), "only a confirmed send clears the locked draft");

    input->setText(QStringLiteral("保留到重开"));
    {
        const QSignalBlocker block(dock);
        dock->hide();
    }
    settle();
    check(!windows.interactionLocked() && !controls->isVisible() && !controls->isWindow(),
          "signal-blocked OBS menu hide removes the independent input tool and unlocks interaction");
    {
        const QSignalBlocker block(dock);
        dock->show();
    }
    settle();
    checkSameControls();
    check(!windows.interactionLocked() && !controls->isWindow() && input->text() == QStringLiteral("保留到重开"),
          "the OBS menu reopens an unlocked editor with the draft intact");
    lock->click();
    dock->close();
    settle();
    check(!controls->isVisible() && !controls->isWindow(), "closing a locked dock leaves no independent input tool");
    windows.showWindow(0);
    check(input->text() == QStringLiteral("保留到重开"), "explicit reopen also preserves the unsent draft");
    send->click();
    check(postedMessages == QStringList{draft, draft, QStringLiteral("保留到重开")},
          "the reopened inline send button retains exactly one submission connection");
    service.danmakuSendFinished(true, QStringLiteral("已发送"));
    lock->click();
    windows.dockWindow(0);
    settle();
    check(!controls->isWindow() && !windows.interactionLocked(), "redocking returns the input controls to the inline row");
    windows.floatWindow(0);
    lock->click();
    windows.shutdown();
    settle();
    check(!controls->isVisible() && !controls->isWindow(), "shutdown removes the locked input tool");
}

static void checkSignedOutDanmaku() {
    QTemporaryDir settings;
    RoomRealtime service(nullptr);
    FloatingWindows windows(&service, settings.path());
    windows.showWindow(0);
    auto *dock = windows.dockWidget(0);
    auto *lock = dock->findChild<QPushButton *>("interaction-lock");
    auto *input = dock->findChild<QLineEdit *>("danmakuInput");
    auto *send = dock->findChild<QPushButton *>("danmakuSend");
    if (lock) lock->click();
    settle();
    check(windows.interactionLocked() && input && send && !input->isEnabled() && !send->isEnabled(),
          "locking an unauthenticated panel preserves the disabled editor and send button");
}
static void checkSettingsWheel(QWidget *panel) {
    auto *scroll = panel->findChild<QScrollArea *>();
    auto *toggle = panel->findChild<QPushButton *>("interaction-settings");
    const auto fields = panel->findChildren<QSpinBox *>();
    check(scroll && toggle && fields.size() == 2, "interaction settings has font and opacity controls");
    if (!scroll || !toggle || fields.size() != 2) return;
    panel->window()->resize(420, 300);
    toggle->click();
    panel->window()->activateWindow();
    settle();
    if (QApplication::platformName() == "minimal") {
        // The headless backend does not activate floating tool windows on request.
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(panel->window());
        QT_WARNING_POP
    }
    for (auto *field : fields) {
        toggle->setFocus(Qt::OtherFocusReason);
        scroll->ensureWidgetVisible(field);
        const auto center = field->rect().center();
        const int initial = field->value();
        QWheelEvent wheel(center, field->mapToGlobal(center), {}, QPoint(0, -120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(field, &wheel);
        check(!wheel.isAccepted() && field->value() == initial && field->focusPolicy() == Qt::StrongFocus,
              "unfocused settings leave the wheel for the dock without editing values");
        field->setFocus(Qt::TabFocusReason);
        QApplication::sendEvent(field, &wheel);
        check(wheel.isAccepted() && field->value() == initial - 1,
              "focused numeric setting accepts a deliberate wheel edit");
        field->setValue(initial);
    }
    toggle->click();
}

static void checkOnlineRanking(LiveController &controller) {
    QTemporaryDir dir;
    QStringList avatars;
    for (int i = 0; i < 8; ++i)
        avatars.append(QStringLiteral("https://i0.hdslb.com/bfs/face/test-rank-%1.png").arg(i));
    fixtureRankFaces = avatars;
    RoomRealtime service(&controller);
    fixtureRankFaces.clear();
    FloatingWindows windows(&service, dir.path());
    auto *cache = windows.findChild<DisplayAssetCache *>();
    check(cache, "the panel owns its public display asset cache");
    if (!cache) return;
    const QColor colors[]{QColor("#ce824a"), QColor("#3c91b6"), QColor("#b07db8"), QColor("#637cb4")};
    for (int i = 0; i < 8; ++i) {
        QPixmap face(32, 32);
        face.fill(colors[i % 4]);
        QPainter p(&face);
        p.setPen(Qt::NoPen);
        p.setBrush(colors[i % 4].lighter(170));
        p.drawEllipse(9, 5, 14, 14);
        p.drawEllipse(4, 21, 24, 18);
        p.end();
        cache->storePreview(avatars.at(i), DisplayAssetCache::Usage::Avatar, face);
    }
    windows.showWindow(1);
    auto *dock = windows.dockWidget(1);
    dock->resize(283, 560);
    service.audienceChanged();
    settle();
    auto *list = dock->findChild<QListView *>("audienceRanking");
    auto *tabs = dock->findChild<QTabBar *>("audienceMaster");
    auto *footer = dock->findChild<QLabel *>("audienceSummary");
    const auto *model = list ? dynamic_cast<AudienceRankModel *>(list->model()) : nullptr;
    check(model && model->rowCount() == 8 && tabs && tabs->count() == 5 && footer && footer->text().contains('8'),
          "online audience uses a ranked list, category tabs and the returned viewer count");
    if (tabs && tabs->count() == 5)
        check(tabs->rect().contains(tabs->tabRect(0)) && tabs->rect().contains(tabs->tabRect(4)),
              "all five default audience tabs fit at the reference dock width");
    if (!model || model->rowCount() != 8) return;
    const auto &first = model->row(0);
    const auto &hidden = model->row(7);
    check(first.name == QStringLiteral("听海的人") && first.rank == "1" && first.score == "1380" &&
          first.glory == 42 && first.medal == "Healli 30" && !model->index(0).data(Qt::DecorationRole).value<QPixmap>().isNull(),
          "first ranked viewer has avatar, wealth, fan badge and contribution score");
    check(hidden.name == QStringLiteral("神秘用户") && hidden.face.isEmpty() && !hidden.glory && hidden.medal.isEmpty() &&
          !hidden.sender.uid && model->complete(), "anonymous rank keeps only public rank and score");
    const auto dirName = qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS");
    if (!dirName.isEmpty()) {
        QDir().mkpath(dirName);
        dock->grab().save(dirName + "/online-ranking.png");
    }
}

static void checkInteractionSplitter(LiveController &controller) {
    QTemporaryDir dir;
    RoomRealtime service(&controller);
    QList<int> adjusted;
    for (int launch = 0; launch < 2; ++launch) {
        FloatingWindows windows(&service, dir.path());
        windows.showWindow(0);
        auto *dock = windows.dockWidget(0);
        dock->resize(380, 630);
        settle();
        auto *split = dock->findChild<QSplitter *>("interactionSplitter");
        auto *input = dock->findChild<QWidget *>("danmakuInputRow");
        auto *activity = dock->findChild<QCheckBox *>("anti");
        check(split && split->count() == 2 && input && activity &&
              split->widget(0)->objectName() == "messageHistory" && split->widget(1)->objectName() == "antiHistory",
              "chat and entries occupy two panes with the composer beneath them");
        if (!split || !input || !activity) return;
        if (launch == 1) {
            check(qAbs(split->sizes()[0] - adjusted.value(0)) <= 3, "the drag position survives a fresh panel instance");
            break;
        }
        auto *handle = split->handle(1);
        const auto before = split->sizes();
        const QPoint inputStart = input->mapTo(dock, QPoint());
        const QPoint start = handle->mapToGlobal(handle->rect().center());
        const QPoint end = start - QPoint(0, 56);
        auto mouse = [handle](QEvent::Type type, QPoint global, Qt::MouseButton button, Qt::MouseButtons down) {
            QMouseEvent e(type, QPointF(handle->mapFromGlobal(global)), QPointF(global), button, down, Qt::NoModifier);
            QApplication::sendEvent(handle, &e);
        };
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        settle();
        adjusted = split->sizes();
        check(adjusted[0] < before[0] - 35 && adjusted[1] > before[1] + 35 &&
              input->mapTo(dock, QPoint()) == inputStart, "dragging resizes both panes while the composer stays put");
        activity->setChecked(false);
        settle();
        check(!handle->isVisible() && split->sizes()[0] > adjusted[0], "hiding entries gives the space back to chat");
        activity->setChecked(true);
        settle();
        check(handle->isVisible() && qAbs(split->sizes()[0] - adjusted[0]) <= 3, "showing entries restores the split");
    }
}

static void checkThemeChanges(LiveController &controller) {
    const auto original = QApplication::palette();
    QTemporaryDir dir;
    RoomRealtime room(&controller);
    FloatingWindows windows(&room, dir.path());
    windows.showWindow(0);
    windows.showWindow(1);
    auto *dock = windows.dockWidget(0);
    dock->findChild<QPushButton *>("show-entry-history")->click();
    auto *chat = dock->findChild<QListView *>("messageHistory");
    auto *entries = dock->findChild<QListView *>("antiHistory");
    auto *history = dock->findChild<QListView *>("entry-history-list");
    auto *rank = windows.dockWidget(1)->findChild<QListView *>("audienceRanking");
    room.events({QJsonObject{{"cmd", "SEND_GIFT"}, {"data", QJsonObject{{"uname", "viewer"}, {"giftName", "flower"}, {"num", 1}}}}});
    settle();
    QPersistentModelIndex gift(chat->model()->index(0, 0));
    auto apply = [&](QColor window, QColor base, QColor ink, QColor primary) {
        auto p = original;
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::WindowText, ink);
        p.setColor(QPalette::Text, ink);
        p.setColor(QPalette::Highlight, primary);
        QApplication::setPalette(p);
        settle();
        const auto &colors = PanelPalette::current();
        for (auto *list : {chat, entries, history, rank}) {
            const auto image = list->viewport()->grab().toImage();
            check(!image.isNull() && image.pixelColor(2, image.height() - 2) == window,
                  "open interaction, rank and history viewports adopt the new host surface");
        }
        check(colors.surface == window && colors.giftAction != colors.giftName &&
              colors.contrast(colors.giftAction, colors.card) >= 4.5 &&
              colors.contrast(colors.giftName, colors.card) >= 4.5 &&
              colors.contrast(colors.entryStart, colors.card) >= 4.5 &&
              colors.contrast(colors.badgeText(colors.badge(primary)), colors.badge(primary)) >= 4.5,
              "gift, entry and badge lettering stays readable with the new host palette");
        check(gift.isValid() && chat->model()->rowCount() == 1,
              "theme changes preserve existing messages and model indices");
        return colors.accent;
    };
    const auto dark = apply("#1d1f26", "#272a33", "#ffffff", "#284cb8");
    const auto custom = apply("#1d1f26", "#272a33", "#ffffff", "#46976b");
    const auto light = apply("#d3d3d3", "#e5e5e5", "#0a0a0a", "#8cb5ff");
    check(dark != custom && custom != light, "changing only the host accent updates the accent of an open panel");
    QApplication::setPalette(original);
    settle();
}

static void checkInlineGift(LiveController &controller) {
    QTemporaryDir dir;
    QSettings settings(QDir(dir.path()).filePath("floating-windows.ini"), QSettings::IniFormat);
    settings.setValue("avatars", false);
    settings.setValue("font", 11);
    settings.sync();
    RoomRealtime service(&controller);
    DisplayAssetCache cache;
    FloatingWindows windows(&service, dir.path());
    windows.showWindow(0);
    auto *dock = windows.dockWidget(0);
    dock->resize(326, 640);
    auto *view = dock->findChild<QListView *>("messageHistory");
    auto *model = static_cast<DisplayListModel *>(view->model());
    model->setAssets(&cache, true);
    const QString url = QStringLiteral("https://s1.hdslb.com/bfs/live/test-ticket.png");
    const QString name = QStringLiteral("白忆寒_DreamWalker");
    QJsonObject gift{{"uname", name}, {"giftId", 34102}, {"giftName", QStringLiteral("人气票")},
                     {"num", 1}, {"tid", "gift-once"}, {"wealth_info", QJsonObject{{"level", 42}}},
                     {"medal_info", QJsonObject{{"medal_name", "Healli"}, {"medal_level", 16}, {"medal_color_start", 0x8b516f}}},
                     {"gift_info", QJsonObject{{"img_basic", "invalid"}, {"webp", url}}}};
    auto sendGift = [](const QJsonObject &data) { return QJsonObject{{"cmd", "SEND_GIFT"}, {"data", data}}; };
    auto chat = [](const QString &name, const QString &body, int glory, int level) {
        QJsonArray info{QJsonArray{}, body, QJsonArray{12, name}, QJsonArray{level, "Healli"}};
        while (info.size() < 16) info.append(QJsonValue::Null);
        info.append(QJsonArray{glory});
        return QJsonObject{{"cmd", "DANMU_MSG"}, {"info", info}};
    };
    service.events({chat(QStringLiteral("阻星_SunAster"), QStringLiteral("王离哥哥今天又在夺舍谁🐱"), 29, 3),
                    chat(QStringLiteral("空明AvA"), QStringLiteral("可惜"), 24, 9), sendGift(gift), sendGift(gift),
                    chat(QStringLiteral("小海"), QStringLiteral("收到了"), 12, 7)});
    settle();
    const auto index = model->index(2);
    const auto expected = QStringLiteral("[荣耀 42] [Healli 16] ") + name + QStringLiteral(" 投喂 人气票 🎁");
    const auto spans = index.data(DisplayListModel::ChatSpansRole).value<QList<ChatSpan>>();
    const auto images = index.data(DisplayListModel::EmotesRole).value<QList<DanmakuEmote>>();
    check(model->rowCount() == 4 && index.data().toString() == expected && spans.size() == 5 &&
          spans[3].kind == ChatSpan::GiftAction && spans[4].kind == ChatSpan::GiftName &&
          images.size() == 1 && images[0].url == url && images[0].start == expected.indexOf(QStringLiteral("🎁")),
          "one gift is interleaved between chats with badges, distinct action/name spans and an inline CDN image");
    QPixmap marker(20, 20);
    marker.fill(QColor("#32ef91"));
    cache.storePreview(url, DisplayAssetCache::Usage::Emote, marker);
    settle();
    const auto rect = view->visualRect(index);
    const auto pixels = view->viewport()->grab(rect).toImage();
    bool found = false;
    for (int y = 0; y < pixels.height() && !found; ++y)
        for (int x = 0; x < pixels.width() && !found; ++x)
            found = pixels.pixelColor(x, y) == QColor("#32ef91");
    check(found, "gift art actually paints inside the mixed row when avatars are disabled");
    const auto previewDir = qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS");
    const QJsonObject entry{{"msg_type", 1}, {"uid", 42}, {"uname", name}, {"fans_medal", gift.value("medal_info")},
                            {"tail_text", QStringLiteral("粉丝团")}};
    service.events({QJsonObject{{"cmd", "INTERACT_WORD"}, {"data", entry}}});
    settle();
    auto *entries = dock->findChild<QListView *>("antiHistory");
    const auto arrival = entries->model()->index(0, 0);
    const auto entryText = arrival.data().toString();
    check(entryText.contains("Healli") && entryText.indexOf(name) > entryText.indexOf("Healli") &&
          entryText.contains(QStringLiteral("进入直播间")) && entryText.endsWith(QStringLiteral("粉丝团")) &&
          !arrival.data(DisplayListModel::ChatSpansRole).value<QList<ChatSpan>>().isEmpty(),
          "entry card shows the supplied fan badge, nickname, entry action and trailing room label");
    if (!previewDir.isEmpty()) {
        const QPixmap sample(qEnvironmentVariable("LIVEHIME_UI_GIFT_IMAGE"));
        if (!sample.isNull()) cache.storePreview(url, DisplayAssetCache::Usage::Emote, sample);
        settle(1900);
        QDir().mkpath(previewDir);
        const auto last = view->visualRect(model->index(3));
        view->viewport()->grab(QRect(0, 0, view->viewport()->width(), last.bottom() + 6)).save(previewDir + "/inline-gift.png");
        dock->grab().save(previewDir + "/interaction-and-entries.png");
    }
    gift.insert("is_mystery", true);
    gift.insert("tid", "anonymous");
    gift.insert("num", 3);
    service.events({sendGift(gift)});
    settle();
    const auto anonymous = model->index(4);
    check(anonymous.data().toString() == QStringLiteral("神秘用户 投喂 人气票 🎁 ×3") &&
          !anonymous.data(DisplayListModel::SenderRole).isValid(), "multi-gift count remains visible without exposing anonymous badges");
}
static QImage paintHighlight(QListView *view, qint64 remainingMs) {
    QStandardItemModel model(1, 1);
    const auto index = model.index(0, 0);
    if (remainingMs)
        model.setData(index, QDeadlineTimer::current(Qt::PreciseTimer).deadline() + remainingMs,
                      DisplayListModel::HighlightDeadlineRole);
    QStyleOptionViewItem option;
    option.initFrom(view);
    option.rect = QRect(0, 0, 220, 32);
    option.palette.setColor(QPalette::Base, QColor("#18222a"));
    option.palette.setColor(QPalette::Highlight, QColor("#55c8fe"));
    QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(option.palette.color(QPalette::Base));
    QPainter painter(&image);
    view->itemDelegate()->paint(&painter, option, index);
    return image;
}

static void checkDanmakuEmotes(LiveController &controller) {
    QByteArray requests;
    QTcpServer proxy;
    check(proxy.listen(QHostAddress::LocalHost), "local emote request observer can listen");
    if (!proxy.isListening()) return;
    const auto previousProxy = QNetworkProxy::applicationProxy();
    const auto restoreProxy = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previousProxy); });
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", proxy.serverPort()));
    QObject::connect(&proxy, &QTcpServer::newConnection, &proxy, [&] {
        while (auto *socket = proxy.nextPendingConnection()) {
            auto reject = [&, socket] {
                if (!socket->canReadLine()) return;
                requests += socket->readLine();
                socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            };
            QObject::connect(socket, &QTcpSocket::readyRead, socket, reject);
            reject();
        }
    });

    QTemporaryDir dir;
    QSettings settings(QDir(dir.path()).filePath("floating-windows.ini"), QSettings::IniFormat);
    settings.setValue("avatars", false);
    settings.sync();
    RoomRealtime service(&controller);
    FloatingWindows windows(&service, dir.path());
    windows.showWindow(0);
    auto *view = windows.panelWidget(0)->findChild<QListView *>("messageHistory");
    check(view, "interaction exposes the danmaku list");
    if (!view) return;
    const QString first = QStringLiteral("[鼓掌]"), second = QStringLiteral("[干杯]");
    const QString urlA = QStringLiteral("https://i0.hdslb.com/bfs/emote/a.png");
    const QString urlB = QStringLiteral("https://i1.hdslb.com/bfs/emote/b.png");
    const QString user = QStringLiteral("阿") + first;
    const QString unknown = QStringLiteral("[普通括号]");
    const QString body = QStringLiteral("😀") + first + second + first + QStringLiteral("   ") + unknown;
    const QJsonObject definitions{{first, QJsonObject{{"url", urlA}, {"descript", first}}},
                                  {second, QJsonObject{{"url", urlB}, {"descript", second}}}};
    const QJsonObject extra{{"show_reply", true}, {"emots", definitions}};
    QJsonArray meta;
    for (int i = 0; i < 15; ++i) meta.append(0);
    const QJsonObject base{{"face", "https://i2.hdslb.com/bfs/face/avatar.png"}};
    meta.append(QJsonObject{{"extra", QString::fromUtf8(QJsonDocument(extra).toJson(QJsonDocument::Compact))},
                            {"user", QJsonObject{{"base", base}}}});
    service.events(QJsonArray{QJsonObject{{"cmd", "DANMU_MSG"}, {"info", QJsonArray{meta, body, QJsonArray{123, user}}}}});
    settle();
    const auto row = view->model()->index(0, 0);
    const auto text = row.data().toString();
    const auto spans = row.data(DisplayListModel::EmotesRole).value<QList<DanmakuEmote>>();
    check(text == user + QStringLiteral("：回复： ") + body && spans.size() == 3,
          "reply and username stay in display text alongside three inline emotes");
    if (spans.size() != 3) return;
    const qsizetype start = text.size() - body.size() + QStringLiteral("😀").size();
    check(spans[0].start == start && spans[0].length == first.size() && spans[0].url == urlA &&
          spans[1].start == start + first.size() && spans[1].length == second.size() && spans[1].url == urlB &&
          spans[2].start == start + first.size() + second.size() && spans[2].length == first.size() &&
          spans[2].url == urlA && !spans[0].standalone && !spans[1].standalone && !spans[2].standalone,
          "emote spans skip the username token, reply prefix and surrogate pair and retain repetitions");
    const auto missing = row.data(DisplayListModel::EmoteImagesRole).toMap();
    const auto avatar = row.data(Qt::DecorationRole).value<QPixmap>();
    for (int i = 0; i < 60 && !(requests.contains("i0.hdslb.com") && requests.contains("i1.hdslb.com")); ++i)
        settle(25);
    check(missing.isEmpty() && avatar.isNull() && requests.contains("i0.hdslb.com") && requests.contains("i1.hdslb.com") &&
          !requests.contains("i2.hdslb.com"), "disabling avatars still fetches both emotes through the offline proxy");

    QStandardItemModel fixture(1, 1);
    const auto index = fixture.index(0, 0);
    fixture.setData(index, text);
    QStyleOptionViewItem option;
    option.initFrom(view);
    option.rect = QRect(0, 0, 620, 92);
    option.palette.setColor(QPalette::Base, QColor("#181818"));
    option.palette.setColor(QPalette::Text, QColor("#eeeeee"));
    option.palette.setColor(QPalette::Highlight, QColor("#18ccfc"));
    auto paint = [&](const QList<DanmakuEmote> &tokens, const QVariantMap &images) {
        fixture.setData(index, QVariant::fromValue(tokens), DisplayListModel::EmotesRole);
        fixture.setData(index, images, DisplayListModel::EmoteImagesRole);
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(option.palette.color(QPalette::Base));
        QPainter painter(&image);
        view->itemDelegate()->paint(&painter, option, index);
        return image;
    };
    auto paintNeutralTheme = [&](const QList<DanmakuEmote> &tokens, const QVariantMap &images) {
        const QColor highlight = option.palette.color(QPalette::Highlight);
        option.palette.setColor(QPalette::Highlight, option.palette.color(QPalette::Base));
        const QImage image = paint(tokens, images);
        option.palette.setColor(QPalette::Highlight, highlight);
        return image;
    };
    const auto plain = paint({}, {});
    const auto pending = paint(spans, {});
    const auto neutralPending = paintNeutralTheme(spans, {});
    auto accent = [](QRgb pixel) {
        return qBlue(pixel) >= qGreen(pixel) ? qMax(0, qGreen(pixel) - qRed(pixel)) : 0;
    };
    auto accentBounds = [&](const QImage &image, const QImage &reference) {
        QRect result;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                if (accent(pixel) > 3 && pixel != reference.pixel(x, y))
                    result |= QRect(x, y, 1, 1);
            }
        return result;
    };
    const auto pendingArea = accentBounds(pending, neutralPending);
    const DanmakuEmote fakeSpan{int(text.indexOf(unknown)), int(unknown.size()), urlA, false};
    // Mark the unknown token only in this reference to locate its glyphs at the current font and DPI.
    const auto fakeArea = accentBounds(paint({fakeSpan}, {}), paintNeutralTheme({fakeSpan}, {}));
    check(plain == paintNeutralTheme({}, {}) && !pendingArea.isEmpty() && !fakeArea.isEmpty() &&
          pendingArea.right() < fakeArea.left(),
          "pending emote styling is confined to recognized spans and skips ordinary bracketed text");
    int strongest = 0;
    for (int y = pendingArea.top(); y <= pendingArea.bottom(); ++y)
        for (int x = pendingArea.left(); x <= pendingArea.right(); ++x)
            if (pending.pixel(x, y) != neutralPending.pixel(x, y))
                strongest = qMax(strongest, accent(pending.pixel(x, y)));
    int fillRows = 0, strongUpperPixels = 0, underlinePixels = 0, unchangedInk = 0;
    for (int y = pendingArea.top(); y <= pendingArea.bottom(); ++y) {
        int fillPixels = 0;
        for (int x = pendingArea.left(); x <= pendingArea.right(); ++x) {
            const auto pixel = pending.pixel(x, y);
            const auto strength = pixel == neutralPending.pixel(x, y) ? 0 : accent(pixel);
            if (strength > 3 && strength < strongest * 2 / 3) ++fillPixels;
            if (strength > 3 && strength >= strongest * 2 / 3) {
                if (y >= pendingArea.center().y()) ++underlinePixels;
                else ++strongUpperPixels;
            }
            if (qRed(pixel) > 200 && qRed(plain.pixel(x, y)) > 200) ++unchangedInk;
        }
        if (fillPixels > 3) ++fillRows;
    }
    check(fillRows >= 2 && strongUpperPixels == 0 && underlinePixels >= 2 && unchangedInk > 0,
          "pending tokens keep their original glyphs with a soft theme background and a stronger underline");
    QPixmap red(22, 20), green(22, 20);
    red.fill(QColor("#d82261"));
    green.fill(QColor("#24ca5a"));
    const QVariantMap one{{urlA, red}};
    const auto mixed = paint(spans, one);
    const auto unmarkedMiddle = paint({spans[0], spans[2]}, one);
    const auto middleArea = accentBounds(mixed, unmarkedMiddle);
    check(!middleArea.isEmpty() && middleArea.width() < pendingArea.width(),
          "in a mixed row the uncached middle token alone retains its pending decoration");
    const auto rendered = paint(spans, {{urlA, red}, {urlB, green}});
    check(rendered == paintNeutralTheme(spans, {{urlA, red}, {urlB, green}}),
          "decoded emotes no longer show the pending decoration");
    QList<QRgb> runs;
    QRgb previous = 0;
    for (int x = 0; x < rendered.width(); ++x) {
        QRgb color = 0;
        for (int y = 0; y < rendered.height(); ++y) {
            const QRgb pixel = rendered.pixel(x, y);
            if (pixel == qRgb(216, 34, 97) || pixel == qRgb(36, 202, 90)) { color = pixel; break; }
        }
        if (color && color != previous) runs.append(color);
        previous = color;
    }
    check(runs == QList<QRgb>{qRgb(216, 34, 97), qRgb(36, 202, 90), qRgb(216, 34, 97)},
          "the production delegate paints both inline images and the repeated first image in order");

    fixture.setData(index, first);
    auto single = spans[0];
    single.start = 0;
    const auto inlineImage = paint({single}, one);
    const auto inlineHeight = view->itemDelegate()->sizeHint(option, index).height();
    single.standalone = true;
    const auto standaloneImage = paint({single}, one);
    const auto standaloneHeight = view->itemDelegate()->sizeHint(option, index).height();
    auto colorRows = [](const QImage &image) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            const auto *pixels = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            if (std::find(pixels, pixels + image.width(), qRgb(216, 34, 97)) != pixels + image.width())
                ++count;
        }
        return count;
    };
    check(colorRows(standaloneImage) > colorRows(inlineImage) && standaloneHeight > inlineHeight &&
          standaloneHeight >= colorRows(standaloneImage), "standalone emotes paint larger and reserve their own row height");
}

static void captureFeature(QWidget *panel, const QString &filename, QWidget *popup = nullptr) {
    const auto folder = qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS");
    if (folder.isEmpty()) return;
    QDir().mkpath(folder);
    const QRect panelRect(panel->mapToGlobal(QPoint()), panel->size());
    const QRect popupRect = popup ? QRect(popup->mapToGlobal(QPoint()), popup->size()) : QRect{};
    const auto area = panelRect.united(popupRect);
    QPixmap shot(area.size());
    shot.fill(panel->palette().window().color());
    QPainter painter(&shot);
    painter.drawPixmap(panelRect.topLeft() - area.topLeft(), panel->grab());
    if (popup) painter.drawPixmap(popupRect.topLeft() - area.topLeft(), popup->grab());
    painter.end();
    check(shot.save(QDir(folder).filePath(filename)), "feature capture saved");
}

static void checkNarrowChatPreview(LiveController &controller) {
    QTcpServer offline;
    if (!offline.listen(QHostAddress::LocalHost)) { check(false, "loopback proxy available"); return; }
    const auto previous = QNetworkProxy::applicationProxy();
    const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previous); });
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", offline.serverPort()));
    QTemporaryDir dir;
    QSettings settings(QDir(dir.path()).filePath("floating-windows.ini"), QSettings::IniFormat);
    settings.setValue("showInlineGifts", false);
    settings.setValue("anti", false);
    settings.sync();
    RoomRealtime service(&controller);
    DisplayAssetCache cache;
    FloatingWindows windows(&service, dir.path());
    windows.showWindow(0);
    auto *dock = windows.dockWidget(0);
    dock->resize(335, 575);
    auto *view = windows.panelWidget(0)->findChild<QListView *>("messageHistory");
    auto *model = view ? dynamic_cast<DisplayListModel *>(view->model()) : nullptr;
    check(model, "viewport verification uses the live message model and view");
    if (!model) return;
    model->setAssets(&cache, true);
    using Phase = RealtimeConnection::Phase;
    auto *indicator = dock->findChild<QLabel *>("realtimeConnectionIndicator");
    check(indicator, "connection indicator belongs to the chat panel");
    if (!indicator) return;
    auto report = [&](Phase p, int attempt, QDeadlineTimer deadline, const QString &text) {
        service.connectionChanged({p, attempt, deadline, text});
    };
    report(Phase::Connected, 0, QDeadlineTimer::Forever, QStringLiteral("弹幕已连接"));

    auto chat = [](const QString &name, const QString &body) {
        return QJsonObject{{"cmd", "DANMU_MSG"}, {"info", QJsonArray{QJsonArray{}, body, QJsonArray{41, name}}}};
    };
    QJsonArray earlier;
    const QStringList lines{QStringLiteral("电台开播了"), QStringLiteral("今天也来听歌"),
                            QStringLiteral("耳机已就位"), QStringLiteral("晚安，明天见")};
    for (int i = 0; i < 40; ++i)
        earlier.append(chat(QStringLiteral("听众 %1").arg(i + 1), lines[i % lines.size()]));
    service.events(earlier);
    settle(2000); // Let earlier message highlights expire before highlighting the last arrival.
    const QString avatarUrl = QStringLiteral("https://i0.hdslb.com/bfs/test/avatar.png");
    const QString emoteUrl = QStringLiteral("https://i0.hdslb.com/bfs/test/room.png");
    const QString name = QStringLiteral("在海风里听晚间电台的长昵称观众");
    const QJsonObject user{{"uid", 42}, {"base", QJsonObject{{"name", name}, {"face", avatarUrl}}},
                           {"wealth", QJsonObject{{"level", 27}}}};
    QJsonArray meta;
    while (meta.size() < 16) meta.append(QJsonValue::Null);
    meta[12] = 1;
    meta[13] = QJsonObject{{"url", emoteUrl}};
    meta[15] = QJsonObject{{"user", user}, {"extra", QJsonObject{{"show_reply", true}}}};
    const QJsonArray info{meta, QStringLiteral("[晚安猫]"), QJsonArray{42, name}, QJsonArray{12, QStringLiteral("海边电台听友")}};
    service.events({QJsonObject{{"cmd", "DANMU_MSG"}, {"info", info}}});
    settle();
    const auto last = model->index(model->rowCount() - 1);
    check(last.data().toString().contains(QStringLiteral("回复")) &&
          last.data().toString().contains(QStringLiteral("海边电台听友")) &&
          last.data().toString().contains(QStringLiteral("荣耀 27")), "last message includes the wrapping reply, medal and glory");
    QPixmap avatar(32, 32);
    avatar.fill(QColor("#4b87a0"));
    {
        QPainter p(&avatar);
        p.setPen(Qt::white);
        p.drawText(avatar.rect(), Qt::AlignCenter, QStringLiteral("海"));
    }
    cache.storePreview(avatarUrl, DisplayAssetCache::Usage::Avatar, avatar);
    settle();
    auto *bar = view->verticalScrollBar();
    const auto pending = view->visualRect(last);
    check(bar->maximum() > 0 && bar->value() == bar->maximum() && !last.data(Qt::DecorationRole).value<QPixmap>().isNull() &&
          last.data(DisplayListModel::EmoteImagesRole).toMap().isEmpty(), "at bottom with real avatar and pending large image");
    captureFeature(dock, "01-highlight-and-placeholder.png");

    const QColor top("#18c8ad"), bottom("#f8687e");
    QPixmap sticker(96, 96);
    sticker.fill(QColor("#ffdb95"));
    {
        QPainter p(&sticker);
        p.fillRect(0, 0, 96, 6, top);
        p.fillRect(0, 90, 96, 6, bottom);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#cf9145"));
        p.drawPolygon(QPolygon({{14, 38}, {16, 12}, {38, 28}}));
        p.drawPolygon(QPolygon({{57, 26}, {80, 13}, {81, 42}}));
        p.setBrush(QColor("#efae62")); p.drawEllipse(QRect(10, 25, 77, 57));
        p.setPen(QPen(QColor("#492d32"), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(30, 51, 36, 48); p.drawLine(60, 48, 66, 51);
        p.drawArc(QRect(39, 53, 18, 12), 180 * 16, 180 * 16);
    }
    cache.storePreview(emoteUrl, DisplayAssetCache::Usage::Emote, sticker);
    settle(120);
    const auto rect = view->visualRect(last);
    const auto viewport = view->viewport()->rect();
    const auto pixels = view->viewport()->grab().toImage().scaled(viewport.size());
    auto bounds = [&](const QColor &color) {
        QRect matched;
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                if (pixels.pixelColor(x, y) == color) matched |= QRect(x, y, 1, 1);
        return matched;
    };
    const auto imageTop = bounds(top), imageBottom = bounds(bottom);
    const auto height = qBound(48, view->fontMetrics().height() * 3, 96);
    check(rect.height() > pending.height() && viewport.contains(rect.center()) && rect.bottom() <= viewport.bottom() &&
          bar->value() == bar->maximum(), "asynchronous larger emote remeasures the last row and retains the bottom");
    check(!imageTop.isEmpty() && !imageBottom.isEmpty() && rect.contains(imageTop) && rect.contains(imageBottom) &&
          imageTop.top() >= rect.top() + view->fontMetrics().height() &&
          qAbs(imageBottom.bottom() - imageTop.top() + 1 - height) <= 3,
          "both edges of the full-height image actually paint below the wrapped sender prefix inside the real viewport");
    captureFeature(dock, "02-full-emote-and-glory.png");

    report(Phase::Reconnecting, 2, QDeadlineTimer(1900, Qt::PreciseTimer), QStringLiteral("离线测试：节点连接中断"));
    settle();
    const auto alarm = indicator->grab().toImage();
    const auto red = alarm.pixelColor(5, 5);
    check(indicator->isVisible() && indicator->property("connectionState") == "reconnecting" &&
          indicator->text().contains(QStringLiteral("2 秒")) && red == PanelPalette::current().retrying,
          "a disconnected chat visibly displays a red warning, retry delay and attempt");
    captureFeature(dock, "03-connection-retry.png");
    settle(1000);
    check(indicator->text().contains(QStringLiteral("1 秒")) && indicator->text().contains(QStringLiteral("第 2 次")),
          "backoff countdown advances while the attempt stays the same");
    report(Phase::Reconnecting, 2, QDeadlineTimer::Forever, QStringLiteral("弹幕认证中"));
    check(indicator->text().contains(QStringLiteral("正在进行第 2 次")), "authentication continues to show a reconnect in progress");
    report(Phase::Connected, 0, QDeadlineTimer::Forever, QStringLiteral("弹幕已连接"));
    settle();
    const auto green = indicator->grab().toImage().pixelColor(5, 5);
    check(indicator->text() == QStringLiteral("弹幕已连接") && green == PanelPalette::current().connected && green != red,
          "successful recovery replaces the warning with a green connected status");
    captureFeature(dock, "03-connection-restored.png");
    if (qEnvironmentVariableIsEmpty("LIVEHIME_UI_SCREENSHOTS")) return;

    auto pt = rect.topLeft() + QPoint(18, 18);
    QContextMenuEvent rightClick(QContextMenuEvent::Mouse, pt, view->viewport()->mapToGlobal(pt));
    QApplication::sendEvent(view->viewport(), &rightClick);
    settle();
    if (auto *menu = view->findChild<QMenu *>("chatUserMenu"); menu && menu->isVisible()) {
        captureFeature(dock, "06-user-menu.png", menu);
        const auto actions = menu->actions();
        const auto mute = std::find_if(actions.cbegin(), actions.cend(), [](QAction *action) {
            return action->text().startsWith(QStringLiteral("禁言"));
        });
        if (mute != actions.cend() && (*mute)->isEnabled() && (*mute)->menu())
            (*mute)->menu()->actions().first()->trigger();
        menu->close();
        settle();
        if (auto *confirm = view->findChild<QMessageBox *>("chatUserConfirm"); confirm && confirm->isVisible()) {
            captureFeature(dock, "06-moderation-confirmation.png", confirm);
            confirm->reject();
        }
    }
    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    settle();
    service.events({chat(QStringLiteral("小鱼"), QStringLiteral("收到")), chat(QStringLiteral("阿树"), QStringLiteral("来了")),
                    chat(QStringLiteral("小夏"), QStringLiteral("今天的歌单好听"))});
    settle();
    if (auto *bubble = view->findChild<QPushButton *>("danmakuUnread"); bubble && bubble->isVisible())
        captureFeature(dock, "04-unread-bubble.png");
}

static void checkChatIdentityAndActions(LiveController &controller) {
    QTemporaryDir dir;
    QSettings settings(QDir(dir.path()).filePath("floating-windows.ini"), QSettings::IniFormat);
    settings.setValue("font", 21);
    settings.sync();
    RoomRealtime service(&controller);
    DisplayAssetCache cache;
    FloatingWindows windows(&service, dir.path());
    windows.showWindow(0);
    windows.dockWidget(0)->resize(490, 660);
    auto *view = windows.panelWidget(0)->findChild<QListView *>("messageHistory");
    auto *model = dynamic_cast<DisplayListModel *>(view->model());
    model->setAssets(&cache, true);
    const QString avatar = "https://i0.hdslb.com/bfs/test/chat-sender.png";
    QPixmap face(32, 32);
    face.fill(Qt::blue);
    cache.storePreview(avatar, DisplayAssetCache::Usage::Avatar, face);
    const QJsonObject user{{"uid", "9876543210"}, {"base", QJsonObject{{"name", "Alice"}, {"face", avatar}}},
                           {"wealth", QJsonObject{{"level", 23}}}};
    auto chat = [](const QJsonObject &user, const QString &id, qint64 legacyUid = 9876543210LL) {
        QJsonArray meta;
        while (meta.size() < 15) meta.append(QJsonValue::Null);
        meta.append(QJsonObject{{"user", user}, {"extra", QJsonObject{{"id_str", id}}}});
        QJsonArray info{meta, "hello", QJsonArray{legacyUid, "legacy"}};
        while (info.size() < 16) info.append(QJsonValue::Null);
        info.append(QJsonArray{7});
        return QJsonObject{{"cmd", "DANMU_MSG"}, {"info", info}, {"data", QJsonObject{{"tid", "shared"}}}};
    };
    auto hidden = user;
    hidden.insert("is_mystery", 1);
    const auto first = chat(user, "first");
    service.events({first, first, chat({}, "second"), chat(hidden, "third"), chat(user, "conflict", 78)});
    settle(140);
    auto index = model->index(0);
    const auto sender = index.data(DisplayListModel::SenderRole).value<DisplaySender>();
    check(model->rowCount() == 4 && sender.name == "Alice" && sender.uid == 9876543210LL &&
          sender.message == "hello" && index.data().toString().contains(QStringLiteral("[荣耀 23]")) &&
          model->index(1).data().toString().contains(QStringLiteral("[荣耀 7]")),
          "chat honours modern and legacy levels, binds the modern UID, and deduplicates by message ID");
    check(model->index(2).data(DisplayListModel::SenderRole).value<DisplaySender>().uid == 0 &&
          !model->index(2).data().toString().contains(QStringLiteral("荣耀")) &&
          model->index(2).data(Qt::DecorationRole).value<QPixmap>().isNull(),
          "mystery chat exposes no identity, glory or avatar");
    check(!model->index(3).data(DisplayListModel::SenderRole).isValid(), "conflicting IDs suppress the sender menu");
    auto menuAt = [&](QPoint pos) {
        QContextMenuEvent event(QContextMenuEvent::Mouse, pos, view->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(view->viewport(), &event);
        return view->findChild<QMenu *>("chatUserMenu");
    };
    auto removeClosed = [] { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); };
    const auto rect = view->visualRect(index);
    check(!menuAt(rect.topRight() + QPoint(-2, 8)), "blank row area does not open the sender menu");
    check(!menuAt(view->visualRect(model->index(2)).topLeft() + QPoint(8, 12)), "mystery sender has no menu");
    auto badgeFont = view->font();
    badgeFont.setPixelSize(qMax(9, qRound(view->fontMetrics().height() * .62)));
    badgeFont.setWeight(QFont::DemiBold);
    const int badgeWidth = QFontMetrics(badgeFont).horizontalAdvance(QStringLiteral("◆ 23")) + 10;
    const auto namePoint = rect.topLeft() + QPoint(6 + 38 + badgeWidth + view->fontMetrics().horizontalAdvance(" Ali"),
                                                   6 + view->fontMetrics().height() / 2);
    const auto avatarPoint = rect.topLeft() + QPoint(20, 20);
    check(!menuAt(rect.topLeft() + QPoint(6 + 38 + badgeWidth / 2, 6 + view->fontMetrics().height() / 2)),
          "the leading glory badge is outside the sender menu");
    auto *menu = menuAt(namePoint);
    check(menu && menu->isVisible(), "right clicking the actual sender name opens the menu");
    if (!menu) return;
    QStringList labels;
    for (auto *action : menu->actions())
        if (!action->isSeparator()) labels.append(action->text());
    check(labels == QStringList{QStringLiteral("拉黑"), QStringLiteral("禁言"), QStringLiteral("设置房管"),
                                QStringLiteral("访问空间")}, "sender menu offers four distinct supported actions");
    menu->close();
    removeClosed();
    fixtureModeration.clear();
    auto confirmAvatarMute = [&]() -> QMessageBox * {
        auto *popup = menuAt(avatarPoint);
        check(popup && popup->isVisible(), "right clicking the avatar also opens the menu");
        if (!popup) return nullptr;
        for (auto *action : popup->actions()) {
            if (action->text() != QStringLiteral("禁言") || !action->menu()) continue;
            QStringList durations;
            for (auto *choice : action->menu()->actions()) {
                durations.append(choice->text());
                if (choice->text() == QStringLiteral("4 小时")) choice->trigger();
            }
            check(durations == QStringList{QStringLiteral("2 小时"), QStringLiteral("4 小时"), QStringLiteral("24 小时")},
                  "mute menu exposes the supported fixed durations");
        }
        popup->close();
        removeClosed();
        return view->findChild<QMessageBox *>("chatUserConfirm");
    };
    auto *confirm = confirmAvatarMute();
    check(confirm && confirm->text().contains("9876543210") && confirm->text().contains(QStringLiteral("4 小时")) &&
          confirm->defaultButton() == confirm->button(QMessageBox::Cancel),
          "moderation confirmation identifies the UID and defaults to cancel");
    if (!confirm) return;
    confirm->button(QMessageBox::Cancel)->click();
    removeClosed();
    check(fixtureModeration.isEmpty(), "cancel never submits a moderation request");
    confirm = confirmAvatarMute();
    if (!confirm) return;
    confirm->button(QMessageBox::Yes)->click();
    removeClosed();
    check(fixtureModeration.size() == 1 && fixtureModeration.first().uid == sender.uid &&
          fixtureModeration.first().action == UserModeration::Mute && fixtureModeration.first().epoch == service.contextEpoch() &&
          fixtureModeration.first().message == "hello" && fixtureModeration.first().durationHours == 4,
          "confirmed mute sends exactly the selected user, duration and message");
    service.moderationFinished(true, QStringLiteral("操作成功"));
    auto *feedback = view->findChild<QMessageBox *>();
    check(feedback && feedback->text().contains("9876543210") && feedback->text().contains(QStringLiteral("操作成功")),
          "the platform response stays attached to the original sender and action");
    if (feedback) feedback->accept();
    removeClosed();
    confirm = confirmAvatarMute();
    if (!confirm) return;
    service.contextChanged();
    removeClosed();
    check(!view->findChild<QMessageBox *>("chatUserConfirm") && fixtureModeration.size() == 1,
          "switching rooms dismisses confirmation without submitting");

    QJsonArray history;
    for (int i = 0; i < 64; ++i) history.append(chat({}, QString::number(i + 10)));
    service.events(history);
    settle(200);
    auto *bar = view->verticalScrollBar();
    bar->triggerAction(QAbstractSlider::SliderPageStepSub);
    settle();
    auto *bubble = view->findChild<QPushButton *>("danmakuUnread");
    const auto again = chat({}, "fresh");
    service.events({again, again, chat({}, "also-fresh"), QJsonObject{{"cmd", "LIKE_INFO_V3_CLICK"}}});
    settle(180);
    check(bubble && bubble->isVisible() && bubble->text().contains(QStringLiteral("2 条新弹幕")) &&
          model->rowCount() == history.size() + 2 && bar->value() < bar->maximum(),
          "unread count and rendered rows agree on unique chats, excluding room activity");
    if (bubble) bubble->click();
    settle();
    check(bubble && !bubble->isVisible() && bar->value() == bar->maximum(), "bubble returns to the latest chat");
}

static void checkLiveHighlights(LiveController &controller) {
    QTemporaryDir dir;
    RoomRealtime service(&controller);
    FloatingWindows windows(&service, dir.path());
    windows.showWindow(0);
    auto *panel = windows.panelWidget(0);
    auto *messages = panel->findChild<QListView *>("messageHistory");
    auto *activity = panel->findChild<QListView *>("antiHistory");
    auto *openEntries = panel->findChild<QPushButton *>("show-entry-history");
    check(messages && activity && openEntries, "interaction exposes the views used for live highlights");
    if (!messages || !activity || !openEntries) return;

    const auto idle = paintHighlight(messages, 0);
    const auto early = paintHighlight(messages, 1100);
    const auto middle = paintHighlight(messages, 480);
    auto difference = [&idle](const QImage &image, int x) {
        const auto a = image.pixelColor(x, 16);
        const auto b = idle.pixelColor(x, 16);
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
    };
    check(difference(early, 12) > difference(middle, 12) && difference(middle, 12) > 0 &&
          difference(early, 12) > difference(early, 205) && paintHighlight(messages, -50) == idle,
          "row background weakens across its width and fades completely by its deadline");
    const auto fresh = paintHighlight(messages, 1750);
    check(difference(fresh, 12) >= 145 && difference(fresh, 205) >= 35 &&
          difference(fresh, 1) > difference(fresh, 12),
          "fresh highlights visibly cover the whole row with a stronger leading stripe");

    auto deadline = [](QListView *view, int row) {
        return view->model()->index(row, 0).data(DisplayListModel::HighlightDeadlineRole).toLongLong();
    };
    auto interaction = [](int type, const QString &id) {
        return QJsonObject{{"cmd", "INTERACT_WORD"}, {"data", QJsonObject{{"msg_type", type}, {"tid", id}, {"uname", "viewer"}}}};
    };
    const QJsonObject chat{{"cmd", "DANMU_MSG:4:0:2:2:2:0"},
                           {"info", QJsonArray{QJsonArray{}, "hello", QJsonArray{12, "viewer"}}}};
    const auto entry = interaction(1, "entry-once");
    const auto arrival = QDeadlineTimer::current(Qt::PreciseTimer).deadline();
    service.events({chat, entry, interaction(2, "follow"), interaction(3, "share"),
                    QJsonObject{{"cmd", "LIKE_INFO_V3_CLICK"}, {"data", QJsonObject{{"uname", "viewer"}}}},
                    QJsonObject{{"cmd", "SEND_GIFT"}, {"data", QJsonObject{{"giftName", "gift"}, {"num", 1}}}}});
    settle();
    const auto firstChat = deadline(messages, 0), firstEntry = deadline(activity, 0);
    check(messages->model()->rowCount() == 2 && activity->model()->rowCount() == 4 &&
          firstChat > arrival && firstEntry > arrival, "live danmaku and room entry get highlight deadlines");
    check(!deadline(activity, 1) && !deadline(activity, 2) && !deadline(activity, 3) &&
          !deadline(messages, 1), "follows, shares, likes and gifts stay unhighlighted");

    class PaintCounter final : public QObject {
      public:
        int paints = 0;
        bool eventFilter(QObject *, QEvent *e) override {
            if (e->type() == QEvent::Paint) ++paints;
            return false;
        }
    } frames;
    messages->viewport()->installEventFilter(&frames);
    settle(210);
    check(frames.paints >= 2, "visible danmaku animates without another service event");
    messages->viewport()->removeEventFilter(&frames);

    service.events({entry, chat}); // Redelivered entry ID, plus another identical-looking, ID-less chat.
    settle();
    windows.showWindow(0);
    check(messages->model()->rowCount() == 3 && deadline(messages, 0) == firstChat && !deadline(messages, 1) &&
          deadline(messages, 2) > firstChat, "a gift stays between the two independently highlighted chats");
    check(activity->model()->rowCount() == 4 && deadline(activity, 0) == firstEntry,
          "a replayed entry ID and a presentation rebuild keep the original entry deadline");

    openEntries->click();
    auto *dialog = panel->findChild<QDialog *>("entry-history-dialog");
    auto *history = dialog ? dialog->findChild<QListView *>("entry-history-list") : nullptr;
    check(history && history->model()->rowCount() == fixtureEntries.size(), "entry history opens with its initial snapshot");
    if (!history) return;
    const auto oldCount = static_cast<int>(fixtureEntries.size());
    check(!deadline(history, oldCount - 1), "opening entry history leaves its existing rows unhighlighted");
    const auto previousEntries = fixtureEntries;
    fixtureEntries.append({QStringLiteral("新增观众"), 1700000100, "test-new-entry", fixtureEntries.last().sequence + 1});
    emit service.entryHistoryChanged();
    settle(280);
    const auto historyDeadline = deadline(history, oldCount);
    check(history->model()->rowCount() == oldCount + 1 && historyDeadline > firstEntry &&
          !deadline(history, oldCount - 1), "new entry in an open history is highlighted separately from the snapshot");
    emit service.entryHistoryChanged();
    settle(280);
    check(deadline(history, oldCount) == historyDeadline, "refreshing an unchanged entry history preserves its deadline");
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    openEntries->click();
    auto *reopened = panel->findChild<QListView *>("entry-history-list");
    check(reopened && reopened->model()->rowCount() == oldCount + 1 && !deadline(reopened, oldCount),
          "reopening entry history treats the recently appended entry as existing history");
    fixtureEntries = previousEntries;

    const auto newest = deadline(messages, 2);
    windows.dockWidget(0)->hide();
    service.events({chat, interaction(1, "while-hidden")});
    windows.showWindow(0);
    check(messages->model()->rowCount() == 4 && activity->model()->rowCount() == 5 &&
          !deadline(messages, 3) && !deadline(activity, 4),
          "chat and entry received while interaction is closed appear as history without a flash");
    windows.dockWidget(0)->hide();
    settle(qMax(0, static_cast<int>(newest - QDeadlineTimer::current(Qt::PreciseTimer).deadline())) + 100);
    windows.showWindow(0);
    const auto now = QDeadlineTimer::current(Qt::PreciseTimer).deadline();
    check(messages->model()->rowCount() == 4 && deadline(messages, 0) <= now && deadline(messages, 2) <= now &&
          deadline(activity, 0) <= now, "reopening interaction cannot restart expired rows");
    service.contextChanged();
    check(!messages->model()->rowCount() && !activity->model()->rowCount(), "changing rooms clears highlighted interaction rows");
}

int main(int argc, char **argv) {
    const bool screenshots = !qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS").isEmpty();
    const bool interactiveDemo = qEnvironmentVariableIntValue("LIVEHIME_UI_DEMO") != 0;
    // qminimal normally uses a dummy font database; explicitly enable its Windows font engine.
    if ((screenshots || interactiveDemo) && qEnvironmentVariable("QT_QPA_PLATFORM").startsWith("minimal"))
        qputenv("QT_QPA_PLATFORM", "minimal:enable_fonts");
    QApplication app(argc, argv);
    check(privateDisplayName(QJsonObject{{"user_info", QJsonObject{{"is_mystery", 1}}},
                                        {"name", "secret"}}) == QStringLiteral("神秘用户"),
          "SC nested mystery identity protected");
    if (screenshots || interactiveDemo) {
        int fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        if (fontId < 0)
            fontId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/simhei.ttf"));
        check(fontId >= 0, "Chinese screenshot font loaded");
        QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
        QApplication::setStyle(QStringLiteral("Fusion"));
        QPalette palette;
        palette.setColor(QPalette::Window, QColor("#252a33"));
        palette.setColor(QPalette::WindowText, QColor("#f3f5f7"));
        palette.setColor(QPalette::Base, QColor("#181c24"));
        palette.setColor(QPalette::AlternateBase, QColor("#242a34"));
        palette.setColor(QPalette::Text, QColor("#f3f5f7"));
        palette.setColor(QPalette::PlaceholderText, QColor("#b1bac7"));
        palette.setColor(QPalette::Link, QColor("#89bdff"));
        palette.setColor(QPalette::Button, QColor("#343c49"));
        palette.setColor(QPalette::ButtonText, QColor("#f3f5f7"));
        palette.setColor(QPalette::Highlight, QColor("#5b73be"));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#929aa9"));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#929aa9"));
        QApplication::setPalette(palette);
    }
    QTemporaryDir dir;
    LiveController controller(dir.path());
    RoomRealtime service(&controller);
    checkDockFocus(service);
    checkLiveHighlights(controller);
    checkInteractionSplitter(controller);
    checkOnlineRanking(controller);
    checkThemeChanges(controller);
    checkInlineGift(controller);
    checkChatIdentityAndActions(controller);
    checkNarrowChatPreview(controller);
    checkDanmakuEmotes(controller);
    checkLockedDanmaku(service);
    checkSignedOutDanmaku();
    {
        QMainWindow host;
        host.show();
        ModerationAlerts alerts(&service, &host);
        auto *dialog = host.findChild<QDialog *>("livehime-moderation-dialog");
        auto *body = dialog->findChild<QTextBrowser *>();
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        ModerationNotice warning;
        warning.title = QStringLiteral("提示");
        warning.body = QStringLiteral("测试警告 <b>保留原文</b>");
        warning.id = "synthetic-1";
        service.moderationNotice(warning);
        service.moderationNotice(warning);
        check(dialog->isVisible() && dialog->windowModality() == Qt::WindowModal &&
              dialog->windowTitle() == warning.title && body->toPlainText() == warning.body,
              "standalone host modal shows literal server text without an interaction dock");
        check(buttons->buttons().size() == 1 &&
              buttons->buttons().first()->text() == QStringLiteral("我知道了"),
              "ordinary warning has the original acknowledgement");
        buttons->buttons().first()->click();
        settle();
        check(!dialog->isVisible(), "a redelivered warning is acknowledged once");
        auto *structured = host.findChild<QDialog *>("livehime-structured-moderation-dialog");
        auto *richBody = structured->findChild<QTextBrowser *>();
        ModerationNotice ecology;
        ecology.kind = ModerationNotice::Kind::Ecology;
        ecology.timestamp = 1;
        ecology.title = QStringLiteral("直播内容整改提醒");
        ecology.body = QStringLiteral("违规原因：测试画面包含不适宜展示的内容。\n整改要求：请切换画面后检查直播内容。");
        const QUrl details(QStringLiteral("https://link.bilibili.com/"));
        ecology.tips = {{{QStringLiteral("可前往"), {}}, {QStringLiteral("违规记录"), details},
                         {QStringLiteral("查看详情。"), {}}}};
        ecology.actions = {{QStringLiteral("查看详情"), details}, {QStringLiteral("我知道了"), {}}};
        service.moderationNotice(ecology);
        settle();
        check(structured->isVisible() && structured->windowTitle() == ecology.title &&
              richBody->toPlainText().contains(QStringLiteral("可前往违规记录查看详情。")),
              "ecology notice displays ordered text and tips");
        auto linkCursor = richBody->document()->find(QStringLiteral("违规记录"));
        check(!linkCursor.isNull() && linkCursor.charFormat().anchorHref() == details.toString() &&
              structured->findChild<QDialogButtonBox *>()->buttons().size() == 2,
              "structured tips are inline links distinct from action buttons");
        if (screenshots) {
            const QDir output(qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS"));
            output.mkpath(".");
            check(structured->grab().save(output.filePath("moderation-notice.png")), "save synthetic notice preview");
        }
        auto update = ecology;
        update.timestamp = 2;
        update.body = "Updated reason";
        service.moderationNotice(update);
        service.moderationNotice(ecology);
        check(richBody->toPlainText().startsWith(update.body), "stale ecology cannot overwrite a newer dialog");
        warning.id = "synthetic-2";
        service.moderationNotice(warning);
        check(!structured->isVisible() && dialog->isVisible(), "a warning immediately replaces a live ecology prompt");
        auto cutoff = update;
        cutoff.kind = ModerationNotice::Kind::CutOffV2;
        cutoff.title = "Cutoff";
        service.moderationNotice(cutoff);
        service.moderationNotice(ecology);
        check(structured->isVisible() && structured->windowTitle() == cutoff.title,
              "ecology cannot cover an active cutoff dialog");
        structured->reject();
        service.moderationNotice(ecology);
        check(structured->isVisible() && structured->windowTitle() == ecology.title,
              "timestamps only apply while a structured dialog is active");
        warning.id = "synthetic-3";
        service.moderationNotice(warning);
        service.contextChanged();
        settle();
        check(!dialog->isVisible() && !structured->isVisible() && body->toPlainText().isEmpty(),
              "room switch clears open and queued notices");
        service.moderationNotice(warning);
        check(dialog->isVisible(), "a new room has an independent message ID cache");
        dialog->reject();
        settle();
        check(!dialog->isVisible(), "acknowledgement stays local and stale notices are not replayed");
    }
    DisplayListModel bounded(3);
    for (int i = 0; i < 9; ++i)
        bounded.append(QString::number(i));
    check(bounded.rowCount() == 3, "bounded history");
    check(bounded.data(bounded.index(0)).toString() == "6", "evicts oldest");
    check(privateDisplayName(QJsonObject{{"name", "real name"}, {"is_mystery", 1}}) ==
              QStringLiteral("神秘用户"),
          "numeric mystery protected");
    check(privateDisplayName(QJsonObject{{"uinfo", QJsonObject{{"base", QJsonObject{{"name", "secret"}}},
                                                               {"anon", QJsonObject{{"name", "anon"}}}}}}) ==
              QStringLiteral("神秘用户"),
          "anonymous nested identity protected");
    auto fallback = audienceChoices({});
    check(fallback.size() == 5, "five protocol fallback masters");
    int combinations = 0;
    for (auto c : fallback)
        combinations += int(c.switches.size());
    check(combinations == 9, "nine protocol fallback combinations");
    QJsonObject sub{{"type", "intimacy_rank"},
                    {"title", QStringLiteral("亲密榜")},
                    {"status", 0},
                    {"default", 1},
                    {"switch", QJsonArray{QJsonObject{{"switch", "intimacy_rank"},
                                                      {"text", QStringLiteral("亲密天数")}}}}};
    auto configured = audienceChoices(QJsonObject{
        {"user_rank_tab_list", QJsonObject{{"tab", QJsonArray{QJsonObject{{"type", "contribution_tab"},
                                                                          {"status", 1},
                                                                          {"sub_tab", QJsonArray{sub}}}}}}}});
    check(configured.size() == 1 && configured[0].preferred && !configured[0].enabled &&
              configured[0].switches[0].value("switch") == "intimacy_rank",
          "server config keys/default/status retained");
    {
        QMainWindow host;
        host.setCentralWidget(new QLabel(QStringLiteral("合成 OBS 主窗口 · 无网络连接")));
        host.resize(1100,720);
        host.show();
        const auto hostFlags=host.windowFlags();
        auto registered=registerShells(host);
        FloatingWindows windows(&service,dir.path(),&host,registered);
        const char *expectedIds[]={"livehime-obs-interaction","livehime-obs-audience","livehime-obs-guard"};
        for(int k=0;k<FloatingWindows::DockCount;++k){
            check(QString::fromLatin1(FloatingWindows::dockId(k))==QString::fromLatin1(expectedIds[k]),"dock restore ID remains stable");
            check(windows.dockWidget(k)==registered[k],"reuses pre-registered OBS dock shell");
            check(windows.panelWidget(k)==registered[k]->widget(),"content injected into existing dock shell");
        }
        for(int k=0;k<3;++k){
            windows.showWindow(k);
            windows.dockWindow(k);
            check(!registered[k]->isFloating(),"each panel docks into the host");
            host.addDockWidget(Qt::BottomDockWidgetArea, registered[k]);
            host.resizeDocks({registered[k]}, {130}, Qt::Vertical);
            settle();
            auto *scroll = registered[k]->findChild<QScrollArea *>();
            check(registered[k]->height() < 200 && scroll && scroll->verticalScrollBar()->maximum() > 0,
                  "short realtime dock scrolls without forcing a taller host layout");
            if (scroll) {
                auto *layout = scroll->widget()->layout();
                QWidget *last = nullptr;
                for (int i = layout->count() - 1; i >= 0 && !last; --i) {
                    auto *widget = layout->itemAt(i)->widget();
                    if (widget && widget->isVisible())
                        last = widget;
                }
                scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                check(last && scroll->viewport()->rect().contains(last->mapTo(scroll->viewport(), last->rect().bottomLeft())),
                      "last visible realtime control is reachable in a short dock");
                host.resizeDocks({registered[k]}, {600}, Qt::Vertical);
                settle();
                check(scroll->verticalScrollBar()->maximum() == 0, "roomy realtime dock uses its list scrollbars only");
            }
            windows.floatWindow(k);
            check(registered[k]->isFloating(),"each panel can return to a floating window");
        }
        auto interaction=windows.dockWidget(0);
        auto panel=windows.panelWidget(0);
        windows.showWindow(0);
        checkSettingsWheel(panel);
        auto lock=panel->findChild<QPushButton *>("interaction-lock");
        auto pin=panel->findChild<QCheckBox *>("window-pin-0");
        auto mode=panel->findChild<QPushButton *>("window-mode-0");
        check(lock&&lock->isCheckable()&&pin&&mode,"native dock controls have stable test IDs");
        if(lock&&pin&&mode){
            check(lock->isEnabled(),"floating interaction can be locked");
            pin->click();
            check(host.windowFlags()==hostFlags,"floating pin toggle never pins the OBS host");
            pin->click();
            lock->click();
            check(windows.interactionLocked()&&lock->isChecked(),"lock reports its active state");
            check(interaction->windowFlags().testFlag(Qt::WindowTransparentForInput),"floating lock enables mouse passthrough");
#ifdef Q_OS_WIN
            if(QApplication::platformName()=="windows")
                check((GetWindowLongPtrW(reinterpret_cast<HWND>(interaction->winId()),GWL_EXSTYLE)&WS_EX_TRANSPARENT)!=0,
                      "lock sets native Windows passthrough style");
#endif
            windows.showWindow(0);
            checkUnlocked(windows,"external reopen clears mouse passthrough");
            lock->click();windows.unlockInteraction();
            checkUnlocked(windows,"external unlock clears mouse passthrough");
            // Exercise flag recovery independently of the remembered bool/button state.
            interaction->setWindowFlag(Qt::WindowTransparentForInput,true);
            interaction->show();windows.showWindow(0);
            checkUnlocked(windows,"reopen force clears a stale native passthrough flag");
            lock->click();windows.dockWindow(0);
            checkUnlocked(windows,"docking automatically unlocks interaction");
            check(!lock->isEnabled()&&!pin->isEnabled(),"dock mode disables floating-only lock and pin");
            check(host.windowFlags()==hostFlags,"docking and lock never modify OBS host flags");
            mode->click();
            check(interaction->isFloating(),"mode button undocks its panel");
            mode->click();
            check(!interaction->isFloating(),"mode button docks its panel");
            const auto resetLayout=host.saveState(24);
            windows.floatWindow(0);lock->click();
            check(host.restoreState(resetLayout,24),"native layout reset accepted");
            settle();
            checkUnlocked(windows,"restoring a docked layout clears interaction passthrough");
            windows.floatWindow(0);windows.showWindow(0);lock->click();interaction->hide();
            checkUnlocked(windows,"hiding a locked panel unlocks it");
            windows.showWindow(0);lock->click();interaction->close();
            checkUnlocked(windows,"closing a locked panel unlocks it");
            windows.showWindow(0);
            check(windows.dockWidget(0)==interaction,"close and reopen retains the same dock object");
            lock->click();windows.shutdown();
            checkUnlocked(windows,"shutdown clears locked native flags");
        }
        check(host.windowFlags()==hostFlags,"all floating tools preserve host window flags");
    }
    {
        FloatingWindows restarted(&service,dir.path());
        restarted.showWindow(0);
        checkUnlocked(restarted,"restart never restores a locked interaction window");
        restarted.shutdown();
    }
    QByteArray savedDockState,savedHostGeometry;
    {
        QMainWindow host;
        QTemporaryDir savedLayoutSettings;
        host.setCentralWidget(new QLabel(QStringLiteral("合成 OBS 布局保存")));
        host.resize(1100,720);host.show();
        auto registered=registerShells(host);
        FloatingWindows windows(&service,savedLayoutSettings.path(),&host,registered);
        for(int k=0;k<3;++k)windows.showWindow(k);
        windows.dockWindow(0);host.addDockWidget(Qt::RightDockWidgetArea,registered[0]);
        windows.floatWindow(1);registered[1]->setGeometry(40,50,370,540);
        windows.dockWindow(2);host.addDockWidget(Qt::BottomDockWidgetArea,registered[2]);registered[2]->hide();
        // Previous releases stored an independent gift dock in OBS's native layout.
        QDockWidget retiredGift(QStringLiteral("礼物流水"), &host);
        retiredGift.setObjectName("livehime-obs-gifts");
        host.addDockWidget(Qt::LeftDockWidgetArea, &retiredGift);
        retiredGift.show();
        settle();savedDockState=host.saveState(23);savedHostGeometry=host.saveGeometry();
        check(!savedDockState.isEmpty(),"host saved its native dock layout before content shutdown");
        windows.shutdown();
    }
    {
        QMainWindow host;
        host.setCentralWidget(new QLabel(QStringLiteral("合成 OBS 布局恢复")));
        QTemporaryDir freshPresentationSettings;
        auto registered=registerShells(host);
        host.restoreGeometry(savedHostGeometry);
        check(host.restoreState(savedDockState,23),"host restores stable empty dock shells before panel injection");
        host.show();settle();
        std::array<bool,FloatingWindows::DockCount> floating{},visible{};
        std::array<Qt::DockWidgetArea,FloatingWindows::DockCount> areas{};
        std::array<QRect,FloatingWindows::DockCount> geometries{};
        for(int k=0;k<FloatingWindows::DockCount;++k){floating[k]=registered[k]->isFloating();visible[k]=registered[k]->isVisible();areas[k]=host.dockWidgetArea(registered[k]);geometries[k]=registered[k]->geometry();}
        check(!floating[0]&&floating[1]&&!floating[2]&&!visible[2],"saved layout includes docked floating and hidden states");
        check(areas[0] == Qt::RightDockWidgetArea && !host.findChild<QDockWidget *>("livehime-obs-gifts"),
              "restoring an older OBS layout keeps interaction docked without recreating a gift dock");
        FloatingWindows windows(&service,freshPresentationSettings.path(),&host,registered);
        settle();
        for(int k=0;k<FloatingWindows::DockCount;++k){
            check(registered[k]->isFloating()==floating[k],"content injection preserves restored floating state");
            check(registered[k]->isVisible()==visible[k],"content injection preserves restored visibility");
            check(host.dockWidgetArea(registered[k])==areas[k],"content injection preserves restored dock area");
            if(floating[k]){
                checkIndependentFloat(registered[k], host);
                const auto minimum=registered[k]->minimumSizeHint();
                // qminimal's 240px virtual display can restore an empty shell narrower
                // than the real panel's 280px minimum; allow that necessary expansion.
                if(geometries[k].width()>=minimum.width()&&geometries[k].height()>=minimum.height())
                    check(registered[k]->geometry()==geometries[k],"content injection preserves restored floating geometry");
                else
                    check(registered[k]->size().width()>=minimum.width()&&registered[k]->size().height()>=minimum.height(),
                          "restored tiny virtual-screen shell expands only to usable panel constraints");
            }
        }
        windows.shutdown();
    }
    {
        QMainWindow host;
        QTemporaryDir panelSettings;
        host.resize(1100,720);
        host.show();
        auto shells=registerShells(host);
        auto windows=new FloatingWindows(&service,panelSettings.path(),&host,shells);
        windows->showWindow(0);
        QPointer<QSplitter> splitter = shells[0]->findChild<QSplitter *>("interactionSplitter");
        windows->shutdown();
        for(auto shell:shells)delete shell;
        check(splitter.isNull(), "removing OBS dock shells releases the interaction splitter");
        delete windows;
        service.events(QJsonArray{QJsonObject{{"cmd","WATCHED_CHANGE"},{"data",QJsonObject{{"num",3}}}}});
        settle();
        check(true,"shutdown dock removal then manager destruction leaves no dangling callbacks");
    }
    {
        FloatingWindows windows(&service, dir.path());
        for(int k=0;k<FloatingWindows::DockCount;++k)
            check(windows.dockWidget(k)->isFloating()&&!windows.dockWidget(k)->isVisible(),
                  "fallback shells initially float and remain hidden");
        for (int k = 0; k < 3; ++k)
            windows.showWindow(k);
        app.processEvents();
        auto a = window(0);
        check(a && window(1) && window(2), "three concurrently visible dock contents");
        windows.showWindow(0);
        check(window(0) == a, "reopen reuses instance");
        windows.dockWidget(0)->close();
        check(!a->isVisible(), "close hides");
        windows.showWindow(0);
        check(a->isVisible(), "hidden window restored");
        auto *entryButton = a->findChild<QPushButton *>("show-entry-history");
        check(entryButton, "interaction offers room entry history");
        if (entryButton) {
            entryButton->click();
            auto *dialog = a->findChild<QDialog *>("entry-history-dialog");
            auto *list = dialog ? dialog->findChild<QListView *>("entry-history-list") : nullptr;
            check(dialog && dialog->isVisible() && !dialog->isModal() && list && list->model()->rowCount() == 2,
                  "entry history opens without blocking the interaction dock");
            if (list) {
                auto *model = list->model();
                const auto firstTime = QDateTime::fromSecsSinceEpoch(1700000000).toString("MM-dd HH:mm:ss");
                check(model->index(0, 0).data().toString().startsWith(firstTime) &&
                      model->index(0, 0).data().toString().contains(QStringLiteral("早到的观众")) &&
                      model->index(1, 0).data().toString().contains(QStringLiteral("新到的观众")),
                      "entry history displays times and names in arrival order");
                if (screenshots)
                    dialog->grab().save(qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS") + "/entry-history.png");
                auto refresh = [&] {
                    QEventLoop updates;
                    bool refreshed = false;
                    QObject::connect(model, &QAbstractItemModel::modelReset, &updates, [&] {
                        refreshed = true;
                        updates.quit();
                    });
                    emit service.entryHistoryChanged();
                    QTimer::singleShot(2000, &updates, &QEventLoop::quit);
                    updates.exec();
                    check(refreshed, "open entry history refreshes when a new batch arrives");
                };
                for (int i = 3; i <= 500; ++i)
                    fixtureEntries.append({QString::number(i), 1700000000 + i, {}, i});
                refresh();
                check(list->verticalScrollBar()->value() == list->verticalScrollBar()->maximum(),
                      "entry history follows new arrivals at the bottom");
                list->scrollTo(model->index(70, 0), QAbstractItemView::PositionAtTop);
                list->verticalScrollBar()->setValue(list->verticalScrollBar()->value() + 3);
                const auto top = list->indexAt(QPoint(1, 0));
                const auto visible = top.data();
                const int offset = list->visualRect(top).top();
                fixtureEntries.removeFirst();
                fixtureEntries.append({QStringLiteral("501"), 1700000501, {}, 501});
                refresh();
                const auto newTop = list->indexAt(QPoint(1, 0));
                check(visible.isValid() && newTop.data() == visible && list->visualRect(newTop).top() == offset,
                      "evicting an old entry keeps the same viewer visible at the same offset");
            }
            entryButton->click();
            check(a->findChildren<QDialog *>("entry-history-dialog").size() == 1, "reopening reuses entry history");
            QPointer<QDialog> previous = dialog;
            emit service.contextChanged();
            settle();
            check(!previous || !previous->isVisible(), "changing room dismisses the previous room's entry history");
        }
        windows.dockWidget(0)->move(-20000, -20000);
        windows.dockWidget(0)->hide();
        windows.showWindow(0);
        check(QGuiApplication::primaryScreen()->availableGeometry().intersects(windows.dockWidget(0)->frameGeometry()),
              "offscreen correction");
        auto master = window(1)->findChild<QTabBar *>("audienceMaster");
        auto secondary = window(1)->findChild<QPushButton *>("audienceSecondary");
        for (int i = 0; i < 5; ++i) {
            master->setCurrentIndex(i);
            check(lastType == fallback[i].type, "master selects protocol type");
            secondary->click();
            check(
                lastSort ==
                    fallback[i].switches[fallback[i].switches.size() > 1 ? 1 : 0].value("switch").toString(),
                "secondary cycles protocol switch");
        }
        auto gm = window(2)->findChild<QComboBox *>("guardMaster");
        auto gs = window(2)->findChild<QPushButton *>("guardSecondary");
        gm->setCurrentIndex(1);
        check(lastGuard == 3, "guard month type");
        gm->setCurrentIndex(2);
        check(lastGuard == 5, "guard accompany intimacy");
        gs->click();
        check(lastGuard == 1, "guard accompany days");
        gm->setCurrentIndex(0);
        check(lastGuard == 4, "guard week type");
        QJsonArray info{QJsonArray{}, QStringLiteral("hello"), QJsonArray{123, QStringLiteral("viewer")}};
        service.events(QJsonArray{QJsonObject{{"cmd", "DANMU_MSG"}, {"info", info}},
                                  QJsonObject{{"cmd", "SEND_GIFT"},
                                              {"data", QJsonObject{{"uname", "viewer"},
                                                                   {"giftName", "gift"},
                                                                   {"num", 1},
                                                                   {"coin_type", "silver"}}}}});
        settle();
        auto msg = a->findChild<QListView *>("messageHistory");
        check(msg->model()->rowCount() == 2 && msg->model()->index(1, 0).data().toString().contains(QStringLiteral("投喂 gift")),
              "the gift follows the chat in the same list");
        for (auto c : a->findChildren<QCheckBox *>())
            if (c->objectName() == "paidOnly")
                c->setChecked(true);
        check(msg->model()->rowCount() == 1,
              "gift filter preserves chat history");
        service.contextChanged();
        check(msg->model()->rowCount() == 0, "context clears history");
        windows.shutdown();
    }
    {
        FloatingWindows windows(&service, dir.path());
        windows.showWindow(0);
        auto a = window(0);
        auto msg = a->findChild<QListView *>("messageHistory");
        QCheckBox *pause = nullptr;
        for (auto c : a->findChildren<QCheckBox *>())
            if (c->text().contains(QStringLiteral("悬停")))
                pause = c;
        check(pause != nullptr, "pause setting exists");
        pause->setChecked(true);
        QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(msg, &enter);
        service.events(
            QJsonArray{QJsonObject{{"cmd", "DANMU_MSG"},
                                   {"info", QJsonArray{QJsonArray{}, "paused", QJsonArray{1, "test"}}}},
                       QJsonObject{{"cmd", "DANMU_MSG"}, {"info", QJsonArray{}}}});
        settle();
        check(msg->model()->rowCount() == 0, "hover freezes model while receiving");
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(msg, &leave);
        settle();
        check(msg->model()->rowCount() == 1,
              "leave displays pending messages and safely ignores malformed event");
        pause->setChecked(false);
        windows.shutdown();
    }
    {
        auto transient = new RoomRealtime(&controller);
        FloatingWindows windows(transient, dir.path());
        windows.showWindow(0);
        delete transient;
        windows.shutdown();
        check(true, "view safely outlives stopped realtime service");
    }
    {
        FloatingWindows windows(&service, dir.path());
        windows.showWindow(0);
        auto panel = window(0);
        auto list = panel->findChild<QListView *>("messageHistory");
        const auto body = QStringLiteral("完整长弹幕：中文自动换行与 emoji 😀，行尾必须完整可读。 ").repeated(12);
        service.events(QJsonArray{QJsonObject{{"cmd", "DANMU_MSG"},
            {"info", QJsonArray{QJsonArray{}, body, QJsonArray{1, "test"}}}}});
        settle();
        int wideHeight = 0;
        for (int width : {420, 280}) {
            windows.dockWidget(0)->resize(width, 600);
            QApplication::processEvents();
            list->doItemsLayout();
            QStyleOptionViewItem option;
            option.font = list->font();
            option.rect = list->viewport()->rect();
            const auto index = list->model()->index(0, 0);
            const auto hint = list->itemDelegate()->sizeHint(option, index);
            QTextLayout layout(index.data().toString(), list->font());
            QTextOption textOption;
            textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(textOption);
            layout.beginLayout();
            qreal height = 0;
            int characters = 0;
            while (true) {
                auto line = layout.createLine();
                if (!line.isValid()) break;
                line.setLineWidth(list->viewport()->width() - 12);
                height += line.height();
                characters += line.textLength();
            }
            layout.endLayout();
            check(characters == index.data().toString().size(), "wrapped layout includes every character");
            check(hint.height() >= qCeil(height) + 12, "row height contains all wrapped text at 280/420 widths");
            if (width == 420) wideHeight = hint.height();
            else check(hint.height() > wideHeight, "narrow resize relayout increases full row height");
        }
        windows.shutdown();
    }
    if (!qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS").isEmpty()) {
        auto output = qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS");
        QDir().mkpath(output);
        FloatingWindows windows(&service, dir.path());
        for (int k = 0; k < 3; ++k) {
            windows.showWindow(k);
            windows.dockWidget(k)->resize(k == 0 ? 420 : 350, 650);
        }
        service.audienceChanged();
        service.guardChanged();
        QJsonArray batch;
        for (int i = 0; i < 14; ++i) {
            QJsonArray info{
                QJsonArray{},
                QStringLiteral("合成弹幕 %1：长文字自动换行，中文与 emoji 😀，这是离线界面验证。").arg(i),
                QJsonArray{123, QStringLiteral("合成长昵称观众与粉丝牌展示")}};
            batch.append(QJsonObject{{"cmd", "DANMU_MSG"}, {"info", info}});
        }
        batch.append(QJsonObject{{"cmd", "SEND_GIFT"},
                                 {"data", QJsonObject{{"name", QStringLiteral("合成礼物观众")},
                                                      {"giftName", QStringLiteral("测试礼物")},
                                                      {"num", 3},
                                                      {"coin_type", "gold"},
                                                      {"total_coin", 300}}}});
        batch.append(QJsonObject{
            {"cmd", "SUPER_CHAT_MESSAGE"},
            {"data", QJsonObject{{"is_mystery", 1},
                                 {"user_info", QJsonObject{{"uname", "never show"}}},
                                 {"message", QStringLiteral("合成匿名醒目留言，身份不得泄露")}}}});
        batch.append(
            QJsonObject{{"cmd", "INTERACT_WORD"},
                        {"data", QJsonObject{{"name", QStringLiteral("合成新观众")}, {"msg_type", 1}}}});
        service.events(batch);
        settle();
        app.processEvents();
        QPixmap canvas(1140, 690);
        canvas.fill(QColor("#242424"));
        QPainter painter(&canvas);
        for (int k = 0; k < 3; ++k)
            painter.drawPixmap(k == 0 ? 0 : k == 1 ? 430 : 790, 20, windows.dockWidget(k)->grab());
        painter.end();
        canvas.save(output + "/three-windows.png");
        windows.dockWidget(0)->resize(280, 560);
        app.processEvents();
        windows.dockWidget(0)->grab().save(output + "/interaction-narrow.png");
        windows.shutdown();
    }
    if(screenshots||interactiveDemo){
        if(interactiveDemo){
            QSettings demoSettings(QDir(dir.path()).filePath("floating-windows.ini"),QSettings::IniFormat);
            demoSettings.setValue("opacity",100);
            demoSettings.sync();
        }
        QMainWindow host;
        host.setWindowTitle(QStringLiteral("Livehime OBS 0.2.2 · 离线合成停靠与锁定验证"));
        auto center=new QWidget;
        auto centerLayout=new QVBoxLayout(center);
        auto preview=new QLabel(QStringLiteral("离线合成 OBS 主窗口\n原生 QDockWidget 停靠 / 浮动\n无真实账号、推流或网络连接"));
        preview->setAlignment(Qt::AlignCenter);centerLayout->addWidget(preview,1);
        auto reopen=new QPushButton(QStringLiteral("打开直播互动（并解锁）"));
        reopen->setObjectName("demo-reopen-interaction");
        auto unlock=new QPushButton(QStringLiteral("解除互动穿透"));
        unlock->setObjectName("demo-unlock-interaction");
        centerLayout->addWidget(reopen);centerLayout->addWidget(unlock);center->setMinimumWidth(270);
        host.setCentralWidget(center);host.resize(1200,760);host.move(30,40);
        auto shells=registerShells(host);
        FloatingWindows windows(&service,dir.path(),&host,shells);
        QObject::connect(reopen,&QPushButton::clicked,&host,[&windows]{windows.showWindow(0);});
        QObject::connect(unlock,&QPushButton::clicked,&host,[&windows]{windows.unlockInteraction();});
        for(int k=0;k<3;++k){windows.showWindow(k);windows.dockWindow(k);}
        host.addDockWidget(Qt::LeftDockWidgetArea,shells[0]);
        host.addDockWidget(Qt::RightDockWidgetArea,shells[1]);
        host.addDockWidget(Qt::RightDockWidgetArea,shells[2]);host.tabifyDockWidget(shells[1],shells[2]);shells[1]->raise();
        host.show();service.audienceChanged();service.guardChanged();
        QJsonArray batch;
        for(int i=0;i<8;++i)batch.append(QJsonObject{{"cmd","DANMU_MSG"},{"info",QJsonArray{QJsonArray{},QStringLiteral("合成互动 %1：支持浮动、原生停靠、关闭重开与外部解锁。").arg(i),QJsonArray{1,QStringLiteral("合成观众")}}}});
        service.events(batch);settle();
        if(screenshots){
            auto output=qEnvironmentVariable("LIVEHIME_UI_SCREENSHOTS");
            host.grab().save(output+"/dock-host.png");
            windows.floatWindow(0);windows.dockWidget(0)->resize(420,650);settle();
            windows.dockWidget(0)->grab().save(output+"/dock-floating.png");
        }
        if(interactiveDemo){
            // Explicit opt-in leaves only this synthetic, no-network fixture interactive.
            // Normal CTest never enters this event loop.
            windows.floatWindow(0);windows.showWindow(0);
            windows.dockWidget(0)->resize(420,600);
            windows.dockWidget(0)->move(host.pos()+QPoint(60,70));
            windows.dockWidget(0)->setWindowOpacity(1.0);
            QTimer::singleShot(180000,&app,&QCoreApplication::quit);
            app.exec();
        }
        windows.shutdown();
    }
    return failures ? 1 : 0;
}
