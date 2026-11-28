#include "goim-client.hpp"
#include "beta-diagnostics.hpp"
#include <QMetaEnum>
#include "gift-v2.hpp"
#include "goim-codec.hpp"
#include "protocol.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
namespace livehime {
class GoimWorker final : public QObject {
    Q_OBJECT
  public:
    void context(quint64 generation, qint64 uid, qint64 room, QString token, QString host, quint16 port,
                 QString buvid) {
        generation_ = generation;
        consecutiveFailures_ = 0;
        const bool same = uid_ == uid && room_ == room && buvid_ == buvid;
        shutdown();
        uid_ = uid;
        room_ = room;
        token_ = std::move(token);
        host_ = std::move(host);
        port_ = port;
        buvid_ = std::move(buvid);
        if (!same || queue_.isEmpty())
            queue_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (room_ <= 0 || token_.isEmpty() || host_.isEmpty() || !port_) {
            emit statusChanged(generation_, QStringLiteral("弹幕配置不完整"));
            return;
        }
        stopped_ = false;
        connectSocket();
    }
    void shutdown(quint64 generation = 0) {
        if (generation)
            generation_ = generation;
        pending_ = {};
        pendingBytes_ = 0;
        inFlight_ = false;
        stopped_ = true;
        authenticated_ = false;
        if (retry_)
            retry_->stop();
        if (heartbeat_)
            heartbeat_->stop();
        if (deadline_)
            deadline_->stop();
        if (socket_) {
            socket_->disconnect(this);
            socket_->abort();
            delete socket_;
            socket_ = nullptr;
        }
        codec_.reset();
        token_.clear();
        emit statusChanged(generation_, QStringLiteral("弹幕已停止"));
    }
  signals:
    void events(quint64 generation, QJsonArray events);
    void statusChanged(quint64 generation, QString status);
    void connectionProgress(quint64 generation, GoimClient::Progress phase, QString detail, QDeadlineTimer retryAt);
    void tokenRefreshRequested(quint64 generation);
    void endpointFailed(quint64 generation);
    void transportPacketObserved(quint64 generation, quint32 operation);

  public:
    void delivered(quint64 generation) {
        if (generation != generation_)
            return;
        inFlight_ = false;
        receive();
    }

