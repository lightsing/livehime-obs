#pragma once
#include <QCache>
#include <QDeadlineTimer>
#include <QNetworkAccessManager>
#include <QPair>
#include <QPixmap>
#include <QQueue>
#include <QSet>
namespace livehime {
// Public display assets only. This manager never receives the login cookie jar.
class DisplayAssetCache final : public QObject {
    Q_OBJECT
  public:
    enum class Usage { Avatar, Emote };
    explicit DisplayAssetCache(QObject *parent = nullptr);
    QPixmap get(const QString &url);
    static QString emoteUrl(const QString &url);
    QPixmap cachedEmote(const QString &normalizedUrl) const;
    QPixmap emote(const QString &normalizedUrl);
    void storePreview(const QString &url, Usage usage, const QPixmap &preview);
  signals:
    void ready(const QString &url);
    void emoteReady(const QString &normalizedUrl);

  private:
    using Key = QPair<QString, Usage>;
    QPixmap requestPreview(const QString &url, Usage usage);
    void pump();
    QCache<QString, QPixmap> avatars_{16384}, emotes_{8192}; // decoded KiB
    QCache<Key, QDeadlineTimer> failures_{256};
    QQueue<Key> pending_;
    QSet<Key> requested_;
    int active_ = 0;
    QNetworkAccessManager network_;
};
} // namespace livehime
