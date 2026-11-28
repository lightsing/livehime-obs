#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QString>

namespace livehime {

// Cookie content is protected by Windows DPAPI for the current Windows user.
// The directory must belong to this plugin, not to an existing browser/login app.
class SessionStore final {
public:
    explicit SessionStore(QString storageDirectory);
    const QString &directory() const { return directory_; }
    QList<QNetworkCookie> loadCookies(QString *error = nullptr) const;
    bool saveCookies(const QList<QNetworkCookie> &cookies, QString *error = nullptr) const;
    bool clear(QString *error = nullptr) const;
    QString deviceId(QString *error = nullptr) const;
    QString browserDataDirectory() const;

private:
    QString directory_;
};

} // namespace livehime
