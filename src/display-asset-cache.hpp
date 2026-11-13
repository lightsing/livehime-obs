#pragma once
#include <QCache>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QQueue>
#include <QSet>
namespace livehime {
// Public display assets only. This manager never receives the login cookie jar.
class DisplayAssetCache final : public QObject {
    Q_OBJECT
  public:
    explicit DisplayAssetCache(QObject *parent = nullptr);
    QPixmap get(const QString &url);
  signals:
    void ready();

  private:
    void pump();
    QNetworkAccessManager network_;
    QCache<QString, QPixmap> cache_{16384}; // KiB; at most 16 MiB decoded pixels
    QQueue<QString> pending_;
    QSet<QString> known_;
    int active_ = 0;
};
} // namespace livehime
