#include "floating-windows.hpp"
#include "display-asset-cache.hpp"
#include "room-realtime.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QtMath>
#include <functional>

namespace livehime {
static QJsonValue av(const QJsonArray &a, int i) { return i >= 0 && i < a.size() ? a.at(i) : QJsonValue{}; }
DisplayListModel::DisplayListModel(int limit, QObject *p) : QAbstractListModel(p), limit_(qMax(1, limit)) {}
int DisplayListModel::rowCount(const QModelIndex &p) const { return p.isValid() ? 0 : int(rows_.size()); }
QVariant DisplayListModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() < 0 || i.row() >= rows_.size())
        return {};
    if (role == Qt::DecorationRole && assets_ && showAvatars_ && !avatars_[i.row()].isEmpty())
        return QVariant::fromValue(assets_->get(avatars_[i.row()]));
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return rows_[i.row()];
    return {};
}
void DisplayListModel::setAssets(DisplayAssetCache *c) {
    assets_ = c;
    connect(c, &DisplayAssetCache::ready, this, [this] {
        if (rowCount())
            emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole});
    });
}
void DisplayListModel::setAvatarsVisible(bool v) {
    showAvatars_ = v;
    if (rowCount())
        emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole});
}
void DisplayListModel::append(const QString &t, const QString &avatar) {
    if (rows_.size() >= limit_) {
        beginRemoveRows({}, 0, 0);
        rows_.removeFirst();
        avatars_.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, int(rows_.size()), int(rows_.size()));
    rows_.append(t.left(8192));
    avatars_.append(avatar);
    endInsertRows();
}
void DisplayListModel::replace(const QStringList &r) {
    beginResetModel();
    rows_ = r.mid(0, limit_);
    avatars_ = QStringList(rows_.size(), QString{});
    endResetModel();
}
void DisplayListModel::clear() { replace({}); }
static QJsonObject userObject(const QJsonObject &r) {
    auto u = r.value("uinfo").toObject();
    if (u.isEmpty())
        u = r.value("sender_uinfo").toObject();
    if (u.isEmpty())
        u = r.value("user").toObject();
    if (u.isEmpty())
        u = r.value("user_info").toObject();
    return u;
}
static bool mystery(const QJsonObject &r) {
    const auto u = userObject(r);
    return !r.value("anon").toObject().isEmpty() || u.value("is_mystery").toBool() ||
           u.value("is_mystery").toInt() != 0 || r.value("is_mystery").toBool() ||
           r.value("is_mystery").toInt() != 0 || u.value("base").toObject().value("is_mystery").toBool() ||
           u.value("base").toObject().value("is_mystery").toInt() != 0 ||
           !u.value("anon").toObject().isEmpty();
}
QString privateDisplayName(const QJsonObject &r) {
    if (mystery(r))
        return QStringLiteral("神秘用户");
    auto name = userObject(r).value("base").toObject().value("name").toString();
    if (name.isEmpty())
        name = r.value("uname").toString(r.value("name").toString(r.value("username").toString()));
    return name.isEmpty() ? QStringLiteral("观众") : name.left(120);
}
QList<RankChoice> audienceChoices(const QJsonObject &info) {
    QList<RankChoice> out;
    for (auto tv : info.value("user_rank_tab_list").toObject().value("tab").toArray()) {
        auto tab = tv.toObject();
        if (tab.value("type") != "contribution_tab")
            continue;
        for (auto sv : tab.value("sub_tab").toArray()) {
            auto sub = sv.toObject();
            RankChoice c;
            c.type = sub.value("type").toString();
            c.title = sub.value("title").toString(c.type);
            c.config = sub;
            c.enabled = tab.value("status").toInt() != 0 && sub.value("status").toInt() != 0;
            c.preferred = sub.value("default").toInt() == 1;
            for (auto v : sub.value("switch").toArray())
                c.switches.append(v.toObject());
            if (!c.type.isEmpty() && !c.switches.isEmpty())
                out.append(c);
        }
    }
    if (!out.isEmpty())
        return out;
    const QStringList types = {"online_rank", "intimacy_rank", "daily_rank", "weekly_rank", "monthly_rank"};
    const QList<QStringList> sorts = {{"contribution_rank", "entry_time_rank"},
                                      {"intimacy_rank"},
                                      {"today_rank", "yesterday_rank"},
                                      {"current_week_rank", "last_week_rank"},
                                      {"current_month_rank", "last_month_rank"}};
    const QStringList titles = {QStringLiteral("在线榜"), QStringLiteral("亲密榜"), QStringLiteral("日榜"),
                                QStringLiteral("周榜"), QStringLiteral("月榜")};
    const QList<QStringList> labels = {{QStringLiteral("贡献值"), QStringLiteral("进房时间")},
                                       {QStringLiteral("亲密天数")},
                                       {QStringLiteral("当日"), QStringLiteral("昨日")},
                                       {QStringLiteral("本周"), QStringLiteral("上周")},
                                       {QStringLiteral("本月"), QStringLiteral("上月")}};
    for (int i = 0; i < types.size(); ++i) {
        RankChoice c;
        c.type = types[i];
        c.title = titles[i];
        for (int j = 0; j < sorts[i].size(); ++j)
            c.switches.append(QJsonObject{
                {"switch", sorts[i][j]},
                {"text", labels[i][j]},
                {"ui_type", QJsonObject{{"show_score", sorts[i][j] == "entry_time_rank" ? 0 : 1}}}});
        out.append(c);
    }
    return out;
}

