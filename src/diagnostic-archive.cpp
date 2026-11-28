#include "diagnostic-archive.hpp"
#include "beta-diagnostics.hpp"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <zip.h>
#include <iowin32.h>

namespace livehime::diagnostics {
QString exportArchive(const QString &destination, QJsonObject report) {
    QMap<QString, QByteArray> files;
#if LIVEHIME_BETA_BUILD
    if (const QString error = readLogs(files); !error.isEmpty())
        report.insert("log_warning", error);
#endif
    files.insert("report.json", QJsonDocument(report).toJson());
    QTemporaryDir staging;
    if (!staging.isValid()) return QStringLiteral("无法创建临时目录。");
    const auto path = staging.filePath("report.zip");
    zlib_filefunc64_def io{};
    fill_win32_filefunc64W(&io);
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    const auto zip = zipOpen2_64(native.c_str(), APPEND_STATUS_CREATE, nullptr, &io);
    if (!zip) return QStringLiteral("无法创建临时 ZIP 文件。");
    const auto now = QDateTime::currentDateTime();
    zip_fileinfo info{};
    info.tmz_date = {now.time().second(), now.time().minute(), now.time().hour(),
                     now.date().day(), now.date().month() - 1, now.date().year()};
    bool ok = true;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (zipOpenNewFileInZip(zip, it.key().toLatin1().constData(), &info, nullptr, 0, nullptr, 0, nullptr,
                                Z_DEFLATED, Z_BEST_SPEED) != ZIP_OK) { ok = false; break; }
        const auto &bytes = it.value();
        ok = zipWriteInFileInZip(zip, bytes.constData(), unsigned(bytes.size())) == ZIP_OK;
        if (zipCloseFileInZip(zip) != ZIP_OK) ok = false;
        if (!ok) break;
    }
    if (zipClose(zip, nullptr) != ZIP_OK || !ok) return QStringLiteral("压缩诊断记录失败，请检查可用空间。");
    QFile archive(path);
    QSaveFile output(destination);
    if (!archive.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        return QStringLiteral("无法保存诊断包，请选择可写目录。");
    const auto bytes = archive.readAll();
    if (bytes.isEmpty() || bytes.size() != archive.size() || output.write(bytes) != bytes.size() || !output.commit())
        return QStringLiteral("诊断包写入失败，请检查可用空间或更换目录。");
    return {};
}
}
