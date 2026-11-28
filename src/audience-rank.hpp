#pragma once

#include "display-asset-cache.hpp"
#include "floating-windows.hpp"
#include "panel-palette.hpp"
#include <QContextMenuEvent>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <functional>
#include <utility>

namespace livehime {
struct AudienceRankRow {
    QString rank, score, name, face, medal, detail;
    QColor nameColor, medalStart, medalEnd, medalText;
    qint64 glory = 0;
    DisplaySender sender;
};

class AudienceRankModel final : public QAbstractListModel {
  public:
    explicit AudienceRankModel(DisplayAssetCache &assets) : assets_(assets) {
        connect(&assets, &DisplayAssetCache::ready, this, [this](const QString &url) {
            if (!showAvatars_) return;
            for (int i = 0; i < rows_.size(); ++i)
                if (rows_[i].face == url)
                    emit dataChanged(index(i), index(i), {Qt::DecorationRole});
        });
    }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(rows_.size()); }
    const AudienceRankRow &row(int i) const { return rows_.at(i); }
    bool showAvatars() const { return showAvatars_; }
    bool complete() const { return complete_; }
    QVariant data(const QModelIndex &idx, int role) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= rows_.size()) return {};
        const auto &r = row(idx.row());
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole || role == Qt::ToolTipRole)
            return r.detail;
        if (role == DisplayListModel::SenderRole && r.sender.uid > 0)
            return QVariant::fromValue(r.sender);
        if (role == Qt::DecorationRole && showAvatars_ && !r.face.isEmpty())
            return QVariant::fromValue(assets_.get(r.face));
        return {};
    }
    void replace(QList<AudienceRankRow> rows, bool complete) {
        beginResetModel();
        rows_ = std::move(rows);
        complete_ = complete;
        endResetModel();
    }
    void clear() { replace({}, false); }
    void setAvatarsVisible(bool visible) {
        if (showAvatars_ == visible) return;
        showAvatars_ = visible;
        if (rowCount()) emit dataChanged(index(0), index(rowCount() - 1), {Qt::DecorationRole});
    }
  private:
    DisplayAssetCache &assets_;
    QList<AudienceRankRow> rows_;
    bool complete_ = false, showAvatars_ = true;
};

class AudienceRankDelegate final : public QStyledItemDelegate {
  public:
    AudienceRankDelegate(AudienceRankModel &model, QWidget *parent) : QStyledItemDelegate(parent), model_(model) {}

    QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &idx) const override {
        const int height = rowHeight(o.fontMetrics);
        return {o.rect.width(), height + (isLast(idx) ? height / 2 : 0)};
    }
    bool hitsSender(const QModelIndex &idx, const QRect &rect, QPoint point, const QFont &font) const {
        if (!idx.isValid()) return false;
        const auto &r = model_.row(idx.row());
        QFontMetrics metrics(font);
        auto g = geometry(rect, metrics, r);
        return (model_.showAvatars() && g.face.contains(point)) ||
               QRect(g.name.topLeft(), QSize(qMin(g.name.width(), metrics.horizontalAdvance(r.name)), g.name.height())).contains(point);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &idx) const override {
        const auto &colors = PanelPalette::current();
        const auto &r = model_.row(idx.row());
        const auto g = geometry(o.rect, o.fontMetrics, r);
        p->save();
        p->setClipRect(o.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        p->setFont(o.font);
        if (o.state.testFlag(QStyle::State_MouseOver))
            p->fillRect(QRect(o.rect.topLeft(), QSize(o.rect.width(), rowHeight(o.fontMetrics))), colors.hover);
        const int rank = r.rank.toInt();
        if (rank >= 1 && rank <= 3) {
            auto f = badgeFont(o.font);
            f.setBold(true);
            const auto fill = colors.badge(colors.giftName);
            paintBadge(*p, g.rank.x(), g.rank.center().y(), g.rank.width(), QStringLiteral("榜%1").arg(rank),
                       f, fill, fill, colors.badgeText(fill, colors.giftName));
        } else {
            p->setPen(colors.muted);
            p->drawText(g.rank, Qt::AlignCenter, r.rank.isEmpty() ? QStringLiteral("–") : r.rank);
        }
        if (model_.showAvatars()) {
            const auto pixmap = idx.data(Qt::DecorationRole).value<QPixmap>();
            p->save();
            QPainterPath circle;
            circle.addEllipse(QRectF(g.face));
            p->setClipPath(circle, Qt::IntersectClip);
            if (pixmap.isNull()) {
                p->fillRect(g.face, colors.badge(colors.muted));
                p->setPen(Qt::NoPen);
                p->setBrush(colors.muted);
                const auto c = QRectF(g.face).center();
                const qreal unit = g.face.width() / 10.;
                p->drawEllipse(c + QPointF(0, -unit), 1.8 * unit, 1.8 * unit);
                p->drawEllipse(c + QPointF(0, 4.7 * unit), 3.5 * unit, 3.5 * unit);
            } else {
                const int side = qMin(pixmap.width(), pixmap.height());
                p->drawPixmap(g.face, pixmap, QRect((pixmap.width() - side) / 2, (pixmap.height() - side) / 2, side, side));
            }
            p->restore();
        }
        const QColor background = o.state.testFlag(QStyle::State_MouseOver) ? colors.hover : colors.surface;
        p->setPen(colors.readable(r.nameColor.isValid() ? colors.mix(r.nameColor, colors.text, .2) : colors.text,
                                  background, colors.text));
        p->drawText(g.name, Qt::AlignLeft | Qt::AlignVCenter, o.fontMetrics.elidedText(r.name, Qt::ElideRight, g.name.width()));
        p->setPen(colors.text);
        p->drawText(g.score, Qt::AlignRight | Qt::AlignVCenter, o.fontMetrics.elidedText(r.score, Qt::ElideRight, g.score.width()));
        const auto f = badgeFont(o.font);
        int x = g.name.left();
        if (r.glory > 0) {
            const auto fill = colors.badge(colors.accent);
            x += paintBadge(*p, x, g.badgeY, g.name.width(), QStringLiteral("◆ %1").arg(r.glory), f,
                            fill, fill, colors.badgeText(fill)) + 3;
        }
        if (!r.medal.isEmpty()) {
            const auto start = colors.badge(r.medalStart);
            const auto end = colors.mix(start, colors.badge(r.medalEnd), .5);
            const auto ink = colors.badgeText(end, colors.badgeText(start, r.medalText));
            paintBadge(*p, x, g.badgeY, g.name.right() - x + 1, r.medal, f, start, end, ink);
        }
        if (isLast(idx)) {
            p->setFont(f);
            p->setPen(colors.muted);
            p->drawText(o.rect.adjusted(0, rowHeight(o.fontMetrics), 0, 0), Qt::AlignCenter, QStringLiteral("已经到底了哦~"));
        }
        p->restore();
    }
  private:
    struct Geometry { QRect rank, face, name, score; int badgeY; };
    static int rowHeight(const QFontMetrics &fm) { return qMax(39, 2 * fm.height() + 6); }
    bool isLast(const QModelIndex &idx) const { return model_.complete() && idx.row() + 1 == model_.rowCount(); }
    Geometry geometry(const QRect &rect, const QFontMetrics &fm, const AudienceRankRow &r) const {
        const int height = rowHeight(fm), top = rect.top(), center = top + height / 2;
        const int side = qBound(26, fm.height() * 2, 36);
        const int rankWidth = qMax(25, fm.horizontalAdvance(QStringLiteral("榜3")) + 5);
        const QRect rank(rect.left() + 2, top, rankWidth, height);
        const QRect face(rank.right() + 5, center - side / 2, side, side);
        const int x = model_.showAvatars() ? face.right() + 7 : rank.right() + 5;
        const int sw = r.score.isEmpty() ? 0 : qMin(rect.width() / 3, fm.horizontalAdvance(r.score) + 1);
        const QRect score(rect.right() - sw - 6, top, sw, height);
        const bool badges = r.glory > 0 || !r.medal.isEmpty();
        const QRect name(x, badges ? center - fm.height() : top, qMax(0, score.x() - x - 5),
                         badges ? fm.height() : height);
        return {rank, face, name, score, center + fm.height() / 2};
    }
    static QFont badgeFont(QFont font) {
        const QFontMetrics fm(font);
        font.setPixelSize(qMax(9, qRound(fm.height() * .65)));
        font.setWeight(QFont::DemiBold);
        return font;
    }
    static int paintBadge(QPainter &p, int x, int centerY, int available, const QString &text, const QFont &f,
                          QColor start, QColor end, QColor foreground) {
        if (available < 12) return 0;
        QFontMetrics fm(f);
        const int width = qMin(available, fm.horizontalAdvance(text) + 8);
        const int h = fm.height() + 1;
        const QRectF pill(x, centerY - h / 2., width, h);
        QLinearGradient gradient(pill.topLeft(), pill.topRight());
        gradient.setColorAt(0, start);
        gradient.setColorAt(1, end);
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawRoundedRect(pill, h / 2., h / 2.);
        p.setFont(f);
        p.setPen(foreground);
        p.drawText(pill, Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, width - 4));
        p.restore();
        return width;
    }
    AudienceRankModel &model_;
};

class AudienceRankView final : public QListView {
  public:
    explicit AudienceRankView(AudienceRankModel &model) : delegate_(new AudienceRankDelegate(model, this)) {
        setObjectName("audienceRanking");
        setAccessibleName(QStringLiteral("房间观众榜单"));
        setFrameShape(QFrame::NoFrame);
        setMouseTracking(true);
        setSelectionMode(QAbstractItemView::NoSelection);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setModel(&model);
        setItemDelegate(delegate_);
    }
    std::function<void(const DisplaySender &, QPoint)> userMenu;
  protected:
    void contextMenuEvent(QContextMenuEvent *event) override {
        const auto point = viewport()->mapFromGlobal(event->globalPos());
        const auto idx = indexAt(point);
        const auto sender = idx.data(DisplayListModel::SenderRole).value<DisplaySender>();
        if (userMenu && sender.uid > 0 && delegate_->hitsSender(idx, visualRect(idx), point, font())) {
            userMenu(sender, event->globalPos());
            event->accept();
        } else {
            QListView::contextMenuEvent(event);
        }
    }
  private:
    AudienceRankDelegate *delegate_;
};
} // namespace livehime