class Panel final : public QWidget {
  public:
    std::function<void(bool)> visibility;
    std::function<void()> save;
    explicit Panel(QWidget *p, bool floating = true) : QWidget(p, floating ? Qt::Window : Qt::Widget) {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setMinimumSize(280, 240);
    }

  protected:
    void closeEvent(QCloseEvent *e) override {
        hide();
        e->ignore();
    }
    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        if (visibility)
            visibility(true);
    }
    void hideEvent(QHideEvent *e) override {
        QWidget::hideEvent(e);
        if (save)
            save();
        if (visibility)
            visibility(false);
    }
};
class WrappedHistoryDelegate final : public QStyledItemDelegate {
  public:
    explicit WrappedHistoryDelegate(QListView *view) : QStyledItemDelegate(view), view_(view) {}
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const bool avatar = !qvariant_cast<QPixmap>(index.data(Qt::DecorationRole)).isNull();
        const int width = qMax(32, view_->viewport()->width());
        QTextLayout layout(index.data().toString().replace(QChar('\n'), QChar(0x2028)), styled.font);
        const qreal height = arrange(layout, qMax(16, width - 12 - (avatar ? 38 : 0)));
        return QSize(width, qMax(avatar ? 44 : 0, qCeil(height) + 12));
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const auto avatar = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
        styled.text.clear();
        styled.icon = {};
        view_->style()->drawControl(QStyle::CE_ItemViewItem, &styled, painter, view_);
        painter->save();
        painter->setClipRect(option.rect);
        int left = option.rect.left() + 6;
        if (!avatar.isNull()) {
            painter->drawPixmap(QRect(left, option.rect.top() + 6, 32, 32), avatar);
            left += 38;
        }
        painter->setPen(styled.palette.color(QPalette::Text));
        QTextLayout layout(index.data().toString().replace(QChar('\n'), QChar(0x2028)), styled.font);
        arrange(layout, qMax(16, option.rect.right() - left - 5));
        layout.draw(painter, QPointF(left, option.rect.top() + 6));
        painter->restore();
    }

  private:
    static qreal arrange(QTextLayout &layout, qreal width) {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        qreal y = 0;
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width);
            line.setPosition(QPointF(0, y));
            y += line.height();
        }
        layout.endLayout();
        return y;
    }
    QListView *view_;
};
class HistoryView final : public QListView {
  public:
    bool pauseOnHover = false, hovered = false;
    std::function<void()> resume;
    explicit HistoryView(QWidget *p = nullptr) : QListView(p) {
        setWordWrap(true);
        setUniformItemSizes(false);
        setSelectionMode(QAbstractItemView::NoSelection);
        setTextElideMode(Qt::ElideNone);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        setItemDelegate(new WrappedHistoryDelegate(this));
    }
    void follow() {
        if (!(pauseOnHover && hovered) && verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 60)
            scrollToBottom();
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QListView::resizeEvent(event);
        doItemsLayout();
        viewport()->update();
    }
    void enterEvent(QEnterEvent *e) override {
        hovered = true;
        QListView::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override {
        hovered = false;
        QListView::leaveEvent(e);
        if (resume)
            resume();
    }
};
struct FloatingWindows::Impl {
    FloatingWindows *owner;
    QPointer<RoomRealtime> service;
    QSettings settings;
    std::array<QPointer<QWidget>, 3> panels;
    std::array<QPointer<QDockWidget>, 3> docks;
    std::array<bool, 3> ownDocks{};
    QPointer<QMainWindow> host;
    bool ownHost = false, applyingFlags = false;
    QCheckBox *pins[3]{};
    QPushButton *modes[3]{}, *lockButton = nullptr;
    QPointer<Panel> giftWindow;
    QVBoxLayout *interactionLayout{};
    DisplayAssetCache assets;
    DisplayListModel messages{500}, gifts{500}, anti{100}, audience{100}, guard{5000};
    HistoryView *messageView{}, *giftView{}, *antiView{};
    QLabel *status[3]{}, *stats{};
    QComboBox *master{};
    QPushButton *secondary{}, *guardSecondary{}, *more{};
    QLabel *guardDescription{};
    QComboBox *guardMaster{};
    QList<RankChoice> choices;
    int secondaryIndex = 0;
    bool locked = false, split = false, stopped = false;
    QWidget *settingsPane{};
    QCheckBox *showGifts{}, *showAnti{}, *showMedal{}, *showIdentity{}, *showAvatars{}, *pause{},
        *hideMoney{}, *paidOnly{};
    QSpinBox *fontSize{}, *minimumGift{}, *opacity{};
    QJsonArray history;
    QSet<QString> ids;
    QTimer renderTimer;
    QQueue<qint64> eventSizes;
    qint64 historyBytes = 0;
    QString watched = QStringLiteral("—"), likes = QStringLiteral("—");
    void updateTools(int k) {
        if (!docks[k])
            return;
        const bool floating = docks[k]->isFloating();
        pins[k]->setEnabled(floating);
        modes[k]->setText(floating ? QStringLiteral("停靠到 OBS") : QStringLiteral("浮动窗口"));
        modes[k]->setEnabled(docks[k]->features().testFlag(QDockWidget::DockWidgetFloatable));
        if (k == 0 && lockButton) {
            const QSignalBlocker block(lockButton);
            lockButton->setEnabled(floating);
            lockButton->setChecked(locked && floating);
            lockButton->setText(locked ? QStringLiteral("已穿透 · 主面板可解锁")
                                       : QStringLiteral("锁定穿透"));
            if (opacity)
                opacity->setEnabled(floating);
        }
    }
    void applyFlags(int k) {
        auto *dock = docks[k].data();
        if (!dock || applyingFlags)
            return;
        applyingFlags = true;
        const bool floating = dock->isFloating();
        auto flags = dock->windowFlags();
        flags.setFlag(Qt::WindowStaysOnTopHint, floating && pins[k]->isChecked());
        flags.setFlag(Qt::WindowTransparentForInput, floating && k == 0 && locked);
        const bool visible = dock->isVisible();
        if (flags != dock->windowFlags()) {
            dock->setWindowFlags(flags);
            if (visible)
                dock->show();
        }
        const qreal wantedOpacity = floating && k == 0 && opacity ? opacity->value() / 100. : 1.;
        if (!qFuzzyCompare(dock->windowOpacity(), wantedOpacity))
            dock->setWindowOpacity(wantedOpacity);
        applyingFlags = false;
        updateTools(k);
    }
    void setLocked(bool wanted) {
        locked = wanted && docks[0] && docks[0]->isFloating();
        settings.setValue("interactionLocked", false);
        applyFlags(0); // Always clear native flags, even if our logical state was already false.
    }
    Impl(FloatingWindows *o, RoomRealtime *s, const QString &path, QWidget *p,
         const std::array<QDockWidget *, 3> &registeredDocks)
        : owner(o), service(s), settings(QDir(path).filePath("floating-windows.ini"), QSettings::IniFormat) {
        QDir().mkpath(path);
        host = qobject_cast<QMainWindow *>(p);
        if (!host) {
            host = new QMainWindow;
            ownHost = true;
        }
        for (auto m : {&messages, &gifts, &anti, &audience, &guard})
            m->setAssets(&assets);
        const QStringList titles = {QStringLiteral("直播互动"), QStringLiteral("房间观众"),
                                    QStringLiteral("大航海")};
        for (int k = 0; k < 3; ++k) {
            auto *dock = registeredDocks[k];
            if (!dock) {
                dock = new QDockWidget(titles[k], host);
                dock->setObjectName(QString::fromLatin1(FloatingWindows::dockId(k)));
                dock->setAllowedAreas(Qt::AllDockWidgetAreas);
                dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                                  QDockWidget::DockWidgetFloatable);
                dock->setWidget(new QWidget);
                host->addDockWidget(Qt::RightDockWidgetArea, dock);
                dock->setFloating(true);
                dock->hide();
                ownDocks[k] = true;
            }
            docks[k] = dock;
            auto *w = dock->widget();
            if (!w) {
                w = new QWidget;
                dock->setWidget(w);
            }
            panels[k] = w;
            w->setObjectName(QStringLiteral("livehimeFloating%1").arg(k));
            w->setMinimumSize(280, 240);
            auto *l = qobject_cast<QVBoxLayout *>(w->layout());
            if (!l)
                l = new QVBoxLayout(w);
            while (auto *item = l->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            auto bar = new QHBoxLayout;
            auto pin = pins[k] = new QCheckBox(QStringLiteral("置顶"), w);
            pin->setObjectName(QStringLiteral("window-pin-%1").arg(k));
            pin->setChecked(settings.value(QString("pin%1").arg(k), true).toBool());
            bar->addWidget(pin);
            modes[k] = new QPushButton(w);
            modes[k]->setObjectName(QStringLiteral("window-mode-%1").arg(k));
            bar->addWidget(modes[k]);
            bar->addStretch();
            auto close = new QPushButton(QStringLiteral("关闭"), w);
            bar->addWidget(close);
            l->addLayout(bar);
            QObject::connect(close, &QPushButton::clicked, dock, &QWidget::hide);
            QObject::connect(pin, &QCheckBox::toggled, owner, [this, k](bool b) {
                settings.setValue(QString("pin%1").arg(k), b);
                applyFlags(k);
            });
            QObject::connect(modes[k], &QPushButton::clicked, owner, [this, k] {
                if (!docks[k])
                    return;
                if (docks[k]->isFloating())
                    owner->dockWindow(k);
                else
                    owner->floatWindow(k);
            });
            status[k] = new QLabel(service->status(), w);
            status[k]->setWordWrap(true);
            l->addWidget(status[k]);
            QObject::connect(dock, &QDockWidget::visibilityChanged, owner, [this, k](bool v) {
                if (stopped || applyingFlags)
                    return;
                if (k == 0)
                    setLocked(false);
                if (service)
                    service->setVisible(k, v);
                if (docks[k] && docks[k]->isFloating())
                    settings.setValue(QString("geometry%1").arg(k), docks[k]->saveGeometry());
                if (v && k == 0 && messageView)
                    rebuild();
            });
            QObject::connect(dock, &QDockWidget::topLevelChanged, owner, [this, k](bool floating) {
                if (stopped || applyingFlags)
                    return;
                if (!floating && k == 0)
                    setLocked(false);
                applyFlags(k);
            });
            QObject::connect(dock, &QDockWidget::featuresChanged, owner, [this, k] { updateTools(k); });
            // OBS has already restored its native dock layout by the time the
            // views are attached. Only migrate the pre-dock floating geometry once.
            if (dock->isFloating() && ownDocks[k]) {
                dock->resize(k == 0 ? 420 : 370, 580);
                dock->restoreGeometry(settings.value(QString("geometry%1").arg(k)).toByteArray());
            }
            applyFlags(k);
        }
        settings.setValue("nativeDocks", true);
        settings.setValue("interactionLocked", false); // Restart always recovers mouse control.
        buildInteraction();
        buildAudience();
        buildGuard();
        renderTimer.setSingleShot(true);
        renderTimer.setInterval(50);
        QObject::connect(&renderTimer, &QTimer::timeout, owner, [this] { rebuild(); });
        QObject::connect(service.data(), &RoomRealtime::events, owner, [this](const QJsonArray &b) {
            if (stopped || !panels[0])
                return;
            for (auto v : b) {
                const qint64 n = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).size();
                if (n > 256 * 1024)
                    continue;
                history.append(v);
                eventSizes.enqueue(n);
                historyBytes += n;
                while (history.size() > 1100 || historyBytes > 8 * 1024 * 1024) {
                    history.removeFirst();
                    historyBytes -= eventSizes.dequeue();
                }
            }
            if ((panels[0]->isVisible() || (giftWindow && giftWindow->isVisible())) &&
                !renderTimer.isActive())
                renderTimer.start();
        });
        QObject::connect(service.data(), &RoomRealtime::contextChanged, owner, [this] {
            if (stopped)
                return;
            history = {};
            eventSizes.clear();
            historyBytes = 0;
            ids.clear();
            watched = likes = QStringLiteral("—");
            messages.clear();
            gifts.clear();
            anti.clear();
            if (panels[0])
                stats->setText(QStringLiteral("看过 —  ·  点赞 —"));
            rebuild();
            loadChoices();
            audience.clear();
            guard.clear();
        });
        QObject::connect(service.data(), &RoomRealtime::rankConfigChanged, owner,
                         [this] { loadChoices(true); });
        QObject::connect(service.data(), &RoomRealtime::statusChanged, owner, [this](const QString &s) {
            if (!stopped && panels[0])
                status[0]->setText(s);
        });
        QObject::connect(service.data(), &RoomRealtime::audienceChanged, owner,
                         [this] { renderRank(false); });
        QObject::connect(service.data(), &RoomRealtime::guardChanged, owner, [this] { renderRank(true); });
        if (service)
            for (int k = 0; k < 3; ++k)
                service->setVisible(k, docks[k] && docks[k]->isVisible() &&
                                           !docks[k]->visibleRegion().isEmpty());
    }
    ~Impl() {
        for (auto &dock : docks)
            if (dock)
                QObject::disconnect(dock, nullptr, owner, nullptr);
        if (giftWindow) {
            giftWindow->visibility = {};
            giftWindow->save = {};
        }
        delete giftWindow.data();
        for (int k = 0; k < 3; ++k) {
            if (ownDocks[k])
                delete docks[k].data();
            else
                delete panels[k].data();
        }
        if (ownHost)
            delete host.data();
    }
    HistoryView *view(DisplayListModel *m, QVBoxLayout *l) {
        auto v = new HistoryView;
        v->setModel(m);
        l->addWidget(v, 1);
        return v;
    }
    QCheckBox *check(const QString &key, const QString &label, bool def, QFormLayout *f) {
        auto c = new QCheckBox(label);
        c->setObjectName(key);
        c->setChecked(settings.value(key, def).toBool());
        f->addRow(c);
        QObject::connect(c, &QCheckBox::toggled, owner, [this, key](bool v) {
            settings.setValue(key, v);
            rebuild();
        });
        return c;
    }
    void buildInteraction() {
        auto *w = panels[0].data();
        auto l = interactionLayout = qobject_cast<QVBoxLayout *>(w->layout());
        auto tools = new QGridLayout;
        auto lock = lockButton = new QPushButton(QStringLiteral("锁定穿透"));
        lock->setObjectName("interaction-lock");
        lock->setCheckable(true);
        lock->setToolTip(
            QStringLiteral("仅浮动时可用；在 B站直播主面板点击“解除互动穿透”或重新打开直播互动即可恢复"));
        auto config = new QPushButton(QStringLiteral("显示设置"));
        auto detach = new QPushButton(QStringLiteral("拆分/合回礼物"));
        auto clear = new QPushButton(QStringLiteral("清屏"));
        int toolIndex = 0;
        for (auto b : {lock, config, detach, clear}) {
            tools->addWidget(b, toolIndex / 2, toolIndex % 2);
            ++toolIndex;
        }
        l->addLayout(tools);
        QObject::connect(lock, &QPushButton::clicked, owner, [this](bool checked) { setLocked(checked); });
        QObject::connect(detach, &QPushButton::clicked, owner, [this] {
            split = !split;
            giftView->setParent(nullptr);
            if (split) {
                if (!giftWindow) {
                    giftWindow = new Panel(docks[0]);
                    giftWindow->setWindowTitle(QStringLiteral("礼物流水"));
                    giftWindow->resize(380, 320);
                    new QVBoxLayout(giftWindow);
                    giftWindow->visibility = [this](bool v) {
                        if (!v && split && !stopped && panels[0])
                            showGifts->setChecked(false);
                    };
                }
                giftWindow->layout()->addWidget(giftView);
                showGifts->setChecked(true);
                giftWindow->show();
            } else {
                interactionLayout->insertWidget(interactionLayout->indexOf(messageView), giftView, 1);
                giftWindow->hide();
            }
            updateAppearance();
        });
        QObject::connect(clear, &QPushButton::clicked, owner, [this] {
            history = {};
            eventSizes.clear();
            historyBytes = 0;
            messages.clear();
            gifts.clear();
            anti.clear();
            rebuild();
        });
        stats = new QLabel;
        l->addWidget(stats);
        settingsPane = new QWidget;
        auto f = new QFormLayout(settingsPane);
        showAvatars = check("avatars", QStringLiteral("显示头像"), true, f);
        showGifts = check("gifts", QStringLiteral("礼物流水"), true, f);
        showAnti = check("anti", QStringLiteral("进房/关注/分享/点赞"), true, f);
        showMedal = check("medal", QStringLiteral("显示勋章文字"), true, f);
        showIdentity = check("identity", QStringLiteral("显示房管/航海身份"), true, f);
        pause = check("pause", QStringLiteral("悬停暂停滚动"), false, f);
        paidOnly = check("paidOnly", QStringLiteral("只显示付费礼物"), false, f);
        auto spin = [this, f](const QString &key, const QString &label, int low, int high, int def) {
            auto s = new QSpinBox;
            s->setRange(low, high);
            s->setValue(settings.value(key, def).toInt());
            f->addRow(label, s);
            QObject::connect(s, &QSpinBox::valueChanged, owner, [this, key](int v) {
                settings.setValue(key, v);
                rebuild();
            });
            return s;
        };
        fontSize = spin("font", QStringLiteral("字号"), 10, 32, 14);
        opacity = spin("opacity", QStringLiteral("窗口不透明度 %"), 30, 100, 90);
        l->addWidget(settingsPane);
        settingsPane->hide();
        QObject::connect(config, &QPushButton::clicked, settingsPane,
                         [this] { settingsPane->setVisible(!settingsPane->isVisible()); });
        giftView = view(&gifts, l);
        giftView->setObjectName("giftHistory");
        giftView->resume = [this] { renderTimer.start(); };
        messageView = view(&messages, l);
        messageView->setObjectName("messageHistory");
        messageView->resume = [this] { renderTimer.start(); };
        antiView = view(&anti, l);
        antiView->setObjectName("antiHistory");
        antiView->resume = [this] { renderTimer.start(); };
        updateAppearance();
    }
    void updateAppearance() {
        if (!messageView)
            return;
        for (auto m : {&messages, &gifts, &anti, &audience, &guard})
            m->setAvatarsVisible(showAvatars->isChecked());
        for (auto v : {messageView, giftView, antiView}) {
            auto f = v->font();
            f.setPointSize(fontSize->value());
            v->setFont(f);
            v->pauseOnHover = pause->isChecked();
        }
        applyFlags(0);
        giftView->setVisible(showGifts->isChecked());
        antiView->setVisible(showAnti->isChecked() && anti.rowCount() > 0);
        if (split && giftWindow)
            giftWindow->setVisible(showGifts->isChecked());
    }
    void buildAudience() {
        auto l = qobject_cast<QVBoxLayout *>(panels[1]->layout());
        master = new QComboBox;
        master->setObjectName("audienceMaster");
        secondary = new QPushButton;
        secondary->setObjectName("audienceSecondary");
        l->addWidget(master);
        l->addWidget(secondary);
        view(&audience, l);
        auto refresh = new QPushButton(QStringLiteral("刷新"));
        l->addWidget(refresh);
        QObject::connect(refresh, &QPushButton::clicked, service.data(), &RoomRealtime::refreshAudience);
        QObject::connect(master, &QComboBox::currentIndexChanged, owner, [this](int) {
            secondaryIndex = 0;
            selectAudience();
        });
        QObject::connect(secondary, &QPushButton::clicked, owner, [this] {
            if (master->currentIndex() < 0)
                return;
            auto &c = choices[master->currentIndex()];
            secondaryIndex = (secondaryIndex + 1) % c.switches.size();
            selectAudience();
        });
        loadChoices();
    }
    void loadChoices(bool preserve = false) {
        if (!service || !panels[1] || stopped)
            return;
        QString previousType = master->currentData().toString();
        QString previousSwitch;
        if (master->currentIndex() >= 0 && master->currentIndex() < choices.size())
            previousSwitch =
                choices[master->currentIndex()].switches.value(secondaryIndex).value("switch").toString();
        choices = audienceChoices(service ? service->rankInfo() : QJsonObject{});
        QSignalBlocker b(master);
        master->clear();
        int selected = 0;
        for (int i = 0; i < choices.size(); ++i) {
            auto &c = choices[i];
            master->addItem(c.title, c.type);
            master->setItemData(i, c.config, Qt::UserRole + 1);
            if (c.preferred && c.enabled)
                selected = i;
        }
        if (preserve)
            for (int i = 0; i < choices.size(); ++i)
                if (choices[i].type == previousType)
                    selected = i;
        master->setCurrentIndex(selected);
        secondaryIndex = 0;
        if (preserve)
            for (int i = 0; i < choices[selected].switches.size(); ++i)
                if (choices[selected].switches[i].value("switch") == previousSwitch)
                    secondaryIndex = i;
        selectAudience();
    }
    void selectAudience() {
        if (!service)
            return;
        int i = master->currentIndex();
        if (i < 0 || i >= choices.size())
            return;
        auto &c = choices[i];
        auto sw = c.switches.value(secondaryIndex);
        secondary->setText(sw.value("text").toString(sw.value("switch").toString()));
        secondary->setToolTip(sw.value("comment").toString());
        secondary->setEnabled(c.switches.size() > 1);
        audience.clear();
        service->selectAudience(c.type, sw.value("switch").toString());
    }
    void buildGuard() {
        auto l = qobject_cast<QVBoxLayout *>(panels[2]->layout());
        guardMaster = new QComboBox;
        guardMaster->setObjectName("guardMaster");
        guardMaster->addItems({QStringLiteral("周榜"), QStringLiteral("月榜"), QStringLiteral("陪伴榜")});
        guardSecondary = new QPushButton(QStringLiteral("亲密度"));
        guardSecondary->setObjectName("guardSecondary");
        guardDescription = new QLabel;
        guardDescription->setWordWrap(true);
        l->addWidget(guardMaster);
        l->addWidget(guardSecondary);
        l->addWidget(guardDescription);
        view(&guard, l);
        auto refresh = new QPushButton(QStringLiteral("刷新"));
        more = new QPushButton(QStringLiteral("加载更多"));
        l->addWidget(refresh);
        l->addWidget(more);
        QObject::connect(refresh, &QPushButton::clicked, service.data(), &RoomRealtime::refreshGuard);
        QObject::connect(more, &QPushButton::clicked, service.data(), &RoomRealtime::loadMoreGuard);
        QObject::connect(guardMaster, &QComboBox::currentIndexChanged, owner, [this](int) {
            guardSecondary->setText(QStringLiteral("亲密度"));
            selectGuard();
        });
        QObject::connect(guardSecondary, &QPushButton::clicked, owner, [this] {
            guardSecondary->setText(guardSecondary->text() == QStringLiteral("亲密度")
                                        ? QStringLiteral("陪伴天数")
                                        : QStringLiteral("亲密度"));
            selectGuard();
        });
        selectGuard();
    }
    void selectGuard() {
        if (!service)
            return;
        int i = guardMaster->currentIndex();
        guardSecondary->setVisible(i == 2);
        guardDescription->setVisible(i != 2);
        guardDescription->setText(i == 0 ? QStringLiteral("榜单每周更新，统计周大航海亲密度")
                                         : QStringLiteral("榜单每月更新，统计月大航海亲密度"));
        guard.clear();
        service->selectGuard(i == 0                                               ? 4
                             : i == 1                                             ? 3
                             : guardSecondary->text() == QStringLiteral("亲密度") ? 5
                                                                                  : 1);
    }
    void renderRank(bool g) {
        if (!service || !panels[g ? 2 : 1])
            return;
        const auto data = g ? service->guardData() : service->audienceData();
        auto rows = data.value(g ? "rows" : "item").toArray();
        if (g && rows.isEmpty())
            rows = data.value("list").toArray();
        auto &model = g ? guard : audience;
        model.clear();
        QJsonObject display;
        if (!g && master->currentIndex() >= 0 && master->currentIndex() < choices.size())
            display =
                choices[master->currentIndex()].switches.value(secondaryIndex).value("ui_type").toObject();
        const auto valueText =
            data.value("config").toObject().value("value_text").toString(QStringLiteral("榜单数值"));
        for (auto rv : rows) {
            const auto r = rv.toObject();
            auto u = userObject(r);
            auto score = r.value(g ? "accompany" : "score");
            QString rank = r.value("rank").toVariant().toString();
            if (!g && display.value("hide_rank_without_score").toInt() != 0 && score.toDouble() == 0)
                rank.clear();
            QString s = (rank.isEmpty() ? QString{} : rank + "  ") + privateDisplayName(r);
            if (!mystery(r)) {
                const int level =
                    r.value("guard_level").toInt(u.value("guard").toObject().value("level").toInt());
                if (level)
                    s += QStringLiteral("  [航海 %1]").arg(level);
                auto exp = u.value("guard").toObject().value("expired_str").toString();
                if (!exp.isEmpty())
                    s += "  " + exp;
            }
            if (g || !display.contains("show_score") || display.value("show_score").toInt() != 0) {
                auto metric = g ? (guardSecondary->text() == QStringLiteral("陪伴天数") &&
                                           guardMaster->currentIndex() == 2
                                       ? QStringLiteral("陪伴天数")
                                       : QStringLiteral("亲密度"))
                                : valueText;
                s += "\n" + metric + QStringLiteral("：") + score.toVariant().toString();
            }
            auto face = u.value("base").toObject().value("face").toString(r.value("face").toString());
            model.append(s, mystery(r) ? QString{} : face);
        }
        auto state = g ? service->guardStatus() : service->audienceStatus();
        if (state.isEmpty())
            state = service->status();
        status[g ? 2 : 1]->setText(
            state + (model.rowCount() > 0 ? QStringLiteral(" · %1 条").arg(model.rowCount()) : QString{}));
        if (g) {
            more->setEnabled(service->guardHasMore());
            more->setText(service->guardHasMore() ? QStringLiteral("加载更多")
                                                  : QStringLiteral("已经到底了"));
        }
    }
    void rebuild() {
        if (!panels[0] || !stats ||
            ((pause->isChecked() && messageView->hovered) || (pause->isChecked() && giftView->hovered) ||
             (pause->isChecked() && antiView->hovered)))
            return;
        const int oldScroll = messageView->verticalScrollBar()->value();
        const bool hold = (pause->isChecked() && messageView->hovered) ||
                          oldScroll < messageView->verticalScrollBar()->maximum() - 60;
        messages.clear();
        gifts.clear();
        anti.clear();
        QSet<QString> seen;
        for (auto v : history) {
            auto e = v.toObject();
            auto cmd = e.value("cmd").toString().section(':', 0, 0);
            auto data = e.value("data").toObject();
            auto id = data.value("tid").toString(data.value("id_str").toString());
            if (id.isEmpty() && cmd == "SUPER_CHAT_MESSAGE")
                id = data.value("id").toVariant().toString();
            if (!id.isEmpty()) {
                id = cmd + ":" + id;
                if (cmd == "SEND_GIFT")
                    id += ":" + data.value("giftId").toVariant().toString();
                if (seen.contains(id))
                    continue;
                seen.insert(id);
            }
            if (cmd == "WATCHED_CHANGE") {
                watched = data.value("text_large").toString(data.value("num").toVariant().toString());
                continue;
            }
            if (cmd == "LIKE_INFO_V3_UPDATE") {
                likes = data.value("click_count").toVariant().toString();
                continue;
            }
            if (cmd.startsWith("DANMU_MSG")) {
                auto info = e.value("info").toArray();
                if (info.size() < 3 || !av(info, 1).isString())
                    continue;
                auto meta = av(info, 0).toArray();
                auto legacy = av(info, 2).toArray();
                auto ext = av(meta, 15).toObject();
                QJsonObject row{{"user", ext.value("user")}, {"name", av(legacy, 1)}};
                QString label = privateDisplayName(row);
                if (!mystery(row)) {
                    if (showMedal->isChecked()) {
                        auto m = av(info, 3).toArray();
                        if (!av(m, 1).toString().isEmpty())
                            label += " [" + av(m, 1).toString() + " " + av(m, 0).toVariant().toString() + "]";
                    }
                    if (showIdentity->isChecked() && av(legacy, 2).toInt())
                        label += QStringLiteral(" [房管]");
                    auto level =
                        ext.value("user").toObject().value("guard").toObject().value("level").toInt();
                    if (showIdentity->isChecked() && level)
                        label += QStringLiteral(" [航海 %1]").arg(level);
                }
                QString body = av(info, 1).toString();
                auto extra = QJsonDocument::fromJson(ext.value("extra").toString().toUtf8()).object();
                if (extra.value("show_reply").toBool())
                    body = QStringLiteral("回复： ") + body;
                messages.append(
                    label + QStringLiteral("：") + body,
                    mystery(row)
                        ? QString{}
                        : ext.value("user").toObject().value("base").toObject().value("face").toString());
            } else if (cmd == "SEND_GIFT") {
                if (paidOnly->isChecked() && data.value("coin_type") != "gold")
                    continue;
                gifts.append(privateDisplayName(data) + QStringLiteral("  赠送 %1 × %2")
                                                            .arg(data.value("giftName").toString(),
                                                                 data.value("num").toVariant().toString()),
                             mystery(data) ? QString{} : data.value("face").toString());
            } else if (cmd == "SUPER_CHAT_MESSAGE") {
                QJsonObject row = data;
                row.insert("name", data.value("user_info").toObject().value("uname"));
                gifts.append(privateDisplayName(row) + QStringLiteral("  醒目留言：") +
                             data.value("message").toString());
            } else if (cmd == "USER_TOAST_MSG_V2") {
                int op = data.value("guard_info").toObject().value("op_type").toInt();
                if (!data.value("option").toObject().value("anchor_show").toBool() || op < 1 || op > 3)
                    continue;
                gifts.append(privateDisplayName(data) + QStringLiteral("  大航海：") +
                             data.value("guard_info").toObject().value("role_name").toString());
            } else if (cmd == "INTERACT_WORD" || cmd == "LIKE_INFO_V3_CLICK") {
                QString kind = QStringLiteral("互动");
                int t = data.value("msg_type").toInt();
                if (t == 1)
                    kind = QStringLiteral("进入直播间");
                if (t == 2)
                    kind = QStringLiteral("关注");
                if (t == 3)
                    kind = QStringLiteral("分享");
                if (cmd == "LIKE_INFO_V3_CLICK")
                    kind = QStringLiteral("点赞");
                anti.append(privateDisplayName(data) + "  " + kind);
            }
        }
        stats->setText(QStringLiteral("看过 %1  ·  点赞 %2").arg(watched, likes));
        updateAppearance();
        if (hold)
            messageView->verticalScrollBar()->setValue(oldScroll);
        else
            messageView->scrollToBottom();
        giftView->follow();
        antiView->follow();
    }
    void correct(QWidget *w) {
        bool visible = false;
        for (auto s : QGuiApplication::screens())
            if (s->availableGeometry().intersects(w->frameGeometry()) &&
                s->availableGeometry().intersected(w->frameGeometry()).width() >= 80 &&
                s->availableGeometry().intersected(w->frameGeometry()).height() >= 80)
                visible = true;
        if (!visible && QGuiApplication::primaryScreen()) {
            auto r = QGuiApplication::primaryScreen()->availableGeometry();
            w->resize(w->size().boundedTo(r.size()));
            w->move(r.topLeft() + QPoint(24, 24));
        }
    }
};
FloatingWindows::FloatingWindows(RoomRealtime *s, QString path, QWidget *p,
                                 std::array<QDockWidget *, 3> registeredDocks)
    : QObject(p), d(std::make_unique<Impl>(this, s, path, p, registeredDocks)) {}
