#pragma once

#include "update-package.hpp"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QThread>
#include <functional>

namespace livehime {
class PluginUpdater : public QObject {
    Q_OBJECT
  public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Error };
    Q_ENUM(State)
    PluginUpdater(QString currentVersion, QString cacheDirectory, QObject *parent = nullptr);
    ~PluginUpdater() override;
    void check();
    void setChannel(update::Channel channel);
    void download();
    State state() const { return state_; }
    QString status() const { return status_; }
    QString version() const { return release_ ? release_->version : QString{}; }
    QString installerPath() const { return installer_.path; }
    QByteArray installerSha256() const { return installer_.sha256; }
  signals:
    void changed();

  protected:
    virtual QNetworkReply *get(const QNetworkRequest &request);

  private:
    using Received = std::function<void(QByteArray, QString)>;
    void request(const QUrl &url, qsizetype limit, bool officialMetadata, int redirects, Received received);
    void selectRelease(const std::optional<update::Release> &mirror, bool signedMirror,
                       const std::optional<update::Release> &official);
    void cancelPending();
    void tryDownload(int index);
    void verify(QByteArray archive, int index);
    void downloadFailed(int index, const QString &error);
    void setState(State state, const QString &status);

    const QString currentVersion_, cacheDirectory_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QThread *worker_ = nullptr;
    std::optional<update::Release> release_;
    QList<QUrl> urls_;
    update::CachedInstaller installer_;
    State state_ = State::Idle;
    QString status_;
    update::Channel channel_ = update::Channel::Stable;
    quint64 generation_ = 0;
};
} // namespace livehime
