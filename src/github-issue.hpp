#pragma once
#include <QString>
#include <QUrl>
#include <functional>

class QObject;
namespace livehime {
struct GitHubIssueDraft {
    QString title;
    QString body;
    QUrl browserUrl() const;
};

enum class IssueSubmission { Created, BrowserRequired, Unconfirmed };
void submitGitHubIssue(const GitHubIssueDraft &draft, QObject *owner,
                       std::function<void(IssueSubmission, const QUrl &)> finished);
}
