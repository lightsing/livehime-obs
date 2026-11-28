#include "error-reports.hpp"
#include "beta-diagnostics.hpp"
#include "diagnostic-archive.hpp"
#include "github-issue.hpp"
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <memory>

namespace livehime {
ErrorReports::ErrorReports(QString obsVersion, QWidget *parent) : QDialog(parent), obsVersion_(std::move(obsVersion)) {
    timer_.start();
    setWindowTitle(QStringLiteral("B站直播 · 错误与诊断"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setAttribute(Qt::WA_QuitOnClose, false);
    auto *layout = new QVBoxLayout(this);
    message_ = new QLabel(this);
    result_ = new QLabel(this);
    issueStatus_ = new QLabel(this);
    for (auto *label : {message_, result_, issueStatus_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
    issueStatus_->hide();
    layout->addWidget(createIssueButton(this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    save_ = buttons->addButton(QStringLiteral("导出诊断包…"), QDialogButtonBox::ActionRole);
    folder_ = buttons->addButton(QStringLiteral("打开所在文件夹"), QDialogButtonBox::ActionRole);
    folder_->hide();
    connect(save_, &QPushButton::clicked, this, &ErrorReports::chooseDestination);
    connect(folder_, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(lastExport_).absolutePath()));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(450, sizeHint().height());
}

ErrorReports::~ErrorReports() {
    if (worker_) worker_->wait();
}

QPushButton *ErrorReports::createIssueButton(QWidget *parent) {
    auto *button = new QPushButton(parent);
    button->setObjectName("createGitHubIssue");
    button->setAutoDefault(false);
    const auto render = [this, button] {
        const Entry *entry = entries_.isEmpty() ? nullptr : &entries_.last();
        button->setEnabled(!submitting_);
        if (submitting_) button->setText(QStringLiteral("正在提交…"));
        else if (entry && !entry->createdIssue.isEmpty())
            button->setText(QStringLiteral("查看 Issue #%1").arg(entry->createdIssue.fileName()));
        else if (!entry || entry->useBrowser) button->setText(QStringLiteral("在 GitHub 填写 Issue…"));
        else button->setText(QStringLiteral("创建 GitHub Issue"));
    };
    button->setToolTip(QStringLiteral("向公开仓库 lightsing/livehime-obs 反馈。已登录 GitHub CLI 时直接提交错误摘要，其他情况在浏览器中填写。"));
    connect(this, &ErrorReports::issueActionChanged, button, render);
    connect(button, &QPushButton::clicked, this, &ErrorReports::createIssue);
    render();
    return button;
}

void ErrorReports::openFeedbackPage(const QUrl &url, const QString &status) {
    if (QDesktopServices::openUrl(url)) {
        issueStatus_->setText(status);
    } else {
        issueStatus_->setText(QStringLiteral("无法打开浏览器，请复制此链接打开：\n") + url.toString(QUrl::FullyEncoded));
    }
    issueStatus_->show();
    show();
}

void ErrorReports::createIssue() {
    if (submitting_) return;
    const Entry *entry = entries_.isEmpty() ? nullptr : &entries_.last();
    if (entry && !entry->createdIssue.isEmpty()) {
        openFeedbackPage(entry->createdIssue, QStringLiteral("已创建的 Issue 可继续补充复现步骤或附上诊断包。"));
        return;
    }
    GitHubIssueDraft draft;
    draft.title = entry ? QStringLiteral("%1失败").arg(entry->operation) : QStringLiteral("插件问题反馈");
    draft.body = QStringLiteral("插件 %1（%2）\nOBS %3 · Qt %4\n系统 %5（%6）\n")
        .arg(QStringLiteral(LIVEHIME_OBS_VERSION), LIVEHIME_BETA_BUILD ? QStringLiteral("测试版") : QStringLiteral("正式版"),
             obsVersion_, QString::fromLatin1(qVersion()), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture());
    if (entry) {
        draft.body += QStringLiteral("\n操作：%1\n最近发生：%2\n累计次数：%3\n")
            .arg(entry->operation, entry->lastSeen).arg(entry->count);
        if (!entry->reason.isEmpty()) draft.body += entry->reason.left(160) + '\n';
        if (!entry->codes.isEmpty())
            draft.body += QStringLiteral("错误码：`%1`\n").arg(QString::fromUtf8(QJsonDocument(entry->codes).toJson(QJsonDocument::Compact)));
    }
    if (!entry || entry->useBrowser) {
        draft.body += QStringLiteral("\n复现步骤：\n");
        openFeedbackPage(draft.browserUrl(), QStringLiteral("请在 GitHub 中补充复现步骤并提交。可将导出的诊断包拖入正文。"));
        return;
    }
    const auto id = entry->id;
    submitting_ = true;
    emit issueActionChanged();
    issueStatus_->setText(QStringLiteral("正在向 GitHub 提交错误摘要…"));
    issueStatus_->show();
    show();
    submitGitHubIssue(draft, &submissionContext_, [this, id, draft](IssueSubmission outcome, const QUrl &url) {
        submitting_ = false;
        const auto original = std::find_if(entries_.begin(), entries_.end(),
                                           [id](const Entry &candidate) { return candidate.id == id; });
        if (original != entries_.end()) {
            original->createdIssue = url;
            original->useBrowser = outcome != IssueSubmission::Created;
        }
        emit issueActionChanged();
        if (outcome == IssueSubmission::Created) {
            openFeedbackPage(url, QStringLiteral("已创建 Issue #%1「%2」。可在评论中补充复现步骤或附上诊断包。").arg(url.fileName(), draft.title));
        } else if (outcome == IssueSubmission::BrowserRequired) {
            openFeedbackPage(draft.browserUrl(), QStringLiteral("请在 GitHub 中确认并提交。可将导出的诊断包拖入正文。"));
        } else {
            openFeedbackPage(QUrl(QStringLiteral("https://github.com/lightsing/livehime-obs/issues?q=is%3Aissue+author%3A%40me")),
                             QStringLiteral("提交结果未确认，请先查看自己的 Issue 以免重复提交。"));
        }
    });
}

void ErrorReports::report(const PluginIssue &issue) {
    const auto now = timer_.elapsed();
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) {
        return entry.operation == issue.operation && entry.codes == issue.codes && entry.reason == issue.exportReason;
    });
    const bool repeated = found != entries_.end() && now - found->lastOccurrence < 120000;
    const bool sameAsDisplayed = found != entries_.end() && found->id == entries_.last().id;
    Entry entry = found == entries_.end() ? Entry{issue.operation, issue.codes, issue.exportReason} : *found;
    if (found != entries_.end()) entries_.erase(found);
    else entry.id = ++nextId_;
    ++entry.count;
    entry.lastSeen = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    entry.lastOccurrence = now;
    if (entries_.size() >= 32) entries_.removeFirst();
    entries_.append(entry);
    emit issueActionChanged();
#if LIVEHIME_BETA_BUILD
    QJsonObject fields{{"operation", entry.operation}, {"codes", entry.codes}};
    if (!entry.reason.isEmpty()) fields.insert("reason", entry.reason);
    diagnostics::record("plugin.error", fields);
#endif
    message_->setText(issue.operation + QStringLiteral("：") + issue.detail.left(1500) +
        (entry.count > 1 ? QStringLiteral("\n已发生 %1 次").arg(entry.count) : QString{}));
    if (!sameAsDisplayed && !submitting_) issueStatus_->hide();
    if (!isVisible() && repeated) return;
    if (!isVisible()) {
        if (!worker_) {
            result_->clear();
            folder_->hide();
        }
        QWidget *modal = QApplication::activeModalWidget();
        show();
        // Keep the export action reachable while an OBS or login dialog blocks the main window.
        if (windowHandle() && parentWidget()) {
            auto *owner = modal ? modal->window() : parentWidget()->window();
            windowHandle()->setTransientParent(owner->windowHandle());
        }
        raise();
    }
}

void ErrorReports::chooseDestination() {
    if (worker_) { show(); return; }
    if (chooser_) { chooser_->raise(); return; }
    const auto name = QStringLiteral("livehime-obs-%1-%2.zip").arg(QStringLiteral(LIVEHIME_OBS_VERSION),
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    auto *dialog = new QFileDialog(isVisible() ? static_cast<QWidget *>(this) : parentWidget(),
        QStringLiteral("保存诊断包"), QDir(dir).filePath(name), QStringLiteral("ZIP 压缩包 (*.zip)"));
    chooser_ = dialog;
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix("zip");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(this, &QObject::destroyed, dialog, &QObject::deleteLater);
    connect(dialog, &QFileDialog::fileSelected, this, &ErrorReports::exportTo);
    dialog->open();
}

void ErrorReports::exportTo(const QString &path) {
    if (path.isEmpty() || worker_) return;
    QJsonArray errors;
    for (const auto &entry : entries_) {
        QJsonObject row{{"operation", entry.operation}, {"codes", entry.codes}, {"last_seen_utc", entry.lastSeen},
                        {"count", entry.count}};
        if (!entry.reason.isEmpty()) row.insert("reason", entry.reason);
        errors.append(row);
    }
    const QJsonObject report{{"plugin_version", LIVEHIME_OBS_VERSION}, {"channel", LIVEHIME_BETA_BUILD ? "beta" : "stable"},
                             {"obs_version", obsVersion_}, {"qt_version", qVersion()}, {"os", QSysInfo::prettyProductName()},
                             {"architecture", QSysInfo::currentCpuArchitecture()}, {"errors", errors},
                             {"exported_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    save_->setEnabled(false);
    folder_->hide();
    result_->setText(QStringLiteral("正在打包诊断记录…"));
    show();
    auto result = std::make_shared<QString>();
    worker_ = QThread::create([path, report, result] { *result = diagnostics::exportArchive(path, report); });
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, [this, path, result] {
        worker_->wait();
        worker_->deleteLater();
        worker_ = nullptr;
        save_->setEnabled(true);
        if (result->isEmpty()) {
            lastExport_ = path;
            result_->setText(QStringLiteral("已保存，可将此文件发送给开发者。\n") + QDir::toNativeSeparators(path));
            folder_->show();
        } else {
            result_->setText(*result);
        }
    }, Qt::QueuedConnection);
    worker_->start();
}
}
