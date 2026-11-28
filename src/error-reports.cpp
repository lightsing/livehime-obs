#include "error-reports.hpp"
#include "beta-diagnostics.hpp"
#include "diagnostic-archive.hpp"
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
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
    for (auto *label : {message_, result_}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(label);
    }
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

void ErrorReports::report(const PluginIssue &issue) {
    const auto now = timer_.elapsed();
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) {
        return entry.operation == issue.operation && entry.codes == issue.codes && entry.reason == issue.exportReason;
    });
    const bool repeated = found != entries_.end() && now - found->lastOccurrence < 120000;
    Entry entry = found == entries_.end() ? Entry{issue.operation, issue.codes, issue.exportReason} : *found;
    if (found != entries_.end()) entries_.erase(found);
    ++entry.count;
    entry.lastSeen = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    entry.lastOccurrence = now;
    if (entries_.size() >= 32) entries_.removeFirst();
    entries_.append(entry);
#if LIVEHIME_BETA_BUILD
    QJsonObject fields{{"operation", entry.operation}, {"codes", entry.codes}};
    if (!entry.reason.isEmpty()) fields.insert("reason", entry.reason);
    diagnostics::record("plugin.error", fields);
#endif
    if (!isVisible() && repeated) return;
    message_->setText(issue.operation + QStringLiteral("：") + issue.detail.left(1500) +
        (entry.count > 1 ? QStringLiteral("\n已发生 %1 次").arg(entry.count) : QString{}));
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
