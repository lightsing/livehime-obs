#include "beta-diagnostics.hpp"
#if LIVEHIME_BETA_BUILD
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QMutex>

namespace livehime::diagnostics {
namespace {
QMutex mutex;
QString directory;
constexpr int FileCount = 4;
constexpr qint64 FileBytes = 1024 * 1024;
}

void start(const QString &configDir) {
    {
        QMutexLocker guard(&mutex);
        directory = QDir(configDir).filePath("diagnostics");
        if (!QDir().mkpath(directory)) directory.clear();
    }
    record("startup", {{"version", LIVEHIME_OBS_VERSION}, {"qt", qVersion()}});
}

void record(const QString &event, QJsonObject fields) {
    QMutexLocker guard(&mutex);
    if (directory.isEmpty()) return;
    const QDir dir(directory);
    QLockFile lock(dir.filePath("beta.lock"));
    if (!lock.tryLock(0)) return;
    fields.insert("time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    fields.insert("pid", QCoreApplication::applicationPid());
    fields.insert("event", event);
    const auto line = QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
    const auto path = [&dir](int index) { return dir.filePath(QStringLiteral("beta-%1.jsonl").arg(index)); };
    QFile file(path(0));
    if (file.size() + line.size() > FileBytes) {
        if (QFile::exists(path(FileCount - 1)) && !QFile::remove(path(FileCount - 1))) return;
        for (int i = FileCount - 2; i >= 0; --i)
            if (QFile::exists(path(i)) && !QFile::rename(path(i), path(i + 1))) return;
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(line);
}
}
#endif
