#include "floating-windows.hpp"
#include "audience-rank.hpp"
#include "interaction-splitter.hpp"
#include "panel-palette.hpp"
#include "danmaku-input-row.hpp"
#include "connection-indicator.hpp"
#include "chat-user-menu.hpp"
#include "danmaku-follow.hpp"
#include "dock-focus.hpp"
#include "display-asset-cache.hpp"
#include "dock-scroll-area.hpp"
#include "room-realtime.hpp"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
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
#include <QTabBar>
#include <QTextCursor>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <functional>
#include <utility>

namespace livehime {
static constexpr qint64 HighlightDurationMs = 1800;
static qint64 monotonicMs() { return QDeadlineTimer::current(Qt::PreciseTimer).deadline(); }
static QJsonValue av(const QJsonArray &a, int i) { return i >= 0 && i < a.size() ? a.at(i) : QJsonValue{}; }
DisplayListModel::DisplayListModel(int limit, QObject *p) : QAbstractListModel(p), limit_(qMax(1, limit)) {}
int DisplayListModel::rowCount(const QModelIndex &p) const { return p.isValid() ? 0 : int(rows_.size()); }
QVariant DisplayListModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() < 0 || i.row() >= rows_.size())
        return {};
    const auto &row = rows_.at(i.row());
    if (role == Qt::DecorationRole && assets_ && showAvatars_ && !row.avatar.isEmpty())
        return QVariant::fromValue(assets_->get(row.avatar));
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return row.text;
    if (role == HighlightDeadlineRole && row.highlightDeadline)
        return row.highlightDeadline;
    if (role == SenderRole && row.sender.uid > 0)
        return QVariant::fromValue(row.sender);
    if (role == MessageSequenceRole && row.sequence)
        return row.sequence;
    if (role == ChatSpansRole && !row.spans.isEmpty())
        return QVariant::fromValue(row.spans);
    if (role == EmotesRole && !row.emotes.isEmpty())
        return QVariant::fromValue(row.emotes);
    if (role == EmoteImagesRole && !row.emotes.isEmpty())
        return emoteImages(i, false);
    return {};
}
QVariantMap DisplayListModel::emoteImages(const QModelIndex &i, bool requestMissing) const {
    QVariantMap images;
    if (!assets_ || !i.isValid() || i.model() != this || i.row() >= rows_.size())
        return images;
    for (const auto &emote : rows_.at(i.row()).emotes) {
        if (emote.url.isEmpty() || images.contains(emote.url))
            continue;
        const auto pixmap = requestMissing ? assets_->emote(emote.url) : assets_->cachedEmote(emote.url);
        if (!pixmap.isNull())
            images.insert(emote.url, pixmap);
    }
    return images;
}
void DisplayListModel::setAssets(DisplayAssetCache *c, bool showEmotes) {
    if (assets_)
        disconnect(assets_, nullptr, this, nullptr);
    assets_ = c;
    connect(c, &DisplayAssetCache::ready, this, [this](const QString &url) {
        if (!showAvatars_) return;
        for (int i = 0; i < rowCount(); ++i)
            if (rows_.at(i).avatar == url)
                emit dataChanged(index(i), index(i), {Qt::DecorationRole, Qt::SizeHintRole});
    });
    if (showEmotes) {
        connect(c, &DisplayAssetCache::emoteReady, this, [this](const QString &url) {
            for (int i = 0; i < rowCount(); ++i) {
                for (const auto &emote : rows_.at(i).emotes) {
                    if (emote.url == url) {
                        emit dataChanged(index(i), index(i), {EmoteImagesRole, Qt::SizeHintRole});
                        break;
                    }
                }
            }
        });
    }
}
void DisplayListModel::setAvatarsVisible(bool v) {
    if (showAvatars_ == v)
        return;
    showAvatars_ = v;
    if (rowCount())
        emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole, Qt::SizeHintRole});
}
void DisplayListModel::append(const QString &t, const QString &avatar, qint64 highlightDeadline,
                              QList<DanmakuEmote> emotes, DisplaySender sender, qint64 sequence,
                              QList<ChatSpan> spans) {
    if (rows_.size() >= limit_) {
        beginRemoveRows({}, 0, 0);
        rows_.removeFirst();
        endRemoveRows();
    }
    beginInsertRows({}, int(rows_.size()), int(rows_.size()));
    const auto text = t.left(8192);
    emotes.removeIf([&text](const DanmakuEmote &e) {
        return e.start < 0 || e.length <= 0 || e.start > text.size() - e.length;
    });
    rows_.append({text, avatar, highlightDeadline, std::move(emotes), std::move(sender), sequence, std::move(spans)});
    endInsertRows();
}
void DisplayListModel::replace(const QStringList &r) {
    beginResetModel();
    rows_.clear();
    for (qsizetype i = 0; i < qMin<qsizetype>(limit_, r.size()); ++i)
        rows_.append({r[i], {}, 0, {}, {}, 0, {}});
    endResetModel();
}
void DisplayListModel::clear() { replace({}); }
static constexpr std::array UserFields{"uinfo", "sender_uinfo", "user", "user_info"};
static QJsonObject userObject(const QJsonObject &r) {
    for (auto field : UserFields) {
        const auto user = r.value(QLatin1String(field)).toObject();
        if (!user.isEmpty()) return user;
    }
    return {};
}
static bool mystery(const QJsonObject &r) {
    auto masked = [](const QJsonObject &object) {
        const auto flag = object.value("is_mystery");
        return flag.toBool() || flag.toInt() != 0 || !object.value("anon").toObject().isEmpty();
    };
    if (masked(r)) return true;
    for (auto field : UserFields) {
        const auto user = r.value(QLatin1String(field)).toObject();
        if (!user.isEmpty() && (masked(user) || masked(user.value("base").toObject()))) return true;
    }
    return false;
}
QString privateDisplayName(const QJsonObject &r) {
    if (mystery(r))
        return QStringLiteral("神秘用户");
    auto name = userObject(r).value("base").toObject().value("name").toString();
    if (name.isEmpty())
        name = r.value("uname").toString(r.value("name").toString(r.value("username").toString()));
    return name.isEmpty() ? QStringLiteral("观众") : name.left(120);
}
static qint64 positiveId(const QJsonValue &value) {
    return qMax(qint64(0), value.isString() ? value.toString().toLongLong() : value.toInteger());
}
static DisplaySender displaySender(const QJsonObject &r, int nameOffset = 0) {
    if (mystery(r))
        return {};
    auto uid = positiveId(r.value("uid"));
    for (auto field : UserFields) {
        const auto nestedUid = positiveId(r.value(QLatin1String(field)).toObject().value("uid"));
        if (uid && nestedUid && uid != nestedUid) return {};
        if (nestedUid) uid = nestedUid;
    }
    return {uid, privateDisplayName(r), nameOffset, {}};
}
static qint64 gloryLevel(const QJsonObject &r, qint64 legacy = 0) {
    if (mystery(r))
        return {};
    auto wealth = userObject(r).value("wealth").toObject();
    if (wealth.isEmpty())
        wealth = r.value("wealth_info").toObject();
    return wealth.contains("level") ? positiveId(wealth.value("level"))
        : r.contains("wealth_level") ? positiveId(r.value("wealth_level")) : legacy;
}
static QString gloryBadge(const QJsonObject &r) {
    const auto level = gloryLevel(r);
    return level > 0 ? QStringLiteral(" [荣耀 %1]").arg(level) : QString{};
}
struct ChatLine {
    QString text;
    QList<ChatSpan> spans;
    DisplaySender sender;

