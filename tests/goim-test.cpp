#include "goim-client.hpp"
#include "goim-codec.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QtEndian>
#include <brotli/encode.h>
#include <cstdio>
#include <functional>
#include <zlib.h>
using namespace livehime;
static void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
static bool waitFor(std::function<bool()> predicate, int timeout = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return predicate();
}
static QByteArray compressed(const QByteArray &input, int version) {
    if (version == 2) {
        uLongf size = compressBound(static_cast<uLong>(input.size()));
        QByteArray out(static_cast<qsizetype>(size), '\0');
        check(compress2(reinterpret_cast<Bytef *>(out.data()), &size,
                        reinterpret_cast<const Bytef *>(input.constData()), static_cast<uLong>(input.size()),
                        6) == Z_OK,
              "compress");
        out.resize(size);
        return out;
    }
    size_t size = BrotliEncoderMaxCompressedSize(static_cast<size_t>(input.size()));
    QByteArray out(static_cast<qsizetype>(size), '\0');
    check(BrotliEncoderCompress(4, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, input.size(),
                                reinterpret_cast<const uint8_t *>(input.constData()), &size,
                                reinterpret_cast<uint8_t *>(out.data())),
          "brotli encode");
    out.resize(static_cast<qsizetype>(size));
    return out;
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QString error;
    QVector<GoimPacket> packets;
    GoimCodec codec;
    const auto first = GoimCodec::frame(5, 9, "{\"cmd\":\"DANMU_MSG\"}");
    for (const char byte : first)
        check(codec.feed(QByteArray(1, byte), packets, error), "incremental");
    check(packets.size() == 1 && packets[0].sequence == 9, "incremental output");
    for (int version : {2, 3}) {
        codec.reset();
        packets.clear();
        const auto outer =
            GoimCodec::frame(5, 1, compressed(first + first, version), static_cast<quint16>(version));
        check(codec.feed(outer, packets, error) && packets.size() == 2 && packets[1].body == packets[0].body,
              "compressed batch");
        packets.clear();
        auto nested = first;
        for (int i = 0; i < 6; ++i)
            nested = GoimCodec::frame(5, 1, compressed(nested, version), static_cast<quint16>(version));
        check(!codec.feed(nested, packets, error), "nesting limit");
        auto bomb = GoimCodec::frame(5, 1, compressed(QByteArray(GoimCodec::MaxBody + 1, 'x'), version),
                                     static_cast<quint16>(version));
        check(!codec.feed(bomb, packets, error), "expansion limit");
        check(!codec.feed(GoimCodec::frame(5, 1, "broken", static_cast<quint16>(version)), packets, error),
              "corrupt compression");
    }
    packets.clear();
    auto malformed = first;
    malformed[5] = 15;
    check(!codec.feed(malformed, packets, error), "invalid header");
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost), "listen");
    GoimClient client;
    int observedHeartbeat = 0;
    QObject::connect(&client, &GoimClient::transportPacketObserved, [&](quint32 op) {
        if (op == 3)
            ++observedHeartbeat;
    });
    int events = 0, refresh = 0;
    bool normalizedGift = false;
    QObject::connect(&client, &GoimClient::events, [&](const QJsonArray &a) { events += a.size();
                         for (const auto &item : a) {
                             const auto event = item.toObject();
                             if (event.value("cmd") == "SEND_GIFT" && event.value("data").toObject().value("giftName") == "test") normalizedGift = true;
                         } });
    QObject::connect(&client, &GoimClient::tokenRefreshRequested, [&] { ++refresh; });
    client.setContext(123, 456, "synthetic-token", "127.0.0.1", server.serverPort(), "synthetic-buvid");
    check(waitFor([&] { return server.hasPendingConnections(); }), "worker TCP connection");
    auto *peer = server.nextPendingConnection();
    GoimCodec received;
    QVector<GoimPacket> sent;
    auto readSent = [&] {
        if (peer->bytesAvailable())
            check(received.feed(peer->readAll(), sent, error), "client packet decode");
    };
    check(waitFor([&] {
              readSent();
              return sent.size() >= 1;
          }),
          "login");
    const auto login = QJsonDocument::fromJson(sent[0].body).object();
    const auto uuid = login.value("queue_uuid").toString();
    check(sent[0].operation == 7 && sent[0].version == 1 && sent[0].sequence == 1 &&
              login.value("protover").toInt() == 3 && !uuid.isEmpty(),
          "auth protocol fields");
    peer->write(GoimCodec::frame(5, 1, "{\"cmd\":\"WARNING\",\"msg\":\"before auth\",\"roomid\":456}"));
    peer->write(GoimCodec::frame(8, 1, "{\"code\":0}"));
    peer->write(GoimCodec::frame(3, 1, QByteArray::fromHex("0000002a")));
    peer->write(GoimCodec::frame(5, 2, "{\"cmd\":\"TEST\",\"data\":{\"id\":\"not-an-ack\"}}"));
    peer->write(GoimCodec::frame(
        5, 3, "{\"cmd\":\"WARNING\",\"roomid\":456,\"msg\":\"after auth\","
              "\"p_is_ack\":true,\"msg_id\":\"synthetic-id\",\"p_msg_type\":1}"));
    check(waitFor([&] {
              readSent();
              return sent.size() >= 2 && events == 2;
          }),
          "events and conditional ACK");
    check(observedHeartbeat == 1, "opaque heartbeat observed");
    check(sent.size() == 2 && sent[1].operation == 24 && sent[1].sequence == 2 &&
              QJsonDocument::fromJson(sent[1].body).object().value("msg_id").toString() == "synthetic-id",
          "exact ACK");
    check(waitFor(
              [&] {
                  readSent();
                  return sent.size() >= 3;
              },
              24000),
          "heartbeat after integer-second gap greater than twenty");
    check(sent[2].operation == 2 && sent[2].body.isEmpty() && sent[2].sequence == 3,
          "empty heartbeat and sequence");
    const auto giftPayload = QByteArray::fromHex("58015210080112047465737418024204676f6c64");
    QJsonObject giftEvent{{"cmd", "SEND_GIFT_V2"}, {"data", QJsonObject{{"pb", QString::fromLatin1(giftPayload.toBase64())}}}};
    peer->write(GoimCodec::frame(5, 4, QJsonDocument(giftEvent).toJson(QJsonDocument::Compact)));
    check(waitFor([&] { return normalizedGift; }), "worker normalizes Gift V2 before UI delivery");
    peer->disconnectFromHost();
    check(waitFor([&] { return server.hasPendingConnections(); }), "reconnect");
    delete peer;
    peer = server.nextPendingConnection();
    received.reset();
    sent.clear();
    check(waitFor([&] {
              readSent();
              return !sent.isEmpty();
          }),
          "reconnect login");
    check(sent[0].sequence == 1 &&
              QJsonDocument::fromJson(sent[0].body).object().value("queue_uuid").toString() == uuid,
          "stable context UUID and reset sequence");
    peer->write(GoimCodec::frame(8, 1, "{\"code\":-101}"));
    check(waitFor([&] { return refresh == 1; }), "token expiry");
    delete peer;
    peer = nullptr;
    client.setContext(123, 456, "refreshed-token", "127.0.0.1", server.serverPort(), "synthetic-buvid");
    check(waitFor([&] { return server.hasPendingConnections(); }), "refresh reconnect");
    peer = server.nextPendingConnection();
    received.reset();
    sent.clear();
    check(waitFor([&] {
              readSent();
              return !sent.isEmpty();
          }),
          "refresh login");
    check(QJsonDocument::fromJson(sent[0].body).object().value("build").toInt() == 10819,
          "binary build auth");
    peer->write(GoimCodec::frame(8, 1, "{\"code\":0}") + GoimCodec::frame(5, 1, "{\"cmd\":\"STALE\"}"));
    peer->flush();
    QThread::msleep(100); // Worker queues delivery while facade's event loop is paused.
    const int beforeStop = events;
    client.stop();
    check(!waitFor([&] { return events != beforeStop; }, 300), "stop drops already queued events");
    delete peer;
    QTcpServer unused;
    check(unused.listen(QHostAddress::LocalHost), "reserve refusal port");
    const auto refusedPort = unused.serverPort();
    unused.close();
    int endpointFailures = 0;
    QObject::connect(&client, &GoimClient::endpointFailed, [&] { ++endpointFailures; });
    client.setContext(123, 456, "refreshed-token", "127.0.0.1", refusedPort, "synthetic-buvid");
    check(waitFor([&] { return endpointFailures == 1; }, 15000), "three refusals exhaust endpoint");
    check(!waitFor([&] { return endpointFailures > 1; }, 1200), "exhausted endpoint stops retries");
    client.setContext(123, 456, "refreshed-token", "127.0.0.1", server.serverPort(), "synthetic-buvid");
    check(waitFor([&] { return server.hasPendingConnections(); }), "next endpoint connects");
    peer = server.nextPendingConnection();
    received.reset();
    sent.clear();
    check(waitFor([&] { readSent(); return !sent.isEmpty(); }), "next endpoint login");
    check(sent[0].sequence == 1 &&
              QJsonDocument::fromJson(sent[0].body).object().value("queue_uuid").toString() == uuid,
          "endpoint failover preserves queue UUID");
    client.stop();
    delete peer;
    std::puts("Goim protocol and local-server checks passed");
    return 0;
}
