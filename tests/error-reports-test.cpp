#include "beta-diagnostics.hpp"
#include "diagnostic-archive.hpp"
#include "error-reports.hpp"
#include <QApplication>
#include <QDesktopServices>
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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>
#include <QUrlQuery>
#include <cstdio>
#include <cstdlib>

using namespace livehime;
class FeedbackBrowser final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> pages;
public slots:
    void open(const QUrl &url) { pages.append(url); }
};

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

// A copy of this test executable acts as gh.exe, including reading the body from stdin.
int fakeGitHubCli(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments().mid(1);
    if (args.size() != 8 || args.first(4) != QStringList{"issue", "create", "--repo", "github.com/lightsing/livehime-obs"}
        || args[4] != "--title" || args.last(2) != QStringList{"--body-file", "-"} ||
        qgetenv("GH_PROMPT_DISABLED") != "1") return 2;
    QFile input;
    QFile capture(qEnvironmentVariable("LIVEHIME_TEST_ISSUE_BODY"));
    if (!input.open(stdin, QIODevice::ReadOnly) || !capture.open(QIODevice::WriteOnly | QIODevice::Append)) return 2;
    capture.write(QJsonDocument(QJsonObject{{"title", args[5]}, {"body", QString::fromUtf8(input.readAll())}})
                      .toJson(QJsonDocument::Compact) + '\n');
    const auto mode = qgetenv("LIVEHIME_TEST_GH_MODE");
    if (mode == "login") return 4;
    if (mode == "network") return 1;
    std::puts("https://github.com/lightsing/livehime-obs/issues/42");
    return 0;
}

void checkIssueFeedback(const QDir &dir) {
    FeedbackBrowser browser;
    QDesktopServices::setUrlHandler("https", &browser, "open");
    QWidget panel;
    ErrorReports reports("33.0.1", &panel);
    auto *panelButton = reports.createIssueButton(&panel);
    auto *popupButton = reports.findChild<QPushButton *>("createGitHubIssue");
    check(popupButton, "issue button in error popup");
    const QByteArray path = qgetenv("PATH");
    const auto bin = dir.filePath("cli");
    check(dir.mkpath("cli"), "fake CLI directory");
    qputenv("PATH", bin.toLocal8Bit());
    check(QStandardPaths::findExecutable("gh.exe").isEmpty(), "test isolates missing CLI");
    panelButton->click();
    check(browser.pages.size() == 1 && browser.pages.last().path() == "/lightsing/livehime-obs/issues/new",
          "feedback without error opens general issue editor");
    auto issue = PluginIssue::api(QStringLiteral("开播"), ApiError{ApiError::Kind::Api, "PRIVATE_COOKIE", -123});
    reports.report(issue);
    popupButton->click();
    check(browser.pages.size() == 2, "missing CLI opens error draft");
    const QUrlQuery form(browser.pages.last());
    const auto body = form.queryItemValue("body", QUrl::FullyDecoded);
    check(form.queryItemValue("title", QUrl::FullyDecoded).contains(issue.operation) && body.contains("-123")
          && body.contains(LIVEHIME_OBS_VERSION) && body.contains("33.0.1")
          && body.contains(LIVEHIME_BETA_BUILD ? QStringLiteral("测试版") : QStringLiteral("正式版"))
          && !body.contains("PRIVATE"), "draft has shareable context and channel");

    const auto mock = QDir(bin).filePath("gh.exe");
    check(QFile::copy(QCoreApplication::applicationFilePath(), mock), "install offline CLI fixture");
    qputenv("PATH", bin.toLocal8Bit() + ';' + path);
    check(QFileInfo(QStandardPaths::findExecutable("gh.exe")) == QFileInfo(mock), "only fixture may submit");
    const auto capturePath = dir.filePath("github.jsonl");
    qputenv("LIVEHIME_TEST_ISSUE_BODY", capturePath.toLocal8Bit());
    qunsetenv("LIVEHIME_TEST_GH_MODE");
    auto waitForSubmission = [&] {
        QElapsedTimer timeout;
        timeout.start();
        while (!popupButton->isEnabled() && timeout.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        check(panelButton->isEnabled() && popupButton->isEnabled(), "submission finished");
    };
    const auto local = PluginIssue::local(QStringLiteral("选择封面"), QStringLiteral("读取失败 & + 100%"));
    reports.report(local);
    panelButton->click();
    check(!popupButton->isEnabled() && !panelButton->isEnabled(), "both entry points exclude double submit");
    popupButton->click();
    reports.report(issue); // An unrelated error becomes current while the CLI is running.
    waitForSubmission();
    const QUrl created("https://github.com/lightsing/livehime-obs/issues/42");
    check(browser.pages.last() == created, "success opens created issue");
    reports.report(local);
    check(popupButton->text().contains("#42"), "result associated with original error");
    popupButton->click();
    QFile capture(capturePath);
    check(capture.open(QIODevice::ReadOnly), "capture CLI submission");
    const auto lines = capture.readAll().trimmed().split('\n');
    const auto submitted = QJsonDocument::fromJson(lines.first()).object();
    check(lines.size() == 1 && browser.pages.last() == created && submitted["title"].toString().contains(local.operation)
          && submitted["body"].toString().contains(local.exportReason), "existing issue reused, UTF-8 body preserved");
    capture.close();

    qputenv("LIVEHIME_TEST_GH_MODE", "login");
    reports.report(PluginIssue::local(QStringLiteral("上传封面"), local.exportReason));
    popupButton->click();
    waitForSubmission();
    check(browser.pages.last().path().endsWith("/issues/new") &&
          QUrlQuery(browser.pages.last()).queryItemValue("body", QUrl::FullyDecoded).contains(local.exportReason),
          "signed-out CLI falls back to correctly escaped browser draft");
    qputenv("LIVEHIME_TEST_GH_MODE", "network");
    reports.report({QStringLiteral("连接弹幕"), "PRIVATE_TOKEN"});
    popupButton->click();
    waitForSubmission();
    check(browser.pages.last().path().endsWith("/issues"), "ambiguous submission opens issue list");
    popupButton->click();
    check(browser.pages.last().path().endsWith("/issues/new") && !browser.pages.last().toEncoded().contains("PRIVATE"),
          "explicit follow-up edits a draft instead of resubmitting");
    qputenv("PATH", path);
    QDesktopServices::unsetUrlHandler("https");
}
}

int main(int argc, char **argv) {
    if (argc > 1) return fakeGitHubCli(argc, argv);
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
    checkIssueFeedback(dir);
    return 0;
}
#include "error-reports-test.moc"