    void add(ChatSpan::Kind kind, const QString &label, const QString &badge = {}, QColor color = {}) {
        spans.append({kind, int(text.size()), int(label.size()), badge, color});
        text += label;
    }
};
static ChatLine chatHeader(const QJsonObject &user, qint64 glory, QString medal, qint64 medalLevel,
                           qint64 medalColor, const QString &identity = {}) {
    ChatLine line;
    if (!mystery(user)) {
        if (glory > 0) {
            line.add(ChatSpan::Glory, QStringLiteral("[荣耀 %1]").arg(glory),
                     QStringLiteral("◆ %1").arg(glory));
            line.text += ' ';
        }
        if (!medal.isEmpty()) {
            const auto label = medal.left(64) + ' ' + QString::number(medalLevel);
            line.add(ChatSpan::Medal, '[' + label + ']', label,
                     medalColor > 0 && medalColor <= 0xffffff ? QColor::fromRgb(QRgb(medalColor)) : QColor{});
            line.text += ' ';
        }
        if (!identity.isEmpty())
            line.text += identity + ' ';
    }
    line.sender = displaySender(user, int(line.text.size()));
    line.add(ChatSpan::Nickname, privateDisplayName(user));
    return line;
}
static ChatLine activityLine(const QJsonObject &user, const QString &action, bool showMedal, const QString &time = {}) {
    const auto medal = showMedal ? user.value("fans_medal").toObject() : QJsonObject{};
    auto line = chatHeader(user, 0, medal.value("medal_name").toString(), positiveId(medal.value("medal_level")),
                           positiveId(medal.value("medal_color_start")));
    if (!mystery(user)) {
        auto &nickname = line.spans.last();
        nickname.kind = ChatSpan::EntryName;
        nickname.badgeColor = QColor::fromString(user.value("uname_color").toString());
    }
    line.text += ' ' + action;
    const auto tail = user.value("tail_text").toString().trimmed().left(64);
    if (!mystery(user) && !tail.isEmpty()) {
        line.text += ' ';
        line.add(ChatSpan::EntryLabel, tail, tail);
    }
    if (!time.isEmpty()) {
        const int offset = int(time.size()) + 1;
        line.text.prepend(time + ' ');
        for (auto &span : line.spans) span.start += offset;
        line.spans.prepend({ChatSpan::EntryTime, 0, int(time.size()), {}, {}});
        line.sender.nameOffset += offset;
    }
    return line;
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

class SettingsSpinBox final : public QSpinBox {
  public:
    SettingsSpinBox() { setFocusPolicy(Qt::StrongFocus); }

  protected:
    void wheelEvent(QWheelEvent *event) override {
        if (hasFocus())
            QSpinBox::wheelEvent(event);
        else
            event->ignore();
    }
};

class DanmakuInput final : public QLineEdit {
  protected:
    void inputMethodEvent(QInputMethodEvent *event) override {
        composing_ = !event->preeditString().isEmpty();
        QLineEdit::inputMethodEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (enter && event->isAutoRepeat()) {
            event->accept();
            return;
        }
        if (composing_ && enter) {
            QGuiApplication::inputMethod()->commit();
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

  private:
    bool composing_ = false;
};

class WrappedHistoryDelegate final : public QStyledItemDelegate {
  public:
    explicit WrappedHistoryDelegate(QListView *view) : QStyledItemDelegate(view), view_(view) {
        frameTimer_.setSingleShot(true);
        frameTimer_.setInterval(33);
        connect(&frameTimer_, &QTimer::timeout, this, [this] {
            view_->viewport()->update(dirtyRows_);
            dirtyRows_ = {};
        });
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        const bool avatar = !qvariant_cast<QPixmap>(index.data(Qt::DecorationRole)).isNull();
        const int width = qMax(32, option.rect.width() > 0 ? qMin(option.rect.width(), view_->viewport()->width())
                                                         : view_->viewport()->width());
        const qreal height = layoutMessage(index, styled, qMax(16, width - 12 - (avatar ? 38 : 0)), nullptr);
        return QSize(width, qMax(avatar ? 44 : 0, qCeil(height) + 12));
    }
    bool hitsSender(const QModelIndex &index, const DisplaySender &sender, QPoint point) const {
        const auto rect = view_->visualRect(index);
        point -= rect.topLeft() + QPoint(6, 6);
        const bool avatar = !index.data(Qt::DecorationRole).value<QPixmap>().isNull();
        if (avatar && QRect(0, 0, 32, 32).contains(point))
            return true;
        if (avatar)
            point.rx() -= 38;
        QStyleOptionViewItem style;
        style.initFrom(view_);
        style.font = view_->font();
        initStyleOption(&style, index);
        int character = -1;
        layoutMessage(index, style, qMax(16, rect.width() - 12 - (avatar ? 38 : 0)), nullptr, point, &character);
        return character >= sender.nameOffset && character < sender.nameOffset + sender.name.size();
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
        const int avatarWidth = avatar.isNull() ? 0 : 38;
        auto paintBackground = [&](qreal contentWidth) {
            if (index.data(DisplayListModel::ChatSpansRole).isValid()) {
                auto card = option.rect.adjusted(1, 2, -1, -2);
                card.setWidth(qMin(card.width(), qCeil(contentWidth) + 12 + avatarWidth));
                painter->setPen(Qt::NoPen);
                painter->setBrush(PanelPalette::current().card);
                painter->setRenderHint(QPainter::Antialiasing);
                painter->drawRoundedRect(card, 8, 8);
            }
            const auto deadline = index.data(DisplayListModel::HighlightDeadlineRole).toLongLong();
            if (deadline) {
                const auto remaining = deadline - monotonicMs();
                if (remaining > 0) {
                    auto tint = styled.palette.color(QPalette::Highlight);
                    const auto base = styled.palette.color(QPalette::Base);
                    tint = PanelPalette::readable(tint, base, styled.palette.color(QPalette::Text), 2.5);
                    const float strength = qMin(1.f, float(remaining) / float(HighlightDurationMs - 350));
                    tint.setAlphaF(0.57f * strength);
                    QLinearGradient wash(option.rect.topLeft(), option.rect.topRight());
                    wash.setColorAt(0, tint);
                    tint.setAlphaF(0.16f * strength);
                    wash.setColorAt(1, tint);
                    painter->fillRect(option.rect, wash);
                    tint.setAlphaF(0.94f * strength);
                    painter->fillRect(QRect(option.rect.topLeft(), QSize(3, option.rect.height())), tint);
                    // Repaint only fading rows. No history rebuild or scroll changes between frames.
                    dirtyRows_ += option.rect;
                    if (!frameTimer_.isActive())
                        frameTimer_.start();
                }
            }
            int left = option.rect.left() + 6;
            if (!avatar.isNull()) {
                painter->drawPixmap(QRect(left, option.rect.top() + 6, 32, 32), avatar);
                left += 38;
            }
            painter->translate(left, option.rect.top() + 6);
        };
        layoutMessage(index, styled, qMax(16, option.rect.width() - avatarWidth - 12), painter, {}, nullptr, paintBackground);
        painter->restore();
    }

  private:
    static qreal layoutMessage(const QModelIndex &index, const QStyleOptionViewItem &style, int width,
                               QPainter *painter, QPoint hitPoint = {}, int *character = nullptr,
                               const std::function<void(qreal)> &paintBackground = {}) {
        const auto text = index.data().toString();
        const auto emotes = index.data(DisplayListModel::EmotesRole).value<QList<DanmakuEmote>>();
        const auto spans = index.data(DisplayListModel::ChatSpansRole).value<QList<ChatSpan>>();
        if (emotes.isEmpty() && spans.isEmpty()) {
            QTextLayout layout(QString(text).replace(QChar('\n'), QChar(0x2028)), style.font);
            const auto height = arrange(layout, width);
            if (character) {
                for (int n = 0; n < layout.lineCount(); ++n) {
                    auto line = layout.lineAt(n);
                    if (line.naturalTextRect().contains(hitPoint)) {
                        *character = line.xToCursor(hitPoint.x(), QTextLine::CursorOnCharacter);
                        break;
                    }
                }
            }
            if (painter) {
                paintBackground(layout.boundingRect().width());
                painter->setPen(style.palette.color(QPalette::Text));
                layout.draw(painter, {});
            }
            return height;
        }

        // Measuring offscreen history must not re-download evicted images. A newly
        // visible row can request its preview again when the delegate paints it.
        const auto *liveRows = painter ? dynamic_cast<const DisplayListModel *>(index.model()) : nullptr;
        const auto images = liveRows ? liveRows->emoteImages(index, true)
                                     : index.data(DisplayListModel::EmoteImagesRole).toMap();
        // Build inline image objects only from decoded pixmaps. All chat and usernames
        // are inserted as literal text, and the document never resolves a remote URL.
        QTextDocument doc;
        doc.setUndoRedoEnabled(false);
        doc.setDocumentMargin(0);
        doc.setDefaultFont(style.font);
        QTextOption wrap;
        wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        doc.setDefaultTextOption(wrap);
        doc.setTextWidth(width);
        QTextCursor cursor(&doc);
        int pos = 0;
        const QTextCharFormat plain;
        const auto &colors = PanelPalette::current();
        QTextCharFormat placeholder;
        auto tint = style.palette.color(QPalette::Highlight);
        placeholder.setUnderlineStyle(QTextCharFormat::DotLine);
        placeholder.setUnderlineColor(tint);
        tint.setAlphaF(0.18f);
        placeholder.setBackground(tint);
        const int lineHeight = style.fontMetrics.height();
        // Badge images occupy one document character. Keep hit testing in the
        // original string's coordinates so only the nickname opens the user menu.
        QList<QPair<int, int>> compressedBadges;
        for (const auto &span : spans) {
            if (span.start < pos || span.start > text.size() - span.length)
                continue;
            cursor.insertText(text.mid(pos, span.start - pos), plain);
            const auto label = text.mid(span.start, span.length);
            if (!span.badgeText.isEmpty()) {
                auto font = style.font;
                font.setPixelSize(qMax(9, qRound(lineHeight * .62)));
                font.setWeight(QFont::DemiBold);
                const QFontMetrics metrics(font);
                const QSize size(qMin(width, metrics.horizontalAdvance(span.badgeText) + 10), metrics.height() + 2);
                const auto key = QStringLiteral("livehime-badge:/%1").arg(span.start);
                if (painter) {
                    QPixmap chip(qCeil(size.width() * painter->device()->devicePixelRatioF()),
                                 qCeil(size.height() * painter->device()->devicePixelRatioF()));
                    chip.setDevicePixelRatio(painter->device()->devicePixelRatioF());
                    chip.fill(Qt::transparent);
                    QPainter badge(&chip);
                    badge.setRenderHint(QPainter::Antialiasing);
                    const bool outlined = span.kind == ChatSpan::EntryLabel;
                    const auto fill = colors.badge(outlined ? colors.muted : span.badgeColor);
                    badge.setPen(outlined ? QPen(colors.divider, 1) : QPen(Qt::NoPen));
                    badge.setBrush(fill);
                    const qreal radius = outlined ? 2. : size.height() / 2.;
                    badge.drawRoundedRect(QRectF(QPointF(), size).adjusted(.5, .5, -.5, -.5), radius, radius);
                    badge.setFont(font);
                    badge.setPen(colors.badgeText(fill));
                    badge.drawText(QRect(QPoint(), size), Qt::AlignCenter, span.badgeText);
                    badge.end();
                    doc.addResource(QTextDocument::ImageResource, QUrl(key), chip);
                }
                QTextImageFormat format;
                format.setName(key);
                format.setWidth(size.width());
                format.setHeight(size.height());
                format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
                compressedBadges.append({cursor.position(), span.length - 1});
                cursor.insertImage(format);
            } else {
                QTextCharFormat format;
                if (span.kind == ChatSpan::EntryName) {
                    format.setFontWeight(QFont::DemiBold);
                    if (span.badgeColor.isValid()) {
                        format.setForeground(colors.readable(colors.mix(span.badgeColor, colors.text, .2),
                                                             colors.card, colors.text));
                    } else {
                        // One gradient across mixed Chinese/Latin runs and wrapped lines.
                        // Keep combining marks and emoji sequences in the same color.
                        QTextBoundaryFinder clusters(QTextBoundaryFinder::Grapheme, label);
                        qsizetype start = 0, end;
                        while ((end = clusters.toNextBoundary()) > start) {
                            const float at = float(start) / float(qMax<qsizetype>(1, label.size() - 1));
                            format.setForeground(colors.mix(colors.entryStart, colors.entryEnd, at));
                            cursor.insertText(label.sliced(start, end - start), format);
                            start = end;
                        }
                        pos = span.start + span.length;
                        continue;
                    }
                } else {
                    format.setForeground(span.kind == ChatSpan::GiftAction ? colors.giftAction :
                                         span.kind == ChatSpan::GiftName ? colors.giftName : colors.muted);
                    if (span.kind == ChatSpan::GiftName)
                        format.setFontWeight(QFont::DemiBold);
                }
                cursor.insertText(label, format);
            }
            pos = span.start + span.length;
        }
        for (const auto &emote : emotes) {
            if (emote.start < pos || emote.length <= 0 || emote.start > text.size() - emote.length)
                continue;
            cursor.insertText(text.mid(pos, emote.start - pos), plain);
            pos = emote.start + emote.length;
            const auto pixmap = images.value(emote.url).value<QPixmap>();
            if (pixmap.isNull()) {
                cursor.insertText(text.mid(emote.start, emote.length), placeholder);
                continue;
            }
            const auto key = QStringLiteral("livehime-emote:/%1").arg(pixmap.cacheKey());
            doc.addResource(QTextDocument::ImageResource, QUrl(key), pixmap);
            const int height = emote.standalone ? qBound(48, lineHeight * 3, 96) : lineHeight;
            const auto size = pixmap.size().scaled(qMin(width, emote.standalone ? 160 : 2 * height),
                                                   height, Qt::KeepAspectRatio);
            QTextImageFormat format;
            format.setName(key);
            format.setWidth(qMax(1, size.width()));
            format.setHeight(qMax(1, size.height()));
            format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
            cursor.insertImage(format);
        }
        cursor.insertText(text.mid(pos), plain);
        if (character) {
            const int hit = doc.documentLayout()->hitTest(hitPoint, Qt::ExactHit);
            *character = hit;
            for (const auto &[offset, skipped] : compressedBadges)
                if (hit > offset)
                    *character += skipped;
        }
        if (painter) {
            paintBackground(doc.idealWidth());
            QAbstractTextDocumentLayout::PaintContext context;
            context.palette = style.palette;
            if (!spans.isEmpty())
                context.palette.setColor(QPalette::Text, colors.text);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            doc.documentLayout()->draw(painter, context);
        }
        return doc.size().height();
    }

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
    mutable QTimer frameTimer_;
    mutable QRegion dirtyRows_;
};
class EntryHistoryModel final : public QAbstractListModel {
  public:
    EntryHistoryModel(qint64 existingThrough, QObject *parent)
        : QAbstractListModel(parent), existingThrough_(existingThrough) {}
    qint64 firstSequence() const { return rows_.isEmpty() ? 0 : rows_.constFirst().sequence; }
    void setEntries(const QList<RoomEntry> &entries, bool showMedal) {
        beginResetModel();
        rows_ = entries;
        lines_.clear();
        lines_.reserve(entries.size());
        for (const auto &entry : entries) {
            const QJsonObject actor{{"uname", entry.name}, {"uid", entry.uid}, {"fans_medal", entry.medal},
                                    {"uname_color", entry.nameColor}, {"tail_text", entry.tailText}, {"is_mystery", entry.anonymous}};
            const auto time = QDateTime::fromSecsSinceEpoch(entry.timestamp).toString(QStringLiteral("MM-dd HH:mm:ss"));
            lines_.append(activityLine(actor, QStringLiteral("进入直播间"), showMedal, time));
        }
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
            return {};
        const auto &entry = rows_.at(index.row());
        if (role == DisplayListModel::HighlightDeadlineRole && entry.sequence > existingThrough_)
            return entry.receivedAtMs + HighlightDurationMs;
        const auto &line = lines_.at(index.row());
        if (role == DisplayListModel::ChatSpansRole) return QVariant::fromValue(line.spans);
        if (role == DisplayListModel::SenderRole)
            return line.sender.uid > 0 ? QVariant::fromValue(line.sender) : QVariant{};
        if (role == Qt::ToolTipRole && entry.glory > 0)
            return line.text + QStringLiteral(" [荣耀 %1]").arg(entry.glory);
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole) return line.text;
        return {};
    }
  private:
    QList<RoomEntry> rows_;
    QList<ChatLine> lines_;
    qint64 existingThrough_;
};

class HistoryView final : public QListView {
  public:
    bool pauseOnHover = false, hovered = false;
    std::function<void()> resume;
    std::function<void()> hoverChanged;
    std::function<void(const DisplaySender &, QPoint)> userMenu;
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
    void refreshRowSizes() { scheduleDelayedItemsLayout(); }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override {
        const QPoint pos = viewport()->mapFromGlobal(event->globalPos());
        const auto index = indexAt(pos);
        const auto sender = index.data(DisplayListModel::SenderRole).value<DisplaySender>();
        if (userMenu && sender.uid > 0 && static_cast<WrappedHistoryDelegate *>(itemDelegate())->hitsSender(index, sender, pos)) {
            userMenu(sender, event->globalPos());
            event->accept();
        } else {
            QListView::contextMenuEvent(event);
        }
    }
    void resizeEvent(QResizeEvent *event) override {
        QListView::resizeEvent(event);
        doItemsLayout();
        viewport()->update();
    }
    void enterEvent(QEnterEvent *e) override {
        hovered = true;
        QListView::enterEvent(e);
        if (hoverChanged)
            hoverChanged();
    }
    void leaveEvent(QEvent *e) override {
        hovered = false;
        QListView::leaveEvent(e);
        if (hoverChanged)
            hoverChanged();
        if (resume)
            resume();
    }
};
struct FloatingWindows::Impl {
    FloatingWindows *owner;
    QPointer<RoomRealtime> service;
    QSettings settings;
    std::array<QPointer<QWidget>, DockCount> panels;
    std::array<QPointer<QDockWidget>, DockCount> docks;
    std::array<bool, DockCount> ownDocks{};
    QPointer<QMainWindow> host;
    bool ownHost = false, applyingFlags = false;
    QCheckBox *pins[DockCount]{};
    QPushButton *modes[DockCount]{}, *lockButton = nullptr;
    QPointer<QDialog> entryDialog;
    QPointer<QWidget> interactionBody, audienceBody;
    QPointer<ConnectionIndicator> indicators[DockCount];
    QPointer<DanmakuInputRow> danmakuRow;
    DisplayAssetCache assets{owner};
    DisplayListModel messages{500}, anti{100}, guard{5000};
    AudienceRankModel audience{assets};
    QPointer<HistoryView> messageView, antiView;
    InteractionSplitter *interactionSplit{};
    QPointer<DanmakuFollow> chatFollow;
    QLabel *status[DockCount]{}, *stats{};
    QTabBar *master{};
    QHBoxLayout *interactionTools{}, *audienceTools{};
    QLabel *audienceSummary{};
    QPushButton *secondary{}, *guardSecondary{}, *more{};
    QLabel *guardDescription{};
    QComboBox *guardMaster{};
    QList<RankChoice> choices;
    int secondaryIndex = 0;
    QPushButton *audienceLock = nullptr;
    bool audienceLocked = false, locked = false, stopped = false;
    QWidget *settingsPane{};
    QCheckBox *showGifts{}, *showAnti{}, *showMedal{}, *showIdentity{}, *showAvatars{}, *pause{}, *paidOnly{};
    QSpinBox *fontSize{}, *opacity{};
    struct ReceivedEvent {
        QJsonObject event;
        qint64 bytes, highlightDeadline;
        QList<DanmakuEmote> emotes;
        bool reply = false;
        qint64 sequence = 0;
    };
    QList<ReceivedEvent> history;
    QSet<QString> chatIds;
    QList<QString> recentChatIds;
    qint64 lastSequence = 0;
    QTimer renderTimer;
    QTimer themeTimer;
    qint64 historyBytes = 0;
    QString watched = QStringLiteral("—"), likes = QStringLiteral("—");
    void updateTools(int k) {
        if (!docks[k])
            return;
        const bool floating = docks[k]->isFloating();
        pins[k]->setEnabled(floating);
        const auto modeText = floating ? QStringLiteral("停靠到 OBS") : QStringLiteral("浮动窗口");
        if (k <= 1) {
            modes[k]->setIcon(modes[k]->style()->standardIcon(floating ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
            modes[k]->setToolTip(modeText);
            modes[k]->setAccessibleName(modeText);
        } else {
            modes[k]->setText(modeText);
        }
        modes[k]->setEnabled(docks[k]->features().testFlag(QDockWidget::DockWidgetFloatable));
        if (k == 1 && audienceLock) {
            const QSignalBlocker block(audienceLock);
            audienceLock->setEnabled(floating);
            audienceLock->setChecked(floating && audienceLocked);
        }
        if (k == 0 && lockButton) {
            const QSignalBlocker block(lockButton);
            lockButton->setEnabled(floating);
            lockButton->setChecked(locked && floating);
            lockButton->setText(locked ? QStringLiteral("已穿透") : QStringLiteral("穿透"));
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
        flags.setFlag(Qt::WindowTransparentForInput, floating && ((k == 0 && locked) || (k == 1 && audienceLocked)));
        const bool visible = dock->isVisible();
        if (flags != dock->windowFlags()) {
            dock->setWindowFlags(flags);
            if (visible)
                dock->show();
        }
        const qreal wantedOpacity = floating && k == 0 && opacity ? opacity->value() / 100. : 1.;
        if (!qFuzzyCompare(dock->windowOpacity(), wantedOpacity))
            dock->setWindowOpacity(wantedOpacity);
        if (k == 0 && danmakuRow)
            danmakuRow->setPassthrough(floating && locked);
        applyingFlags = false;
        updateTools(k);
    }
    void setLocked(bool wanted, int kind = 0) {
        if (kind == 1) {
            audienceLocked = wanted && docks[1] && docks[1]->isFloating();
            applyFlags(1);
            return;
        }
        locked = wanted && docks[0] && docks[0]->isFloating();
        settings.setValue("interactionLocked", false);
        applyFlags(0); // Always clear native flags, even if our logical state was already false.
    }
    Impl(FloatingWindows *o, RoomRealtime *s, const QString &path, QWidget *p,
         const std::array<QDockWidget *, DockCount> &registeredDocks)
        : owner(o), service(s), settings(QDir(path).filePath("floating-windows.ini"), QSettings::IniFormat) {
        QDir().mkpath(path);
        host = qobject_cast<QMainWindow *>(p);
        if (!host) {
            host = new QMainWindow;
            ownHost = true;
        }
        for (auto m : {&messages, &anti, &guard})
            m->setAssets(&assets, m == &messages);
        std::array<QVBoxLayout *, DockCount> contentLayouts;
        for (int k = 0; k < DockCount; ++k) {
            auto *dock = registeredDocks[k];
            if (!dock) {
                dock = new QDockWidget(FloatingWindows::dockTitle(k), host);
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
            installDockFocusPolicy(dock, host);
            w->setObjectName(QStringLiteral("livehimeFloating%1").arg(k));
            w->setMinimumWidth(280);
            auto *shell = qobject_cast<QVBoxLayout *>(w->layout());
            if (!shell)
                shell = new QVBoxLayout(w);
            while (auto *item = shell->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            auto *l = addScrollableDockContent(shell);
            contentLayouts[k] = l;
            auto *content = l->parentWidget();
            auto bar = new QHBoxLayout;
            if (k <= 1) {
                if (k == 0)
                    interactionTools = bar;
                else
                    audienceTools = bar;
                auto *title = new QLabel(FloatingWindows::dockTitle(k), content);
                title->setObjectName(k == 0 ? "interactionTitle" : "audienceTitle");
                bar->addWidget(title);
                bar->addStretch(1);
            }
            auto pin = pins[k] = new QCheckBox(QStringLiteral("置顶"), content);
            pin->setObjectName(QStringLiteral("window-pin-%1").arg(k));
            pin->setChecked(settings.value(QString("pin%1").arg(k), true).toBool());
            bar->addWidget(pin);
            modes[k] = new QPushButton(content);
            modes[k]->setObjectName(QStringLiteral("window-mode-%1").arg(k));
            bar->addWidget(modes[k]);
            if (k > 1)
                bar->addStretch();
            auto close = new QPushButton(QStringLiteral("关闭"), content);
            if (k <= 1) {
                close->setText({});
                close->setAccessibleName(QStringLiteral("关闭") + FloatingWindows::dockTitle(k));
                close->setToolTip(close->accessibleName());
                close->setIcon(close->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
            }
            bar->addWidget(close);
            if (k == 0) {
                pin->setProperty("chatTool", true);
                modes[k]->setProperty("chatTool", true);
                close->setProperty("chatTool", true);
            }
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
            auto *indicator = new ConnectionIndicator(service, w);
            indicators[k] = indicator;
            shell->insertWidget(0, indicator);
            if (k == 0) {
                status[k] = indicator;
            } else {
                auto alarmOnly = [indicator](const RealtimeConnection &s) {
                    indicator->setVisible(s.phase == RealtimeConnection::Phase::Reconnecting);
                };
                alarmOnly(service->connection());
                QObject::connect(service.data(), &RoomRealtime::connectionChanged, indicator, alarmOnly);
                status[k] = new QLabel(service->status(), content);
                status[k]->setWordWrap(true);
                l->addWidget(status[k]);
            }
            QObject::connect(dock, &QDockWidget::visibilityChanged, owner, [this, k](bool v) {
                if (stopped || applyingFlags)
                    return;
                if (k <= 1)
                    setLocked(false, k);
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
                if (!floating && k <= 1)
                    setLocked(false, k);
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
        buildInteraction(contentLayouts[0]);
        buildAudience(contentLayouts[1]);
        buildGuard(contentLayouts[2]);
        themeTimer.setSingleShot(true);
        QObject::connect(&themeTimer, &QTimer::timeout, owner, [this] { applyTheme(); });
        applyTheme();
        renderTimer.setSingleShot(true);
        renderTimer.setInterval(50);
        QObject::connect(&renderTimer, &QTimer::timeout, owner, [this] { rebuild(); });
        QObject::connect(&assets, &DisplayAssetCache::giftCatalogReady, owner, [this] { renderTimer.start(); });
        QObject::connect(service.data(), &RoomRealtime::events, owner, [this](const QJsonArray &b) {
            if (stopped || !panels[0])
                return;
            const auto deadline = panels[0]->isVisible() ? monotonicMs() + HighlightDurationMs : 0;
            int chatCount = 0;
            for (auto v : b) {
                const auto event = v.toObject();
                const qint64 n = QJsonDocument(event).toJson(QJsonDocument::Compact).size();
                if (n > 256 * 1024)
                    continue;
                ReceivedEvent received{event, n, deadline, {}};
                if (event.value("cmd").toString().startsWith("DANMU_MSG")) {
                    const auto info = event.value("info").toArray();
                    if (info.size() < 3 || !av(info, 1).isString())
                        continue;
                    const auto meta = av(info, 0).toArray();
                    const auto rawExtra = av(meta, 15).toObject().value("extra");
                    const auto extra = rawExtra.isObject() ? rawExtra.toObject()
                        : QJsonDocument::fromJson(rawExtra.toString().toUtf8()).object();
                    const auto data = event.value("data").toObject();
                    QString chatId;
                    for (const auto &id : {extra.value("id_str"), event.value("msg_id"), data.value("id_str"), data.value("tid")}) {
                        const auto key = id.isString() ? id.toString() :
                            (id.toInteger() > 0 ? QString::number(id.toInteger()) : QString{});
                        if (!key.isEmpty() && key != QStringLiteral("0") && key.size() <= 128) {
                            chatId = key;
                            break;
                        }
                    }
                    if (!chatId.isEmpty()) {
                        if (chatIds.contains(chatId))
                            continue;
                        chatIds.insert(chatId);
                        recentChatIds.append(chatId);
                        if (recentChatIds.size() > 1100)
                            chatIds.remove(recentChatIds.takeFirst());
                    }
                    ++chatCount;
                    received.reply = extra.value("show_reply").toBool();
                    received.emotes = danmakuEmotes(av(info, 1).toString(), meta, extra);
                    for (const auto &emote : std::as_const(received.emotes)) {
                        if (!emote.url.isEmpty())
                            assets.emote(emote.url);
                    }
                }
                received.sequence = ++lastSequence;
                history.append(std::move(received));
                historyBytes += n;
                while (history.size() > 1100 || historyBytes > 8 * 1024 * 1024) {
                    const auto oldest = history.takeFirst();
                    historyBytes -= oldest.bytes;
                }
            }
            if (chatFollow && chatCount)
                chatFollow->messagesArrived(chatCount);
            if (panels[0]->isVisible() && !renderTimer.isActive())
                renderTimer.start();
        });
        QObject::connect(service.data(), &RoomRealtime::contextChanged, owner, [this] {
            if (stopped)
                return;
            history = {};
            chatIds.clear();
            recentChatIds.clear();
            if (chatFollow) chatFollow->reset();
            historyBytes = 0;
            watched = likes = QStringLiteral("—");
            messages.clear();
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
        for (int k = 0; k < DockCount; ++k) {
            if (ownDocks[k])
                delete docks[k].data();
            else
                delete panels[k].data();
        }
        if (ownHost)
            delete host.data();
    }
    HistoryView *view(DisplayListModel *m, QVBoxLayout *l = nullptr) {
        auto v = new HistoryView;
        enableUserMenu(v);
        v->setModel(m);
        // QListView can retain an item's previous height after an async image arrives.
        // Ask the view to measure the row again with the now-cached image.
        QObject::connect(m, &QAbstractItemModel::dataChanged, v, [v](const QModelIndex &,
                                 const QModelIndex &, const QList<int> &roles) {
            if (roles.contains(Qt::SizeHintRole))
                v->refreshRowSizes();
        });
        if (l)
            l->addWidget(v, 1);
        return v;
    }
    void enableUserMenu(HistoryView *v) {
        v->userMenu = [this, v](const DisplaySender &sender, QPoint globalPos) {
            showChatUserMenu(v, service, sender.uid, sender.name, sender.message, globalPos);
        };
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
    void showEntryHistory() {
        if (!service || stopped) return;
        if (!entryDialog) {
            auto *dialog = new QDialog(panels[0]);
            entryDialog = dialog;
            dialog->setObjectName("entry-history-dialog");
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(QStringLiteral("入场历史"));
            dialog->resize(360, 400);
            auto *layout = new QVBoxLayout(dialog);
            auto *summary = new QLabel(dialog);
            layout->addWidget(summary);
            auto *view = new HistoryView(dialog);
            view->setFont(messageView->font());
            view->setFrameShape(QFrame::NoFrame);
            enableUserMenu(view);
            view->setObjectName("entry-history-list");
            view->setStyleSheet(historyStyle());
            view->setAccessibleName(QStringLiteral("入场时间和观众"));
            const auto &entries = service->entryHistory();
            auto *model = new EntryHistoryModel(entries.isEmpty() ? 0 : entries.constLast().sequence, view);
            view->setModel(model);
            layout->addWidget(view, 1);
            auto refresh = [room = service, medals = showMedal, summary, view, model] {
                if (!room)
                    return;
                auto *scroll = view->verticalScrollBar();
                const auto top = view->indexAt(QPoint(1, 0));
                const bool follow = scroll->value() == scroll->maximum() || !top.isValid();
                const auto firstVisible = model->firstSequence() + top.row();
                const int topOffset = follow ? 0 : view->visualRect(top).top();
                model->setEntries(room->entryHistory(), medals->isChecked());
                summary->setText(model->rowCount() ? QStringLiteral("本次连接 · 最近 %1 条入场记录").arg(model->rowCount())
                                                   : QStringLiteral("本次连接尚未收到入场记录"));
                if (follow) {
                    view->scrollToBottom();
                } else if (firstVisible < model->firstSequence()) {
                    view->scrollToTop();
                } else {
                    const auto row = static_cast<int>(firstVisible - model->firstSequence());
                    view->scrollTo(model->index(row, 0), QAbstractItemView::PositionAtTop);
                    scroll->setValue(scroll->value() - topOffset);
                }
            };
            refresh();
            QObject::connect(showMedal, &QCheckBox::toggled, dialog, refresh);
            QTimer::singleShot(0, view, &QListView::scrollToBottom);
            auto *timer = new QTimer(dialog);
            timer->setSingleShot(true);
            timer->setInterval(200);
            QObject::connect(timer, &QTimer::timeout, dialog, refresh);
            QObject::connect(service.data(), &RoomRealtime::entryHistoryChanged, dialog,
                             [timer] { if (!timer->isActive()) timer->start(); });
            QObject::connect(service.data(), &RoomRealtime::contextChanged, dialog, &QDialog::reject);
            QObject::connect(service.data(), &RoomRealtime::stopped, dialog, &QDialog::reject);
        }
        entryDialog->show();
        entryDialog->raise();
        entryDialog->activateWindow();
    }
    static QString historyStyle() {
        const auto &c = PanelPalette::current();
        return QStringLiteral("QListView#entry-history-list { background: %1; color: %2; border: none;"
                              " font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI'; }")
            .arg(c.surface.name(), c.text.name());
    }
    void applyTheme() {
        const auto &c = PanelPalette::current();
        if (interactionBody) interactionBody->setStyleSheet(QStringLiteral(
            "#interactionBody, #interactionBody QWidget { font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI'; }"
            "#interactionBody, QListView#messageHistory, QListView#antiHistory { background: %1; }"
            "QListView#messageHistory, QListView#antiHistory { color: %2; border: none; padding: 0; }"
            "#interactionBody QLabel, #interactionBody QCheckBox { color: %3; }"
            "#interactionBody #interactionTitle { color: %2; font-weight: bold; }"
            "#interactionBody *[chatTool=true] { background: transparent; color: %3;"
            " border: none; padding: 1px 3px; min-width: 0; min-height: 0; }"
            "#interactionBody *[chatTool=true]:hover { background: %4; }"
            "#interactionBody QPushButton[chatTool=true]:checked { color: %5; }")
            .arg(c.surface.name(), c.text.name(), c.muted.name(), c.hover.name(), c.accent.name()));
        if (audienceBody) audienceBody->setStyleSheet(QStringLiteral(
            "#audienceBody, #audienceBody QWidget { font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI'; }"
            "#audienceBody, QListView#audienceRanking { background: %1; color: %2; border: none; }"
            "#audienceTitle { color: %2; font-weight: bold; }"
            "#audienceBody QPushButton, #audienceBody QCheckBox { background: transparent; color: %3;"
            " border: none; padding: 1px 3px; min-width: 0; min-height: 0; }"
            "#audienceBody QPushButton:hover, #audienceBody QCheckBox:hover { background: %4; }"
            "#audienceBody QPushButton:checked { color: %5; }"
            "#audienceMaster::tab { color: %3; background: transparent; border: none;"
            " border-bottom: 2px solid transparent; padding: 4px 3px; min-width: 0; margin: 0; }"
            "#audienceMaster::tab:selected { color: %5; border-bottom-color: %5; }"
            "#audienceSummary { color: %3; border-top: 1px solid %6; padding: 4px 3px; }"
            "#audienceState { color: %3; padding: 2px 4px; }")
            .arg(c.surface.name(), c.text.name(), c.muted.name(), c.hover.name(), c.accent.name(), c.divider.name()));
        if (entryDialog)
            if (auto *list = entryDialog->findChild<QListView *>("entry-history-list"))
                list->setStyleSheet(historyStyle());
        for (const auto &indicator : indicators)
            if (indicator) indicator->refreshColors();
    }
    void buildInteraction(QVBoxLayout *l) {
        interactionBody = l->parentWidget();
        interactionBody->setObjectName("interactionBody");
        interactionBody->setAttribute(Qt::WA_StyledBackground);
        l->setContentsMargins(6, 3, 6, 4);
        l->setSpacing(3);
        interactionTools->setSpacing(2);
        auto lock = lockButton = new QPushButton(QStringLiteral("锁定穿透"));
        lock->setObjectName("interaction-lock");
        lock->setAccessibleName(QStringLiteral("鼠标穿透"));
        lock->setCheckable(true);
        lock->setToolTip(
            QStringLiteral("浮窗穿透时仍可输入和发送弹幕。点击主面板的“解除互动穿透”或重新打开直播互动可解锁。"));
        auto config = new QPushButton(QStringLiteral("设置"));
        config->setAccessibleName(QStringLiteral("显示设置"));
        config->setToolTip(config->accessibleName());
        config->setObjectName("interaction-settings");
        auto clear = new QPushButton(QStringLiteral("清屏"));
        for (auto *button : {lock, config, clear}) {
            button->setProperty("chatTool", true);
            interactionTools->insertWidget(interactionTools->count() - 1, button);
        }
        QObject::connect(lock, &QPushButton::clicked, owner, [this](bool checked) { setLocked(checked); });
        QObject::connect(clear, &QPushButton::clicked, owner, [this] {
            history = {};
            chatIds.clear();
            recentChatIds.clear();
            if (chatFollow) chatFollow->reset();
            historyBytes = 0;
            messages.clear();
            anti.clear();
            rebuild();
        });
        auto *summaryRow = new QHBoxLayout;
        stats = new QLabel;
        stats->setWordWrap(true);
        summaryRow->addWidget(stats, 1);
        auto *entries = new QPushButton(QStringLiteral("入场历史"));
        entries->setObjectName("show-entry-history");
        entries->setProperty("chatTool", true);
        summaryRow->addWidget(entries);
        l->addLayout(summaryRow);
        QObject::connect(entries, &QPushButton::clicked, owner, [this] { showEntryHistory(); });
        settingsPane = new QWidget;
        auto f = new QFormLayout(settingsPane);
        showAvatars = check("avatars", QStringLiteral("显示头像"), true, f);
        // Closing the old detached ledger also wrote gifts=false; that was a dock visibility preference.
        const bool inlineDefault = settings.value("giftSplit", false).toBool() || settings.value("gifts", true).toBool();
        showGifts = check("showInlineGifts", QStringLiteral("在弹幕中显示礼物"), inlineDefault, f);
        showAnti = check("anti", QStringLiteral("进房/关注/分享/点赞"), true, f);
        showMedal = check("medal", QStringLiteral("显示勋章文字"), true, f);
        showIdentity = check("identity", QStringLiteral("显示房管/航海身份"), true, f);
        pause = check("pause", QStringLiteral("悬停暂停滚动"), false, f);
        paidOnly = check("paidOnly", QStringLiteral("只显示付费礼物"), false, f);
        auto spin = [this, f](const QString &key, const QString &label, int low, int high, int def) {
            auto *s = new SettingsSpinBox;
            s->setRange(low, high);
            s->setValue(settings.value(key, def).toInt());
            f->addRow(label, s);
            QObject::connect(s, &QSpinBox::valueChanged, owner, [this, key](int v) {
                settings.setValue(key, v);
                rebuild();
            });
            return s;
        };
        fontSize = spin("font", QStringLiteral("字号"), 10, 32, 11);
        opacity = spin("opacity", QStringLiteral("窗口不透明度 %"), 30, 100, 90);
        l->addWidget(settingsPane);
        settingsPane->hide();
        QObject::connect(config, &QPushButton::clicked, settingsPane,
                         [this] { settingsPane->setVisible(!settingsPane->isVisible()); });
        interactionSplit = new InteractionSplitter(l->parentWidget());
        l->addWidget(interactionSplit, 1);
        messageView = view(&messages);
        antiView = view(&anti);
        interactionSplit->addWidget(messageView);
        interactionSplit->addWidget(antiView);
        messageView->setObjectName("messageHistory");
        for (auto *list : {messageView.data(), antiView.data()}) {
            list->setFrameShape(QFrame::NoFrame);
            list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        }
        chatFollow = new DanmakuFollow(messageView, DisplayListModel::MessageSequenceRole);
        QObject::connect(chatFollow, &DanmakuFollow::latestRequested, owner, [this] { rebuild(true); });
        auto updateHover = [this] {
            if (stopped || !panels[0] || !chatFollow)
                return;
            auto hovered = [](HistoryView *v) { return v && v->isVisible() && v->hovered; };
            chatFollow->setPaused(pause->isChecked() && (hovered(messageView) || hovered(antiView)));
        };
        messageView->hoverChanged = updateHover;
        QObject::connect(pause, &QCheckBox::toggled, owner, updateHover);
        messageView->resume = [this] { renderTimer.start(); };
        buildDanmakuInput(l);
        antiView->hoverChanged = updateHover;
        antiView->setObjectName("antiHistory");
        antiView->resume = [this] { renderTimer.start(); };
        antiView->setMinimumHeight(42);
        updateAppearance();
        const auto saved = settings.value("interactionSplitter").toByteArray();
        if (saved.isEmpty() || !interactionSplit->restoreState(saved))
            interactionSplit->setSizes({700, 300});
        QObject::connect(interactionSplit, &QSplitter::splitterMoved, owner, [this] {
            settings.setValue("interactionSplitter", interactionSplit->saveState());
        });
    }
    void buildDanmakuInput(QVBoxLayout *layout) {
        auto *input = new DanmakuInput;
        input->setObjectName("danmakuInput");
        input->setAccessibleName(QStringLiteral("发送弹幕"));
        auto *button = new QPushButton(QStringLiteral("发送"));
        button->setObjectName("danmakuSend");
        danmakuRow = new DanmakuInputRow(docks[0], input, button, layout->parentWidget());
        layout->addWidget(danmakuRow);
        auto *feedback = new QLabel;
        feedback->setObjectName("danmakuSendStatus");
        feedback->setTextFormat(Qt::PlainText);
        feedback->setWordWrap(true);
        layout->addWidget(feedback);
        auto refresh = [this, input, button] {
            const bool ready = service && service->canSendDanmaku();
            const bool busy = ready && service->sendingDanmaku();
            input->setEnabled(ready);
            input->setReadOnly(busy);
            input->setPlaceholderText(ready ? QStringLiteral("输入弹幕") : QStringLiteral("登录并加载房间后可发弹幕"));
            button->setEnabled(ready && !busy && !input->text().trimmed().isEmpty());
        };
        auto reset = [input, feedback, refresh] {
            input->clear();
            feedback->clear();
            feedback->hide();
            refresh();
        };
        auto submit = [this, input, button, feedback, refresh] {
            if (!service || !button->isEnabled()) return;
            feedback->setText(QStringLiteral("发送中…"));
            feedback->show();
            service->sendDanmaku(input->text());
            refresh();
        };
        QObject::connect(input, &QLineEdit::textChanged, input, refresh);
        QObject::connect(input, &QLineEdit::returnPressed, input, submit);
        QObject::connect(button, &QPushButton::clicked, input, submit);
        QObject::connect(service.data(), &RoomRealtime::danmakuSendFinished, input,
                         [input, feedback, refresh](bool accepted, const QString &message) {
            if (accepted) input->clear();
            feedback->setText(message);
            feedback->show();
            refresh();
        });
        QObject::connect(service.data(), &RoomRealtime::contextChanged, input, reset);
        QObject::connect(service.data(), &RoomRealtime::stopped, input, reset);
        QObject::connect(service.data(), &QObject::destroyed, input, reset);
        reset();
    }
    void updateAppearance() {
        if (!messageView)
            return;
        audience.setAvatarsVisible(showAvatars->isChecked());
        for (auto m : {&messages, &anti, &guard})
            m->setAvatarsVisible(showAvatars->isChecked());
        for (auto v : {messageView.data(), antiView.data()}) {
            if (!v)
                continue;
            auto f = v->font();
            f.setPointSize(fontSize->value());
            v->setFont(f);
            v->pauseOnHover = pause->isChecked();
        }
        applyFlags(0);
        // Leave room for full-size stickers by default while allowing the reader
        // to give most of the space to arrivals with a deliberate drag.
        const int line = messageView->fontMetrics().height();
        const int stickerRow = qBound(48, line * 3, 96) + line * 2 + 14;
        messageView->setMinimumHeight(line + 12);
        interactionSplit->setMinimumHeight(stickerRow + (showAnti->isChecked() ? antiView->minimumHeight() + interactionSplit->handleWidth() : 0));
        antiView->setVisible(showAnti->isChecked());
    }
    void buildAudience(QVBoxLayout *l) {
        audienceBody = l->parentWidget();
        audienceBody->setObjectName("audienceBody");
        audienceBody->setAttribute(Qt::WA_StyledBackground);
        l->setContentsMargins(6, 3, 6, 0);
        l->setSpacing(2);
        audienceTools->setSpacing(2);
        audienceLock = new QPushButton(QStringLiteral("穿透"));
        audienceLock->setObjectName("audience-lock");
        audienceLock->setCheckable(true);
        audienceLock->setAccessibleName(QStringLiteral("鼠标穿透"));
        audienceLock->setToolTip(QStringLiteral("重新打开“房间观众”即可解除穿透"));
        audienceTools->insertWidget(audienceTools->count() - 1, audienceLock);
        QObject::connect(audienceLock, &QPushButton::clicked, owner, [this](bool checked) { setLocked(checked, 1); });
        updateTools(1);
        auto *refresh = new QPushButton;
        refresh->setIcon(refresh->style()->standardIcon(QStyle::SP_BrowserReload));
        refresh->setToolTip(QStringLiteral("刷新榜单"));
        refresh->setAccessibleName(refresh->toolTip());
        audienceTools->insertWidget(audienceTools->count() - 1, refresh);
        QObject::connect(refresh, &QPushButton::clicked, service.data(), &RoomRealtime::refreshAudience);
        master = new QTabBar;
        master->setObjectName("audienceMaster");
        master->setAccessibleName(QStringLiteral("观众榜单"));
        master->setExpanding(false);
        master->setDrawBase(false);
        master->setDocumentMode(true);
        master->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        secondary = new QPushButton;
        secondary->setObjectName("audienceSecondary");
        auto *tabs = new QHBoxLayout;
        tabs->setSpacing(2);
        tabs->addWidget(master, 1);
        tabs->addWidget(secondary);
        l->addLayout(tabs);
        status[1]->setObjectName("audienceState");
        l->removeWidget(status[1]);
        l->addWidget(status[1]);
        auto *list = new AudienceRankView(audience);
        list->userMenu = [this, list](const DisplaySender &sender, QPoint pos) {
            showChatUserMenu(list, service, sender.uid, sender.name, sender.message, pos);
        };
        l->addWidget(list, 1);
        audienceSummary = new QLabel;
        audienceSummary->setObjectName("audienceSummary");
        audienceSummary->setTextFormat(Qt::PlainText);
        l->addWidget(audienceSummary);
        for (auto *setting : {showMedal, showIdentity})
            QObject::connect(setting, &QCheckBox::toggled, owner, [this] { renderAudience(); });
        QObject::connect(master, &QTabBar::currentChanged, owner, [this](int) {
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
        QString previousType = master->tabData(master->currentIndex()).toString();
        QString previousSwitch;
        if (master->currentIndex() >= 0 && master->currentIndex() < choices.size())
            previousSwitch =
                choices[master->currentIndex()].switches.value(secondaryIndex).value("switch").toString();
        choices = audienceChoices(service ? service->rankInfo() : QJsonObject{});
        QSignalBlocker b(master);
        while (master->count())
            master->removeTab(master->count() - 1);
        int selected = 0;
        for (int i = 0; i < choices.size(); ++i) {
            auto &c = choices[i];
            master->addTab(c.title);
            master->setTabData(i, c.type);
            master->setTabToolTip(i, c.config.value("comment").toString());
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
        const auto sortText = sw.value("text").toString(sw.value("switch").toString());
        secondary->setText(c.switches.size() > 1 ? QStringLiteral("↕ %1").arg(sortText) : sortText);
        secondary->setAccessibleName(QStringLiteral("排序：%1").arg(sortText));
        secondary->setToolTip(sw.value("comment").toString());
        secondary->setEnabled(c.switches.size() > 1);
        service->selectAudience(c.type, sw.value("switch").toString());
        renderAudience();
    }
    void buildGuard(QVBoxLayout *l) {
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
    static QColor rankColor(const QJsonValue &value, QColor fallback = {}) {
        if (value.isString() && value.toString().startsWith('#')) {
            auto color = QColor::fromString(value.toString());
            return color.isValid() ? color : fallback;
        }
        auto rgb = positiveId(value);
        return rgb > 0 && rgb <= 0xffffff ? QColor::fromRgb(QRgb(rgb)) : fallback;
    }
    void renderAudience() {
        const int tab = master->currentIndex();
        if (tab < 0 || tab >= choices.size()) return;
        const auto &choice = choices[tab];
        const auto sort = choice.switches.value(secondaryIndex);
        const auto display = sort.value("ui_type").toObject();
        const auto data = service->audienceData();
        const auto items = data.value("item").toArray();
        const auto metric = data.value("config").toObject().value("value_text").toString(sort.value("text").toString());
        QList<AudienceRankRow> rows;
        for (const auto &v : items) {
            if (!v.isObject() || rows.size() >= 100) continue;
            const auto r = v.toObject();
            const auto user = userObject(r);
            const auto base = user.value("base").toObject();
            AudienceRankRow row;
            row.name = privateDisplayName(r);
            row.sender = displaySender(r);
            const auto score = r.value("score");
            if (!display.value("hide_rank_without_score").toInt() || positiveId(score))
                row.rank = r.value("rank").toVariant().toString();
            if (display.value("show_score").toInt(1))
                row.score = score.toVariant().toString();
            QStringList parts;
            if (!row.rank.isEmpty()) parts << row.rank;
            parts << row.name;
            if (!mystery(r)) {
                row.face = DisplayAssetCache::emoteUrl(base.value("face").toString(r.value("face").toString()));
                row.nameColor = rankColor(base.value("name_color_str"), rankColor(base.value("name_color")));
                row.glory = gloryLevel(r);
                if (row.glory > 0) parts << QStringLiteral("[荣耀 %1]").arg(row.glory);
                const auto medal = user.value("medal").toObject();
                const auto legacy = r.value("medal_info").toObject();
                const auto name = medal.value("name").toString(legacy.value("medal_name").toString());
                const auto level = medal.contains("level") ? positiveId(medal.value("level")) :
                    positiveId(legacy.contains("level") ? legacy.value("level") : legacy.value("medal_level"));
                if (showMedal->isChecked() && !name.isEmpty()) {
                    row.medal = name.left(64);
                    if (level) row.medal += ' ' + QString::number(level);
                    row.medalStart = rankColor(medal.value("v2_medal_color_start"),
                        rankColor(medal.value("color_start"), rankColor(legacy.value("medal_color_start"))));
                    row.medalEnd = rankColor(medal.value("v2_medal_color_end"),
                        rankColor(medal.value("color_end"), rankColor(legacy.value("medal_color_end"), row.medalStart)));
                    row.medalText = rankColor(medal.value("v2_medal_color_text"));
                    parts << '[' + row.medal + ']';
                }
                const auto guardInfo = user.value("guard").toObject();
                const int guardLevel = guardInfo.value("level").toInt(r.value("guard_level").toInt());
                if (guardLevel && showIdentity->isChecked()) {
                    parts << QStringLiteral("[航海 %1]").arg(guardLevel);
                    const auto expiry = guardInfo.value("expired_str").toString();
                    if (!expiry.isEmpty()) parts << expiry;
                }
            }
            if (!row.score.isEmpty()) parts << metric + QStringLiteral("：") + row.score;
            row.detail = parts.join(' ');
            rows.append(std::move(row));
        }
        const auto count = data.value("count").toInteger(-1);
        const bool complete = count >= 0 && rows.size() >= count;
        audience.replace(std::move(rows), complete);
        const auto state = service->audienceStatus();
        const bool empty = audience.rowCount() == 0;
        status[1]->setVisible(empty || service->audienceLoadFailed());
        status[1]->setText(empty && count == 0 && !service->audienceLoadFailed() && state.endsWith(QStringLiteral(" 条")) ? QStringLiteral("暂无上榜观众")
            : state.isEmpty() ? service->status() : state);
        audienceSummary->setText((choice.type == "online_rank" ? QStringLiteral("本场数据") : choice.title) +
            QStringLiteral("    上榜人数  ") + (count >= 0 ? QString::number(count) : QStringLiteral("—")));
        audienceSummary->setToolTip(state);
    }
    void renderRank(bool g) {
        if (!service || !panels[g ? 2 : 1])
            return;
        if (!g) {
            renderAudience();
            return;
        }
        const auto data = service->guardData();
        auto rows = data.value("rows").toArray();
        if (rows.isEmpty())
            rows = data.value("list").toArray();
        guard.clear();
        for (auto rv : rows) {
            const auto r = rv.toObject();
            auto u = userObject(r);
            auto score = r.value("accompany");
            QString rank = r.value("rank").toVariant().toString();
            const QString prefix = rank.isEmpty() ? QString{} : rank + "  ";
            QString s = prefix + privateDisplayName(r) + gloryBadge(r);
            if (!mystery(r)) {
                const int level =
                    r.value("guard_level").toInt(u.value("guard").toObject().value("level").toInt());
                if (level)
                    s += QStringLiteral("  [航海 %1]").arg(level);
                auto exp = u.value("guard").toObject().value("expired_str").toString();
                if (!exp.isEmpty())
                    s += "  " + exp;
            }
            const auto metric = guardSecondary->text() == QStringLiteral("陪伴天数") && guardMaster->currentIndex() == 2
                                  ? QStringLiteral("陪伴天数") : QStringLiteral("亲密度");
            s += "\n" + metric + QStringLiteral("：") + score.toVariant().toString();
            auto face = u.value("base").toObject().value("face").toString(r.value("face").toString());
            guard.append(s, mystery(r) ? QString{} : face, 0, {}, displaySender(r, int(prefix.size())));
        }
        auto state = service->guardStatus();
        if (state.isEmpty())
            state = service->status();
        status[2]->setText(state + (guard.rowCount() > 0 ? QStringLiteral(" · %1 条").arg(guard.rowCount()) : QString{}));
        more->setEnabled(service->guardHasMore());
        more->setText(service->guardHasMore() ? QStringLiteral("加载更多") : QStringLiteral("已经到底了"));
    }
    void rebuild(bool jumpToLatest = false) {
        if (stopped || !panels[0] || !stats || !chatFollow || !messageView || !antiView)
            return;
        auto hovered = [](const HistoryView *view) { return view && view->isVisible() && view->hovered; };
        if (!jumpToLatest && pause->isChecked() && (hovered(messageView) || hovered(antiView)))
            return;
        chatFollow->beginRebuild();
        messages.clear();
        anti.clear();
        QSet<QString> seen;
        for (const auto &received : std::as_const(history)) {
            const auto &e = received.event;
            auto cmd = e.value("cmd").toString().section(':', 0, 0);
            auto data = e.value("data").toObject();
            auto id = data.value("tid").toString(data.value("id_str").toString());
            if (id.isEmpty() && cmd == "SUPER_CHAT_MESSAGE")
                id = data.value("id").toVariant().toString();
            if (!cmd.startsWith("DANMU_MSG") && !id.isEmpty()) {
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
                QJsonObject row{{"user", ext.value("user")}, {"name", av(legacy, 1)}, {"uid", av(legacy, 0)}};
                auto medal = showMedal->isChecked() ? av(info, 3).toArray() : QJsonArray{};
                QStringList identity;
                if (showIdentity->isChecked() && av(legacy, 2).toInt())
                    identity << QStringLiteral("[房管]");
                const int guardLevel = userObject(row).value("guard").toObject().value("level").toInt();
                if (showIdentity->isChecked() && guardLevel)
                    identity << QStringLiteral("[航海 %1]").arg(guardLevel);
                auto line = chatHeader(row, gloryLevel(row, positiveId(av(av(info, 16).toArray(), 0))),
                                       av(medal, 1).toString(), positiveId(av(medal, 0)), positiveId(av(medal, 7)),
                                       identity.join(' '));
                line.text += QStringLiteral("：");
                if (received.reply)
                    line.text += QStringLiteral("回复： ");
                auto emotes = received.emotes;
                for (auto &emote : emotes)
                    emote.start += int(line.text.size());
                const auto body = av(info, 1).toString();
                line.sender.message = body;
                messages.append(
                    line.text + body,
                    mystery(row)
                        ? QString{}
                        : ext.value("user").toObject().value("base").toObject().value("face").toString(),
                    received.highlightDeadline, std::move(emotes), std::move(line.sender), received.sequence,
                    std::move(line.spans));
            } else if (cmd == "SEND_GIFT") {
                if (!showGifts->isChecked() || (paidOnly->isChecked() && data.value("coin_type") != "gold"))
                    continue;
                const auto medal = showMedal->isChecked() ? data.value("medal_info").toObject() : QJsonObject{};
                const auto level = positiveId(data.value("guard_level"));
                auto line = chatHeader(data, gloryLevel(data), medal.value("medal_name").toString(),
                                       positiveId(medal.value("medal_level")), positiveId(medal.value("medal_color_start")),
                                       showIdentity->isChecked() && level ? QStringLiteral("[航海 %1]").arg(level) : QString{});
                line.text += ' ';
                line.add(ChatSpan::GiftAction, QStringLiteral("投喂"));
                line.text += ' ';
                line.add(ChatSpan::GiftName, data.value("giftName").toString(QStringLiteral("礼物")).left(512));
                line.text += ' ';
                const auto icon = QStringLiteral("🎁");
                const int iconStart = int(line.text.size());
                line.text += icon;
                const auto count = positiveId(data.value("num"));
                if (count > 1)
                    line.text += QStringLiteral(" ×%1").arg(count);
                QList<DanmakuEmote> images;
                const auto url = assets.giftImageUrl(positiveId(data.value("giftId")), data.value("gift_info").toObject());
                if (!url.isEmpty())
                    images.append({iconStart, int(icon.size()), url, false});
                const auto face = mystery(data) ? QString{} : data.value("face").toString();
                messages.append(line.text, face, 0, std::move(images), std::move(line.sender), received.sequence, std::move(line.spans));
            } else if (cmd == "SUPER_CHAT_MESSAGE") {
                QJsonObject row = data;
                row.insert("name", data.value("user_info").toObject().value("uname"));
                const auto text = privateDisplayName(row) + gloryBadge(row) + QStringLiteral("  醒目留言：") + data.value("message").toString();
                if (showGifts->isChecked())
                    messages.append(text, {}, 0, {}, displaySender(row), received.sequence);
            } else if (cmd == "USER_TOAST_MSG_V2") {
                int op = data.value("guard_info").toObject().value("op_type").toInt();
                if (!data.value("option").toObject().value("anchor_show").toBool() || op < 1 || op > 3)
                    continue;
                const auto text = privateDisplayName(data) + gloryBadge(data) + QStringLiteral("  大航海：") +
                                  data.value("guard_info").toObject().value("role_name").toString();
                if (showGifts->isChecked())
                    messages.append(text, {}, 0, {}, displaySender(data), received.sequence);
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
                auto line = activityLine(data, kind, showMedal->isChecked());
                anti.append(line.text, {}, cmd == "INTERACT_WORD" && t == 1 ? received.highlightDeadline : 0,
                            {}, std::move(line.sender), received.sequence, std::move(line.spans));
            }
        }
        stats->setText(QStringLiteral("看过 %1  ·  点赞 %2").arg(watched, likes));
        updateAppearance();
        chatFollow->endRebuild();
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
                                 std::array<QDockWidget *, DockCount> registeredDocks)
    : QObject(p), d(std::make_unique<Impl>(this, s, path, p, registeredDocks)) {
    d->docks[0]->installEventFilter(this);
    d->docks[1]->installEventFilter(this);
    qApp->installEventFilter(this);
}
FloatingWindows::~FloatingWindows() { shutdown(); }
bool FloatingWindows::eventFilter(QObject *watched, QEvent *event) {
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange && !d->stopped)
        d->themeTimer.start(0); // OBS applies its stylesheet immediately after the application palette.
    if (watched == d->docks[1] && event->type() == QEvent::HideToParent &&
        d->audienceLocked && !d->stopped && !d->applyingFlags && d->docks[1]->isHidden())
        d->setLocked(false, 1);
    if (watched == d->docks[0] && event->type() == QEvent::HideToParent &&
        d->locked && !d->stopped && !d->applyingFlags && d->docks[0]->isHidden())
        d->setLocked(false); // The OBS Docks menu blocks visibilityChanged.
    return QObject::eventFilter(watched, event);
}
const char *FloatingWindows::dockId(int kind) {
    static constexpr std::array<const char *, DockCount> ids{
        "livehime-obs-interaction", "livehime-obs-audience", "livehime-obs-guard"};
    return kind >= 0 && kind < DockCount ? ids[kind] : "";
}
QString FloatingWindows::dockTitle(int kind) {
    static const std::array<QString, DockCount> titles{QStringLiteral("直播互动"), QStringLiteral("房间观众"),
                                                       QStringLiteral("大航海")};
    return kind >= 0 && kind < DockCount ? titles[kind] : QString{};
}
QDockWidget *FloatingWindows::dockWidget(int kind) const {
    return kind >= 0 && kind < DockCount ? d->docks[kind].data() : nullptr;
}
QWidget *FloatingWindows::panelWidget(int kind) const {
    return kind >= 0 && kind < DockCount ? d->panels[kind].data() : nullptr;
}
bool FloatingWindows::interactionLocked() const {
    return d->locked && d->docks[0] && d->docks[0]->isFloating() &&
           d->docks[0]->windowFlags().testFlag(Qt::WindowTransparentForInput);
}
void FloatingWindows::showWindow(int kind) {
    if (kind < 0 || kind >= DockCount || d->stopped || !d->docks[kind])
        return;
    if (kind <= 1)
        d->setLocked(false, kind);
    auto *w = d->docks[kind].data();
    if (w->isFloating())
        d->correct(w);
    w->show();
    if (kind == 0)
        d->rebuild();
    w->raise();
    if (w->isFloating())
        w->activateWindow();
}
void FloatingWindows::unlockInteraction() { d->setLocked(false); }
void FloatingWindows::dockWindow(int kind) {
    if (kind < 0 || kind >= DockCount || d->stopped || !d->docks[kind])
        return;
    if (kind <= 1)
        d->setLocked(false, kind);
    d->docks[kind]->setFloating(false);
    d->applyFlags(kind);
    if (d->ownHost && d->host)
        d->host->show();
    d->docks[kind]->show();
    d->docks[kind]->raise();
}
void FloatingWindows::floatWindow(int kind) {
    if (kind < 0 || kind >= DockCount || d->stopped || !d->docks[kind])
        return;
    if (kind <= 1)
        d->setLocked(false, kind);
    d->docks[kind]->setFloating(true);
    d->applyFlags(kind);
    showWindow(kind);
}
void FloatingWindows::shutdown() {
    if (d->stopped)
        return;
    unlockInteraction();
    d->setLocked(false, 1);
    d->stopped = true;
    d->renderTimer.stop();
    d->themeTimer.stop();
    qApp->removeEventFilter(this);
    if (d->docks[0])
        d->docks[0]->removeEventFilter(this);
    if (d->docks[1])
        d->docks[1]->removeEventFilter(this);
    for (int k = 0; k < DockCount; ++k) {
        if (!d->docks[k])
            continue;
        if (d->docks[k]->isFloating())
            d->settings.setValue(QString("geometry%1").arg(k), d->docks[k]->saveGeometry());
        d->docks[k]->hide();
        if (d->service)
            d->service->setVisible(k, false);
    }
    if (d->entryDialog)
        d->entryDialog->close();
    d->settings.sync();
}
} // namespace livehime
