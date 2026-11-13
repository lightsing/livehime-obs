#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
namespace livehime {
struct GoimPacket {
    quint16 version;
    quint32 operation;
    quint32 sequence;
    QByteArray body;
};
class GoimCodec {
  public:
    static constexpr qsizetype MaxBody = 10 * 1024 * 1024;
    static QByteArray frame(quint32 operation, quint32 sequence, const QByteArray &body = {},
                            quint16 version = 1);
    bool feed(const QByteArray &bytes, QVector<GoimPacket> &packets, QString &error);
    void reset() { buffer_.clear(); }

  private:
    bool decode(const QByteArray &bytes, int depth, qsizetype &budget, QVector<GoimPacket> &packets,
                QString &error);
    QByteArray buffer_;
};
} // namespace livehime
