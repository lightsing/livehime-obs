#pragma once
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QObject>
#include <QThread>
namespace livehime {
class GoimWorker;
class GoimClient final : public QObject {
    Q_OBJECT
  public:
    enum class Progress { Connecting, Connected, Waiting };
    Q_ENUM(Progress)
    explicit GoimClient(QObject *parent = nullptr);
    ~GoimClient() override;
    void setContext(qint64 uid, qint64 roomId, QString token, QString host, quint16 port, QString buvid);
    void stop();
  signals:
    void events(QJsonArray events);
    void statusChanged(QString status);
    void connectionProgress(GoimClient::Progress phase, QString detail, QDeadlineTimer retryAt);
    void tokenRefreshRequested();
    void endpointFailed();
    void transportPacketObserved(quint32 operation);

  private:
    QThread thread_;
    GoimWorker *worker_;
    quint64 generation_ = 0;
};
} // namespace livehime
