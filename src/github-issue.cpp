#include "github-issue.hpp"
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <chrono>

namespace livehime {
namespace {
constexpr char repository[] = "github.com/lightsing/livehime-obs";
QByteArray issuePrefix() { return QByteArray("https://") + repository + "/issues/"; }
}

QUrl GitHubIssueDraft::browserUrl() const {
    return QUrl::fromEncoded(issuePrefix() + "new?title=" + QUrl::toPercentEncoding(title) +
                             "&body=" + QUrl::toPercentEncoding(body));
}

void submitGitHubIssue(const GitHubIssueDraft &draft, QObject *owner,
                       std::function<void(IssueSubmission, const QUrl &)> finished) {
    const auto cli = QStandardPaths::findExecutable(QStringLiteral("gh.exe"));
    if (cli.isEmpty()) {
        finished(IssueSubmission::BrowserRequired, {});
        return;
    }
    auto *process = new QProcess(owner);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("GH_PROMPT_DISABLED", "1");
    env.insert("GH_NO_UPDATE_NOTIFIER", "1");
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(QDir::tempPath());
    process->setStandardErrorFile(QProcess::nullDevice());
    QObject::connect(process, &QProcess::errorOccurred, owner, [process, finished](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            finished(IssueSubmission::BrowserRequired, {});
        }
    });
    QObject::connect(process, &QProcess::finished, owner, [process, finished](int code, QProcess::ExitStatus exit) {
        const QByteArray output = process->readAllStandardOutput().trimmed();
        process->deleteLater();
        if (exit == QProcess::NormalExit && code == 4) {
            finished(IssueSubmission::BrowserRequired, {});
            return;
        }
        const auto prefix = issuePrefix();
        const auto number = output.mid(prefix.size());
        if (exit == QProcess::NormalExit && code == 0 && output.startsWith(prefix) &&
            !number.isEmpty() && number.front() != '0' &&
            std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            finished(IssueSubmission::Created, QUrl::fromEncoded(output));
            return;
        }
        // The server may have created the issue before the reply was lost.
        finished(IssueSubmission::Unconfirmed, {});
    });
    QTimer::singleShot(std::chrono::seconds{30}, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    QObject::connect(process, &QProcess::started, process, [process, body = draft.body.toUtf8()] {
        process->write(body);
        process->closeWriteChannel();
    });
    process->start(cli, {"issue", "create", "--repo", repository, "--title", draft.title, "--body-file", "-"});
}
}
