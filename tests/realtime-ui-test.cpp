// Real presentation widgets with a main-thread, no-network realtime service double.
#include "floating-windows.hpp"
#include "moderation-alerts.hpp"
#include "room-realtime.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFontDatabase>
#include <QJsonArray>
#include <QListView>
#include <QMainWindow>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QTextBrowser>
#include <QtMath>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif
#include <iostream>

namespace livehime {
SessionStore::SessionStore(QString p) : directory_(std::move(p)) {}
LiveController::LiveController(QString p, QObject *o)
    : QObject(o), store_(std::move(p)), client_(nullptr), bridge_(nullptr) {}
LiveController::~LiveController() = default;
RoomRealtime::RoomRealtime(LiveController *c, QObject *p) : QObject(p), controller_(c), goim_(nullptr) {
    status_ = QStringLiteral("离线合成数据 · 布局验证");
    audienceStatus_ = guardStatus_ = QStringLiteral("离线合成数据");
    audience_ = QJsonObject{
        {"item",
         QJsonArray{
             QJsonObject{{"rank", 1},
                         {"name", QStringLiteral("离线合成观众 · 测试昵称")},
                         {"score", 246},
                         {"guard_level", 3}},
             QJsonObject{{"rank", 2}, {"name", "must never display"}, {"is_mystery", 1}, {"score", 120}}}},
        {"config", QJsonObject{{"value_text", QStringLiteral("亲密度")}}}};
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
static void settle() {
    QEventLoop loop;
    QTimer::singleShot(80, &loop, &QEventLoop::quit);
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
static std::array<QDockWidget *,3> registerShells(QMainWindow &host) {
    std::array<QDockWidget *,3> docks{};
    const QStringList titles={QStringLiteral("直播互动"),QStringLiteral("房间观众"),QStringLiteral("大航海")};
    for(int k=0;k<3;++k){
        docks[k]=new QDockWidget(titles[k],&host);
        docks[k]->setObjectName(QString::fromLatin1(FloatingWindows::dockId(k)));
        docks[k]->setWidget(new QWidget);
        host.addDockWidget(Qt::RightDockWidgetArea,docks[k]);
        docks[k]->hide();
    }
    return docks;
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
        for(int k=0;k<3;++k){
            check(QString::fromLatin1(FloatingWindows::dockId(k))==QString::fromLatin1(expectedIds[k]),"dock restore ID remains stable");
            check(windows.dockWidget(k)==registered[k],"reuses pre-registered OBS dock shell");
            check(windows.panelWidget(k)==registered[k]->widget(),"content injected into existing dock shell");
            windows.showWindow(k);
            windows.dockWindow(k);
            check(!registered[k]->isFloating(),"each panel docks into the host");
            windows.floatWindow(k);
            check(registered[k]->isFloating(),"each panel can return to a floating window");
        }
        auto interaction=windows.dockWidget(0);
        auto panel=windows.panelWidget(0);
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
        host.setCentralWidget(new QLabel(QStringLiteral("合成 OBS 布局保存")));
        host.resize(1100,720);host.show();
        auto registered=registerShells(host);
        FloatingWindows windows(&service,dir.path(),&host,registered);
        for(int k=0;k<3;++k)windows.showWindow(k);
        windows.dockWindow(0);host.addDockWidget(Qt::RightDockWidgetArea,registered[0]);
        windows.floatWindow(1);registered[1]->setGeometry(40,50,370,540);
        windows.dockWindow(2);host.addDockWidget(Qt::BottomDockWidgetArea,registered[2]);registered[2]->hide();
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
        std::array<bool,3> floating{},visible{};
        std::array<Qt::DockWidgetArea,3> areas{};
        std::array<QRect,3> geometries{};
        for(int k=0;k<3;++k){floating[k]=registered[k]->isFloating();visible[k]=registered[k]->isVisible();areas[k]=host.dockWidgetArea(registered[k]);geometries[k]=registered[k]->geometry();}
        check(!floating[0]&&floating[1]&&!floating[2]&&!visible[2],"saved layout includes docked floating and hidden states");
        FloatingWindows windows(&service,freshPresentationSettings.path(),&host,registered);
        settle();
        for(int k=0;k<3;++k){
            check(registered[k]->isFloating()==floating[k],"content injection preserves restored floating state");
            check(registered[k]->isVisible()==visible[k],"content injection preserves restored visibility");
            check(host.dockWidgetArea(registered[k])==areas[k],"content injection preserves restored dock area");
            if(floating[k]){
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
        host.show();
        auto shells=registerShells(host);
        auto windows=new FloatingWindows(&service,dir.path(),&host,shells);
        windows->floatWindow(0);windows->showWindow(0);
        auto panel=windows->panelWidget(0);
        auto gift=panel->findChild<QListView *>("giftHistory");
        for(auto button:panel->findChildren<QPushButton *>())
            if(button->text().contains(QStringLiteral("拆分/合回")))button->click();
        QPointer<QWidget> splitWindow=gift->window();
        check(splitWindow&&splitWindow!=shells[0],"lifecycle fixture opens split gift window");
        windows->shutdown();
        for(auto shell:shells)delete shell;
        check(splitWindow.isNull(),"OBS dock removal destroys its split gift child");
        delete windows;
        service.events(QJsonArray{QJsonObject{{"cmd","WATCHED_CHANGE"},{"data",QJsonObject{{"num",3}}}}});
        settle();
        check(true,"shutdown dock removal then manager destruction leaves no dangling callbacks");
    }
    {
        FloatingWindows windows(&service, dir.path());
        for(int k=0;k<3;++k)
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
        windows.dockWidget(0)->move(-20000, -20000);
        windows.dockWidget(0)->hide();
        windows.showWindow(0);
        check(QGuiApplication::primaryScreen()->availableGeometry().intersects(windows.dockWidget(0)->frameGeometry()),
              "offscreen correction");
        auto master = window(1)->findChild<QComboBox *>("audienceMaster");
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
        auto gift = a->findChild<QListView *>("giftHistory");
        check(msg->model()->rowCount() == 1 && gift->model()->rowCount() == 1,
              "events route to separate models");
        for (auto c : a->findChildren<QCheckBox *>())
            if (c->objectName() == "paidOnly")
                c->setChecked(true);
        check(gift->model()->rowCount() == 0 && msg->model()->rowCount() == 1,
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
        auto gift = a->findChild<QListView *>("giftHistory");
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
        auto shared = gift->model();
        for (auto b : a->findChildren<QPushButton *>())
            if (b->text().contains(QStringLiteral("拆分/合回"))) {
                b->click();
                check(gift->model() == shared && gift->window() != windows.dockWidget(0), "split keeps shared gift model");
                b->click();
                check(gift->model() == shared && gift->window() == windows.dockWidget(0), "rejoin keeps shared gift model");
            }
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
