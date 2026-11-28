#pragma once
#include "plugin-issue.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>
#include <QUrl>

class QFileDialog;
class QLabel;
class QPushButton;
class QThread;
namespace livehime {
class ErrorReports final : public QDialog {
    Q_OBJECT
public:
    ErrorReports(QString obsVersion, QWidget *parent);
    ~ErrorReports() override;
    void report(const PluginIssue &issue);
    void chooseDestination();
    QPushButton *createIssueButton(QWidget *parent);
signals:
    void issueActionChanged();
private:
    void createIssue();
    void openFeedbackPage(const QUrl &url, const QString &status);
    void exportTo(const QString &path);
    struct Entry {
        QString operation;
        QJsonObject codes;
        QString reason;
        QString lastSeen;
        int count = 0;
        qint64 lastOccurrence = 0;
        quint64 id = 0;
        QUrl createdIssue;
        bool useBrowser = false;
    };
    QString obsVersion_, lastExport_;
    QList<Entry> entries_;
    QElapsedTimer timer_;
    QLabel *message_, *result_, *issueStatus_;
    QPushButton *save_, *folder_;
    QPointer<QFileDialog> chooser_;
    QThread *worker_ = nullptr;
    quint64 nextId_ = 0;
    bool submitting_ = false;
    QObject submissionContext_; // Disconnect CLI callbacks before destroying the dialog state.
};
}