  private:
    void report(GoimClient::Progress phase, const QString &detail, QDeadlineTimer retryAt = QDeadlineTimer::Forever) {
        emit statusChanged(generation_, detail);
#if LIVEHIME_BETA_BUILD
        diagnostics::record("danmaku.connection", {{"phase", QMetaEnum::fromType<GoimClient::Progress>().valueToKey(int(phase))},
                                                   {"failures", consecutiveFailures_}});
#endif
        emit connectionProgress(generation_, phase, detail, retryAt);
    }
    void send(quint32 op, const QByteArray &body = {}) {
        if (!socket_ || socket_->state() != QAbstractSocket::ConnectedState)
            return;
        if (socket_->bytesToWrite() > 1024 * 1024) {
            failed();
            return;
        }
        const auto frame = GoimCodec::frame(op, sequence_++, body);
        if (socket_->write(frame) != frame.size())
            failed();
    }
    void failed() {
        if (stopped_ || !socket_)
            return;
#if LIVEHIME_BETA_BUILD
        diagnostics::record("danmaku.disconnect", {{"socket_error", int(socket_->error())}, {"authenticated", authenticated_}});
#endif
        authenticated_ = false;
        heartbeat_->stop();
        deadline_->stop();
        if (socket_) {
            socket_->disconnect(this);
            socket_->abort();
            socket_->deleteLater();
            socket_ = nullptr;
        }
        codec_.reset();
        pending_ = {};
        pendingBytes_ = 0;
        if (++consecutiveFailures_ >= 3) {
            shutdown();
            emit statusChanged(generation_, QStringLiteral("弹幕节点连接失败，正在切换节点"));
            emit endpointFailed(generation_);
            return;
        }
        retry_->start(1000);
        report(GoimClient::Progress::Waiting, QStringLiteral("弹幕连接中断，正在重连"),
               QDeadlineTimer(retry_->remainingTime(), Qt::PreciseTimer));
    }
    void connectSocket() {
        if (stopped_)
            return;
        if (!retry_) {
            retry_ = new QTimer(this);
            retry_->setSingleShot(true);
            connect(retry_, &QTimer::timeout, this, &GoimWorker::connectSocket);
            heartbeat_ = new QTimer(this);
            heartbeat_->setTimerType(Qt::PreciseTimer);
            heartbeat_->setInterval(21000);
            connect(heartbeat_, &QTimer::timeout, this, [this] {
                if (authenticated_)
                    send(2);
            });
            deadline_ = new QTimer(this);
            deadline_->setSingleShot(true);
            connect(deadline_, &QTimer::timeout, this, &GoimWorker::failed);
        }
        sequence_ = 1;
        codec_.reset();
        socket_ = new QTcpSocket(this);
        socket_->setReadBufferSize(256 * 1024);
        connect(socket_, &QTcpSocket::connected, this, [this] {
            QJsonObject login{{"uid", uid_},
                              {"roomid", room_},
                              {"key", token_},
                              {"protover", 3},
                              {"app_id", 102},
                              {"mobi_app", "pc_link"},
                              {"platform", "pc_link"},
                              {"channel", "bili"},
                              {"build", protocol::ClientBuild},
                              {"buvid", buvid_},
                              {"version_name", QString::fromLatin1(protocol::ClientVersion)},
                              {"device", "pc"},
                              {"osver", "Windows"},
                              {"support_ack", true},
                              {"queue_uuid", queue_}};
            send(7, QJsonDocument(login).toJson(QJsonDocument::Compact));
            if (socket_)
                report(GoimClient::Progress::Connecting, QStringLiteral("弹幕认证中"));
        });
        connect(socket_, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { failed(); });
        connect(socket_, &QTcpSocket::disconnected, this, &GoimWorker::failed);
        connect(socket_, &QTcpSocket::readyRead, this, [this] { receive(); });
        report(GoimClient::Progress::Connecting, QStringLiteral("弹幕连接中"));
        deadline_->start(15000);
        socket_->connectToHost(host_, port_);
    }
    void receive() {
        if (inFlight_ || stopped_)
            return;
        if (pending_.isEmpty() && socket_ && socket_->bytesAvailable()) {
            QVector<GoimPacket> packets;
            QString error;
            if (!codec_.feed(socket_->read(16384), packets, error)) {
                failed();
                return;
            }
            for (const auto &packet : packets) {
                if (packet.operation == 8) {
                    QJsonParseError parse;
                    auto json = packet.body;
                    if (json.endsWith('\0'))
                        json.chop(1);
                    const auto doc = QJsonDocument::fromJson(json, &parse);
                    if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
                        !doc.object().value("code").isDouble()) {
                        failed();
                        return;
                    }
                    emit transportPacketObserved(generation_, 8);
                    const int code = doc.object().value("code").toInt();
                    if (code == -101) {
                        shutdown();
                        emit statusChanged(generation_, QStringLiteral("弹幕令牌已过期"));
                        emit tokenRefreshRequested(generation_);
                        return;
                    }
                    if (code != 0) {
                        failed();
                        return;
                    }
                    consecutiveFailures_ = 0;
                    authenticated_ = true;
                    deadline_->start(60000);
                    heartbeat_->start();
                    report(GoimClient::Progress::Connected, QStringLiteral("弹幕已连接"));
                } else if (packet.operation == 3) {
                    if (packet.body.size() != 4) {
                        failed();
                        return;
                    }
                    emit transportPacketObserved(generation_, 3);
                    deadline_->start(60000);
                    // No online-user/heat interpretation is assigned to this opaque transport value.
                } else if (packet.operation == 5 && authenticated_) {
                    QJsonParseError parse;
                    auto json = packet.body;
                    if (json.endsWith('\0'))
                        json.chop(1);
                    const auto doc = QJsonDocument::fromJson(json, &parse);
                    if (parse.error != QJsonParseError::NoError || !doc.isObject())
                        continue;
                    const auto obj = doc.object();
                    const auto data = obj;
                    // Raw GoimLoop evidence reads these ACK fields from the parsed root dictionary.
                    if (data.value("p_is_ack").toBool() && data.value("msg_id").isString() &&
                        data.value("cmd").isString() && data.value("p_msg_type").isDouble()) {
                        QJsonObject ack{{"msg_id", data.value("msg_id")},
                                        {"cmd", data.value("cmd")},
                                        {"p_msg_type", data.value("p_msg_type")}};
                        send(24, QJsonDocument(ack).toJson(QJsonDocument::Compact));
                    }
                    const auto normalized =
                        obj.value("cmd").toString() == "SEND_GIFT_V2" ? decodeGiftV2(obj) : QJsonArray{obj};
                    for (const auto &event : normalized) {
                        const auto eventBytes =
                            QJsonDocument(event.toObject()).toJson(QJsonDocument::Compact).size();
                        if (eventBytes > 256 * 1024 || pendingBytes_ + eventBytes > 1024 * 1024 ||
                            pending_.size() >= 1024) {
                            failed();
                            return;
                        }
                        pending_.append(event);
                        pendingBytes_ += eventBytes;
                    }
                }
            }
        }
        if (!pending_.isEmpty()) {
            QJsonArray batch;
            qsizetype bytes = 0;
            while (!pending_.isEmpty() && batch.size() < 256) {
                const auto value = pending_.first();
                const auto size = QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).size();
                if (!batch.isEmpty() && bytes + size > 256 * 1024)
                    break;
                batch.append(value);
                pending_.removeFirst();
                bytes += size;
            }
            pendingBytes_ = 0;
            for (const auto &value : pending_)
                pendingBytes_ += QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).size();
            inFlight_ = true;
            emit events(generation_, batch);
        } else if (socket_ && socket_->bytesAvailable())
            QMetaObject::invokeMethod(this, [this] { receive(); }, Qt::QueuedConnection);
    }
    quint64 generation_ = 0;
    QJsonArray pending_;
    qsizetype pendingBytes_ = 0;
    bool inFlight_ = false;
    QTcpSocket *socket_ = nullptr;
    QTimer *retry_ = nullptr;
    QTimer *heartbeat_ = nullptr;
    QTimer *deadline_ = nullptr;
    GoimCodec codec_;
    quint32 sequence_ = 1;
    int consecutiveFailures_ = 0;
    bool stopped_ = true, authenticated_ = false;
    qint64 uid_ = 0, room_ = 0;
    quint16 port_ = 0;
    QString token_, host_, buvid_, queue_;
};
GoimClient::GoimClient(QObject *parent) : QObject(parent), worker_(new GoimWorker) {
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &GoimWorker::events, this, [this](quint64 generation, const QJsonArray &batch) {
        QPointer<GoimClient> guard(this);
        if (generation == generation_)
            emit events(batch);
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, generation] { worker->delivered(generation); }, Qt::QueuedConnection);
    });
    connect(worker_, &GoimWorker::statusChanged, this, [this](quint64 generation, const QString &status) {
        if (generation == generation_)
            emit statusChanged(status);
    });
    connect(worker_, &GoimWorker::connectionProgress, this,
            [this](quint64 generation, Progress phase, const QString &detail, QDeadlineTimer retryAt) {
                if (generation == generation_)
                    emit connectionProgress(phase, detail, retryAt);
            });
    connect(worker_, &GoimWorker::tokenRefreshRequested, this, [this](quint64 generation) {
        if (generation == generation_)
            emit tokenRefreshRequested();
    });
    connect(worker_, &GoimWorker::endpointFailed, this, [this](quint64 generation) {
        if (generation == generation_)
            emit endpointFailed();
    });
    connect(worker_, &GoimWorker::transportPacketObserved, this,
            [this](quint64 generation, quint32 operation) {
                if (generation == generation_)
                    emit transportPacketObserved(operation);
            });
    thread_.start();
}
GoimClient::~GoimClient() {
    QMetaObject::invokeMethod(
        worker_, [worker = worker_] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}
void GoimClient::setContext(qint64 uid, qint64 roomId, QString token, QString host, quint16 port,
                            QString buvid) {
    const auto generation = ++generation_;
    QMetaObject::invokeMethod(
        worker_,
        [=, worker = worker_] { worker->context(generation, uid, roomId, token, host, port, buvid); },
        Qt::QueuedConnection);
}
void GoimClient::stop() {
    const auto generation = ++generation_;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, generation] { worker->shutdown(generation); }, Qt::QueuedConnection);
}
} // namespace livehime
#include "goim-client.moc"
