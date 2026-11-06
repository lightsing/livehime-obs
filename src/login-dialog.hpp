#pragma once

#include <QDialog>
#include <QList>
#include <QNetworkCookie>
#include <memory>

namespace livehime {

// A separate, InPrivate WebView2 profile, never a user's existing browser profile.
// cookiesReady is a candidate session: callers must verify it with BiliClient nav.
class LoginDialog final : public QDialog {
    Q_OBJECT
public:
    explicit LoginDialog(QString browserDataDir, QString nativeBuvid,
                         QWidget *parent = nullptr);
    ~LoginDialog() override;

signals:
    void cookiesReady(const QList<QNetworkCookie> &cookies);
    void loginFailed(const QString &message);

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void done(int result) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void initialize();
    void initializeController();
    void configureBrowser();
    void resizeBrowser();
    void collectCookies();
    void fail(const QString &message);
    void closeBrowser();
};

} // namespace livehime