FloatingWindows::~FloatingWindows() { shutdown(); }
const char *FloatingWindows::dockId(int kind) {
    static constexpr std::array<const char *, 3> ids{"livehime-obs-interaction", "livehime-obs-audience",
                                                     "livehime-obs-guard"};
    return kind >= 0 && kind < 3 ? ids[kind] : "";
}
QDockWidget *FloatingWindows::dockWidget(int kind) const {
    return kind >= 0 && kind < 3 ? d->docks[kind].data() : nullptr;
}
QWidget *FloatingWindows::panelWidget(int kind) const {
    return kind >= 0 && kind < 3 ? d->panels[kind].data() : nullptr;
}
bool FloatingWindows::interactionLocked() const {
    return d->locked && d->docks[0] && d->docks[0]->isFloating() &&
           d->docks[0]->windowFlags().testFlag(Qt::WindowTransparentForInput);
}
void FloatingWindows::showWindow(int kind) {
    if (kind < 0 || kind > 2 || d->stopped || !d->docks[kind])
        return;
    if (kind == 0)
        unlockInteraction();
    auto *w = d->docks[kind].data();
    if (w->isFloating())
        d->correct(w);
    w->show();
    if (kind == 0)
        d->rebuild();
    w->raise();
    w->activateWindow();
}
void FloatingWindows::unlockInteraction() { d->setLocked(false); }
void FloatingWindows::dockWindow(int kind) {
    if (kind < 0 || kind > 2 || d->stopped || !d->docks[kind])
        return;
    if (kind == 0)
        unlockInteraction();
    d->docks[kind]->setFloating(false);
    d->applyFlags(kind);
    if (d->ownHost && d->host)
        d->host->show();
    d->docks[kind]->show();
    d->docks[kind]->raise();
}
void FloatingWindows::floatWindow(int kind) {
    if (kind < 0 || kind > 2 || d->stopped || !d->docks[kind])
        return;
    if (kind == 0)
        unlockInteraction();
    d->docks[kind]->setFloating(true);
    d->applyFlags(kind);
    showWindow(kind);
}
void FloatingWindows::shutdown() {
    if (d->stopped)
        return;
    unlockInteraction();
    d->stopped = true;
    for (int k = 0; k < 3; ++k) {
        if (!d->docks[k])
            continue;
        if (d->docks[k]->isFloating())
            d->settings.setValue(QString("geometry%1").arg(k), d->docks[k]->saveGeometry());
        d->docks[k]->hide();
        if (d->service)
            d->service->setVisible(k, false);
    }
    if (d->giftWindow)
        d->giftWindow->hide();
    d->settings.sync();
}
} // namespace livehime
