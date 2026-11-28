#include "beta-diagnostics.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir config;
    if (!config.isValid()) return 1;
    livehime::diagnostics::start(config.path());
    QDir dir(config.filePath("diagnostics"));
#if LIVEHIME_BETA_BUILD
    QFile log(dir.filePath("beta-0.jsonl"));
    if (!log.open(QIODevice::ReadOnly)) return 1;
    const auto first = QJsonDocument::fromJson(log.readLine()).object();
    if (first["event"] != "startup" || first["version"] != LIVEHIME_OBS_VERSION || first["time"].toString().isEmpty()) return 1;
    log.close();
    QFile sentinel(dir.filePath("keep.txt"));
    if (!sentinel.open(QIODevice::WriteOnly)) return 1;
    sentinel.close();
    for (int i = 0; i < 6; ++i) {
        if (!log.open(QIODevice::ReadWrite) || !log.resize(1024 * 1024)) return 1;
        log.close();
        livehime::diagnostics::record("test", {{"index", i}});
    }
    if (!sentinel.exists() || dir.entryList({"*.jsonl"}, QDir::Files).size() != 4 || !log.open(QIODevice::ReadOnly)) return 1;
    return QJsonDocument::fromJson(log.readLine()).object()["index"].toInt(-1) == 5 ? 0 : 1;
#else
    return dir.exists() ? 1 : 0;
#endif
}
