#include "session-store.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <QUuid>
#include <Windows.h>
#include <dpapi.h>

#include <utility>

namespace livehime {
namespace {
constexpr auto sessionName = "session.dpapi";
constexpr qint64 maxFileSize = 1024 * 1024;
const QByteArray entropy("livehime-obs/session/v1");

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool allowedCookie(const QNetworkCookie &cookie)
{
    QString domain = cookie.domain().toLower();
    if (domain.startsWith('.'))
        domain.remove(0, 1);
    return (domain == QStringLiteral("bilibili.com") ||
            domain.endsWith(QStringLiteral(".bilibili.com"))) &&
           !cookie.name().isEmpty() && cookie.name().size() <= 4096 &&
           cookie.value().size() <= 32768 &&
           (cookie.isSessionCookie() || cookie.expirationDate() > QDateTime::currentDateTimeUtc());
}

bool writeAtomic(const QString &path, const QByteArray &data, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        setError(error, QStringLiteral("无法创建插件会话目录。"));
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setError(error, QStringLiteral("无法保存插件会话：%1").arg(file.errorString()));
        return false;
    }
    return true;
}

QByteArray protect(const QByteArray &plain, QString *error)
{
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(entropy.constData()))};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"livehime-obs session", &extra, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        setError(error, QStringLiteral("Windows 无法加密会话（错误 %1）。").arg(GetLastError()));
        return {};
    }
    const QByteArray result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return result;
}

QByteArray unprotect(const QByteArray &encrypted, QString *error)
{
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(entropy.constData()))};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &extra, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        setError(error, QStringLiteral("无法解密保存的会话，请重新扫码登录。"));
        return {};
    }
    const QByteArray result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
} // namespace

SessionStore::SessionStore(QString storageDirectory) : directory_(std::move(storageDirectory)) {}

QList<QNetworkCookie> SessionStore::loadCookies(QString *error) const
{
    setError(error, {});
    QFile file(QDir(directory_).filePath(QString::fromLatin1(sessionName)));
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maxFileSize) {
        setError(error, QStringLiteral("保存的会话无法读取或文件大小无效。"));
        return {};
    }
    QByteArray plain = unprotect(file.readAll(), error);
    if (plain.isEmpty())
        return {};
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(plain, &parseError);
    SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
    plain.clear();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("version")).toInt() != 1 ||
        !document.object().value(QStringLiteral("cookies")).isArray()) {
        setError(error, QStringLiteral("保存的会话格式无效，请重新扫码登录。"));
        return {};
    }
    const auto array = document.object().value(QStringLiteral("cookies")).toArray();
    if (array.size() > 512) {
        setError(error, QStringLiteral("保存的会话包含过多 Cookie。"));
        return {};
    }
    QList<QNetworkCookie> result;
    for (const auto &item : array) {
        if (!item.isObject())
            continue;
        const auto entry = item.toObject();
        QNetworkCookie cookie(QByteArray::fromBase64(entry.value(QStringLiteral("name")).toString().toLatin1()),
                              QByteArray::fromBase64(entry.value(QStringLiteral("value")).toString().toLatin1()));
        cookie.setDomain(entry.value(QStringLiteral("domain")).toString());
        cookie.setPath(entry.value(QStringLiteral("path")).toString());
        cookie.setSecure(entry.value(QStringLiteral("secure")).toBool());
        cookie.setHttpOnly(entry.value(QStringLiteral("httpOnly")).toBool());
        const int sameSite = entry.value(QStringLiteral("sameSite")).toInt(0);
        if (sameSite >= 0 && sameSite <= 3)
            cookie.setSameSitePolicy(static_cast<QNetworkCookie::SameSite>(sameSite));
        if (entry.contains(QStringLiteral("expires"))) {
            bool valid = false;
            const qint64 expiry = entry.value(QStringLiteral("expires")).toString().toLongLong(&valid);
            if (!valid)
                continue;
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(expiry, QTimeZone::UTC));
        }
        if (allowedCookie(cookie))
            result.append(cookie);
    }
    return result;
}

bool SessionStore::saveCookies(const QList<QNetworkCookie> &cookies, QString *error) const
{
    setError(error, {});
    QJsonArray array;
    for (const auto &cookie : cookies) {
        if (!allowedCookie(cookie))
            continue;
        QJsonObject entry{{QStringLiteral("name"), QString::fromLatin1(cookie.name().toBase64())},
                          {QStringLiteral("value"), QString::fromLatin1(cookie.value().toBase64())},
                          {QStringLiteral("domain"), cookie.domain()},
                          {QStringLiteral("path"), cookie.path()},
                          {QStringLiteral("secure"), cookie.isSecure()},
                          {QStringLiteral("httpOnly"), cookie.isHttpOnly()},
                          {QStringLiteral("sameSite"), static_cast<int>(cookie.sameSitePolicy())}};
        if (!cookie.isSessionCookie())
            entry.insert(QStringLiteral("expires"), QString::number(cookie.expirationDate().toSecsSinceEpoch()));
        array.append(entry);
    }
    if (array.isEmpty())
        return clear(error);
    QByteArray plain = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                               {QStringLiteral("cookies"), array}}).toJson(QJsonDocument::Compact);
    if (array.size() > 512 || plain.size() > maxFileSize / 2) {
        setError(error, QStringLiteral("会话内容过大，未保存。"));
        SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
        return false;
    }
    const QByteArray encrypted = protect(plain, error);
    SecureZeroMemory(plain.data(), static_cast<SIZE_T>(plain.size()));
    plain.clear();
    return !encrypted.isEmpty() &&
           writeAtomic(QDir(directory_).filePath(QString::fromLatin1(sessionName)), encrypted, error);
}

bool SessionStore::clear(QString *error) const
{
    setError(error, {});
    QFile file(QDir(directory_).filePath(QString::fromLatin1(sessionName)));
    if (!file.exists() || file.remove())
        return true;
    setError(error, QStringLiteral("无法删除保存的会话：%1").arg(file.errorString()));
    return false;
}

QString SessionStore::deviceId(QString *error) const
{
    setError(error, {});
    const QString path = QDir(directory_).filePath(QStringLiteral("device-id"));
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 256) {
            setError(error, QStringLiteral("无法读取插件设备标识。"));
            return {};
        }
        const auto existing = QString::fromLatin1(file.readAll()).trimmed();
        const QRegularExpression format(QStringLiteral("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}[0-9]+user$"));
        if (format.match(existing).hasMatch())
            return existing;
        setError(error, QStringLiteral("插件设备标识格式无效。"));
        return {};
    }
    const auto generated = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper() +
                           QString::number(QCoreApplication::applicationPid()) + QStringLiteral("user");
    return writeAtomic(path, generated.toLatin1(), error) ? generated : QString();
}

QString SessionStore::browserDataDirectory() const
{
    return QDir(directory_).filePath(QStringLiteral("login-browser"));
}

} // namespace livehime
