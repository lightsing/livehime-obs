#pragma once
#include "plugin-issue.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

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
private:
    void exportTo(const QString &path);
    struct Entry {
        QString operation;
        QJsonObject codes;
        QString reason;
        QString lastSeen;
        int count = 0;
        qint64 lastOccurrence = 0;
    };
    QString obsVersion_, lastExport_;
    QList<Entry> entries_;
    QElapsedTimer timer_;
    QLabel *message_, *result_;
    QPushButton *save_, *folder_;
    QPointer<QFileDialog> chooser_;
    QThread *worker_ = nullptr;
};
}
