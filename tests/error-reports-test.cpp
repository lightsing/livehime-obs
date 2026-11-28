#include "beta-diagnostics.hpp"
#include "diagnostic-archive.hpp"
#include "error-reports.hpp"
#include <QApplication>
#include <QDir>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>
#include <cstdio>
#include <cstdlib>

using namespace livehime;
namespace {
void check(bool ok, const char *name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
QByteArray tar(const QStringList &args) {
    QProcess process;
    process.start("tar.exe", args);
    check(process.waitForFinished(10000) && process.exitCode() == 0, "standard ZIP reader");
    return process.readAllStandardOutput();
}
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary dir");
    const QDir dir(temporary.path());
    diagnostics::start(dir.path());
    check(dir.mkpath("diagnostics"), "create fixture log dir");
    QFile sentinel(dir.filePath("diagnostics/session.ini"));
    check(sentinel.open(QIODevice::WriteOnly), "fixture session");
    sentinel.write("PRIVATE_COOKIE"); sentinel.close();
    QWidget main;
    ErrorReports reports("33.0.1", &main);
    const PluginIssue first = PluginIssue::api(QStringLiteral("开播"),
        ApiError{ApiError::Kind::Api, "PRIVATE_MESSAGE", -123});
    reports.report(first);
    check(reports.isVisible() && !reports.isModal(), "opens without blocking OBS");
    reports.reject();
    reports.report(first);
    check(!reports.isVisible(), "continuous repeat is counted silently");
    QDialog login(&main);
    login.open();
    reports.report({QStringLiteral("连接弹幕"), "PRIVATE_TOKEN"});
    check(reports.isVisible(), "different failure alerts");
    check(reports.windowHandle()->transientParent() == login.windowHandle(), "export accessible above a modal dialog");
    login.reject();
    reports.report(PluginIssue::local(QStringLiteral("选择封面"), QStringLiteral("无法读取此图片。")));
    reports.chooseDestination();
    auto *picker = reports.findChild<QFileDialog *>();
    check(picker, "error dialog save action");
    picker->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    reports.chooseDestination();
    picker = reports.findChild<QFileDialog *>();
    check(picker, "can retry after cancellation");
    const QString path = dir.filePath(QStringLiteral("诊断 包.zip"));
    picker->setDirectory(dir);
    auto *filename = picker->findChild<QLineEdit *>("fileNameEdit");
    auto *buttons = picker->findChild<QDialogButtonBox *>();
    check(filename && buttons && buttons->button(QDialogButtonBox::Save), "save dialog controls");
    filename->setText(QFileInfo(path).fileName());
    buttons->button(QDialogButtonBox::Save)->click();
    QElapsedTimer timeout;
    timeout.start();
    while (!QFile::exists(path) && timeout.elapsed() < 8000) {
        app.processEvents();
        QThread::msleep(5);
    }
    check(QFile::exists(path), "export from dialog");
    const auto files = tar({"-tf", path}).trimmed().replace("\r", "").split('\n');
    check(files.contains("report.json") && files.size() == (LIVEHIME_BETA_BUILD ? 2 : 1), "only intended archive members");
    const auto summary = tar({"-xOf", path, "report.json"});
    const auto report = QJsonDocument::fromJson(summary).object();
    const auto errors = report["errors"].toArray();
    check(report["plugin_version"] == LIVEHIME_OBS_VERSION && report["obs_version"] == "33.0.1", "runtime versions");
    check(report["channel"] == (LIVEHIME_BETA_BUILD ? "beta" : "stable") && errors.size() == 3, "channel and error history");
    const auto row = errors.first().toObject();
    check(row["count"] == 2 && row["codes"].toObject()["api"] == -123 && !row["last_seen_utc"].toString().isEmpty(), "counts and codes");
    check(!summary.contains("PRIVATE") && !files.contains("session.ini"), "private text and session excluded");
    check(errors.last().toObject()["reason"] == QStringLiteral("无法读取此图片。"), "local failure reason is actionable");
#if LIVEHIME_BETA_BUILD
    const auto log = tar({"-xOf", path, "beta-0.jsonl"});
    check(files.contains("beta-0.jsonl") && log.contains("startup") && log.contains("plugin.error")
          && !log.contains("PRIVATE"), "beta includes only shareable diagnostics");
    QFile oversized(dir.filePath("diagnostics/beta-0.jsonl"));
    check(oversized.open(QIODevice::WriteOnly) && oversized.resize(1024 * 1024 + 1), "oversized log fixture");
    oversized.close();
    const auto partial = dir.filePath("partial.zip");
    check(diagnostics::exportArchive(partial, report).isEmpty(), "unavailable logs do not block export");
    check(tar({"-xOf", partial, "report.json"}).contains("log_warning"), "incomplete log set explained");
#endif
    check(!diagnostics::exportArchive(dir.filePath("missing/fail.zip"), {}).isEmpty(), "unwritable destination reported");
    return 0;
}
