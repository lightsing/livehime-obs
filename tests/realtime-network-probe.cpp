// Explicitly invoked read-only probe. Uses only this plugin's DPAPI session.
#include "bili-client.hpp"
#include "goim-client.hpp"
#include "session-store.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>
using namespace livehime;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4) {
        std::cerr << "Usage: realtime-network-probe plugin-session-directory private-qt-plugin-directory "
                     "summary-json\n";
        return 2;
    }
    QCoreApplication::addLibraryPath(QString::fromLocal8Bit(argv[2]));
    SessionStore store(QString::fromLocal8Bit(argv[1]));
    QString error;
    const auto cookies = store.loadCookies(&error);
    if (!error.isEmpty() || cookies.isEmpty()) {
        std::cerr << "Plugin session unavailable.\n";
        return 3;
    }
    BiliClient client;
    const auto device = store.deviceId(&error);
    if (!error.isEmpty())
        return 3;
    client.setDeviceId(device);
    client.setCookies(cookies);
    GoimClient goim;
    QJsonArray results;
    bool authenticated = false, finished = false;
    int failures = 0, heartbeatReplies = 0;
    QJsonObject config;
    QList<QPair<QString, protocol::Request>> jobs;
    int index = 0;
    qint64 uid = 0, room = 0;
    auto finish = [&] {
        if (finished)
            return;
        finished = true;
        goim.stop();
        QFile output(QString::fromLocal8Bit(argv[3]));
        if (!output.open(QIODevice::WriteOnly)) {
            app.exit(4);
            return;
        }
        output.write(QJsonDocument(
                         QJsonObject{{"scope", "production Qt client read-only probe; plugin-owned session"},
                                     {"access_key_empty", true},
                                     {"requests", results},
                                     {"goim_authenticated", authenticated},
                                     {"heartbeat_reply_count", heartbeatReplies},
                                     {"finished_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}})
                         .toJson());
        app.exit(failures ? 1 : 0);
    };
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setInterval(180000);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
        ++failures;
        finish();
    });
    deadline.start();
    QObject::connect(&goim, &GoimClient::statusChanged, &app, [&](const QString &status) {
        if (status == QStringLiteral("弹幕已连接") && !authenticated) {
            authenticated = true;
            QTimer::singleShot(65000, &app, [&] {
                if (heartbeatReplies < 2)
                    ++failures;
                finish();
            });
        }
    });
    QObject::connect(&goim, &GoimClient::transportPacketObserved, &app, [&](quint32 op) {
        if (op == 3)
            ++heartbeatReplies;
    });
    std::function<void()> next;
    next = [&] {
        if (index == jobs.size()) {
            const auto hosts = config.value("ip_list").toArray();
            if (hosts.isEmpty() || config.value("token").toString().isEmpty()) {
                ++failures;
                finish();
                return;
            }
            const auto host = hosts.first().toObject();
            goim.setContext(uid, room, config.value("token").toString(), host.value("host").toString(),
                            quint16(host.value("port").toInt()), device);
            QTimer::singleShot(20000, &app, [&] {
                if (!authenticated) {
                    ++failures;
                    finish();
                }
            });
            return;
        }
        const auto job = jobs[index++];
        client.fetchRealtime(job.second, [&, name = job.first](Result<QJsonObject> response) {
            QJsonObject summary{{"operation", name}};
            if (const auto *e = std::get_if<ApiError>(&response)) {
                summary["success"] = false;
                summary["http_status"] = e->httpStatus;
                summary["api_code"] = e->code;
                summary["error_kind"] = int(e->kind);
                ++failures;
            } else {
                const auto data = std::get<QJsonObject>(response);
                summary["success"] = true;
                summary["api_code"] = 0;
                if (name == "danmu_info") {
                    config = data;
                    summary["address_count"] = data.value("ip_list").toArray().size();
                } else if (name.startsWith("guard:")) {
                    summary["top3_count"] = data.value("top3").toArray().size();
                    summary["list_count"] = data.value("list").toArray().size();
                    summary["page"] = data.value("info").toObject().value("now");
                } else
                    summary["item_count"] = data.value("item").toArray().size();
            }
            results.append(summary);
            QTimer::singleShot(100, &app, next);
        });
    };
    client.validateSession([&](Result<UserInfo> response) {
        if (!std::holds_alternative<UserInfo>(response)) {
            ++failures;
            finish();
            return;
        }
        uid = std::get<UserInfo>(response).uid;
        client.fetchRoom(uid, [&](Result<RoomInfo> response) {
            if (!std::holds_alternative<RoomInfo>(response)) {
                ++failures;
                finish();
                return;
            }
            room = std::get<RoomInfo>(response).roomId;
            const auto now = QDateTime::currentSecsSinceEpoch();
            jobs.append({"danmu_info", protocol::danmuInfo(room, now)});
            const QList<QPair<QString, QString>> modes{
                {"online_rank", "contribution_rank"}, {"online_rank", "entry_time_rank"},
                {"intimacy_rank", "intimacy_rank"},   {"daily_rank", "today_rank"},
                {"daily_rank", "yesterday_rank"},     {"weekly_rank", "current_week_rank"},
                {"weekly_rank", "last_week_rank"},    {"monthly_rank", "current_month_rank"},
                {"monthly_rank", "last_month_rank"}};
            for (const auto &mode : modes)
                jobs.append({"audience:" + mode.first + "/" + mode.second,
                             protocol::contributionRank(uid, room, mode.first, mode.second, now)});
            for (int typ : {4, 3, 5, 1})
                jobs.append(
                    {QStringLiteral("guard:%1").arg(typ), protocol::guardRank(uid, room, typ, 1, now)});
            next();
        });
    });
    return app.exec();
}
