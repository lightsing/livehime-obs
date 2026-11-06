#include "session-store.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>

#include <iostream>

namespace {
bool check(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory"))
        return 1;
    livehime::SessionStore store(directory.path());
    QString error;
    if (!check(store.loadCookies(&error).isEmpty() && error.isEmpty(), "new store is empty"))
        return 1;
    const auto device = store.deviceId(&error);
    if (!check(!device.isEmpty() && error.isEmpty(), "create device id")) {
        std::cerr << error.toUtf8().constData() << '\n';
        return 1;
    }
    const auto restoredDevice = store.deviceId(&error);
    if (!check(device == restoredDevice && error.isEmpty(), "stable device id")) {
        std::cerr << error.toUtf8().constData() << '\n';
        return 1;
    }

    QNetworkCookie session(QByteArrayLiteral("SESSDATA"), QByteArrayLiteral("synthetic-secret-for-offline-test"));
    session.setDomain(QStringLiteral(".bilibili.com"));
    session.setPath(QStringLiteral("/"));
    session.setSecure(true);
    session.setHttpOnly(true);
    session.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
    session.setExpirationDate(QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() + 3600, QTimeZone::UTC));
    QNetworkCookie csrf(QByteArrayLiteral("bili_jct"), QByteArrayLiteral("synthetic-csrf"));
    csrf.setDomain(QStringLiteral("api.live.bilibili.com"));
    csrf.setPath(QStringLiteral("/xlive"));
    csrf.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
    QNetworkCookie expired = session;
    expired.setName(QByteArrayLiteral("expired"));
    expired.setExpirationDate(QDateTime::currentDateTimeUtc().addSecs(-1));
    QNetworkCookie unrelated = session;
    unrelated.setDomain(QStringLiteral("bilibili.com.evil.example"));
    QNetworkCookie suffixConfusion = session;
    suffixConfusion.setDomain(QStringLiteral("notbilibili.com"));
    if (!check(store.saveCookies({session, csrf, expired, unrelated, suffixConfusion}, &error) && error.isEmpty(), "save encrypted session"))
        return 1;

    QFile file(QDir(directory.path()).filePath(QStringLiteral("session.dpapi")));
    if (!check(file.open(QIODevice::ReadOnly), "open encrypted session"))
        return 1;
    auto encrypted = file.readAll();
    file.close();
    if (!check(!encrypted.isEmpty() && !encrypted.contains(session.value()) && !encrypted.contains("SESSDATA"), "no plaintext cookie file"))
        return 1;
    const auto loaded = store.loadCookies(&error);
    if (!check(error.isEmpty() && loaded.size() == 2 && loaded[0] == session && loaded[1] == csrf,
               "DPAPI roundtrip retains attributes and filters expired/foreign cookies"))
        return 1;

    encrypted[encrypted.size() / 2] = static_cast<char>(encrypted.at(encrypted.size() / 2) ^ 0x40);
    if (!check(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(encrypted) == encrypted.size(), "write corruption fixture"))
        return 1;
    file.close();
    if (!check(store.loadCookies(&error).isEmpty() && !error.isEmpty(), "corrupted DPAPI data rejected"))
        return 1;
    if (!check(store.clear(&error) && error.isEmpty() && !file.exists(), "logout removes session"))
        return 1;
    if (!check(device == store.deviceId(), "logout preserves independent device identity"))
        return 1;
    if (!check(store.saveCookies({session}) && store.saveCookies({}) && !file.exists(), "empty session removes old saved credentials"))
        return 1;
    std::cout << "SessionStore offline checks passed\n";
    return 0;
}
