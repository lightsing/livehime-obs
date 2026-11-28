#pragma once
#include "danmaku-emotes.hpp"
#include <QAbstractListModel>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <array>
#include <memory>

class QDockWidget;

namespace livehime {
class RoomRealtime;
class DisplayAssetCache;
struct DisplaySender {
    qint64 uid = 0;
    QString name;
    int nameOffset = 0;
    QString message;
};
struct ChatSpan {
    enum Kind { Glory, Medal, Nickname, GiftAction, GiftName, EntryName, EntryLabel, EntryTime };
    Kind kind;
    int start, length;
    QString badgeText;
    QColor badgeColor;
};
// Text stays available for tooltips and as a fallback while an emote is loading.
class DisplayListModel final : public QAbstractListModel {
  public:
    enum { HighlightDeadlineRole = Qt::UserRole + 1, EmotesRole, EmoteImagesRole, SenderRole, MessageSequenceRole, ChatSpansRole };
    explicit DisplayListModel(int limit, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariantMap emoteImages(const QModelIndex &, bool requestMissing) const;
    void append(const QString &text, const QString &avatar = {}, qint64 highlightDeadline = 0,
                QList<DanmakuEmote> emotes = {}, DisplaySender sender = {}, qint64 sequence = 0,
                QList<ChatSpan> spans = {});
    void setAssets(DisplayAssetCache *cache, bool showEmotes = false);
    void setAvatarsVisible(bool visible);
    void replace(const QStringList &rows);
    void clear();

  private:
    int limit_;
    struct Row {
        QString text, avatar;
        qint64 highlightDeadline = 0;
        QList<DanmakuEmote> emotes;
        DisplaySender sender;
        qint64 sequence = 0;
        QList<ChatSpan> spans;
    };
    QList<Row> rows_;
    QPointer<DisplayAssetCache> assets_;
    bool showAvatars_ = true;
};
struct RankChoice {
    QString type, title;
    QJsonObject config;
    QList<QJsonObject> switches;
    bool enabled = true, preferred = false;
};
QList<RankChoice> audienceChoices(const QJsonObject &rankInfo);
QString privateDisplayName(const QJsonObject &row);
class FloatingWindows final : public QObject {
  public:
    static constexpr int DockCount = 3;
    FloatingWindows(RoomRealtime *, QString storageDirectory, QWidget *parent = nullptr,
                    std::array<QDockWidget *, DockCount> registeredDocks = {});
    ~FloatingWindows() override;
    void showWindow(int kind);
    void unlockInteraction();
    void dockWindow(int kind);
    void floatWindow(int kind);
    QDockWidget *dockWidget(int kind) const;
    QWidget *panelWidget(int kind) const;
    bool interactionLocked() const;
    static const char *dockId(int kind);
    static QString dockTitle(int kind);
    void shutdown();

  private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace livehime

Q_DECLARE_METATYPE(livehime::DisplaySender)
