#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

namespace livehime::update {
enum class Channel { Stable, Beta };
inline constexpr char PackageName[] = "@lightsing/livehime-obs-win32-x64";
inline constexpr qsizetype MaxArchiveBytes = 32 * 1024 * 1024;

struct Release {
    QString version;
    QUrl tarball;
    QByteArray sha512;
};

struct CachedInstaller {
    QString path;
    QByteArray sha256;
    QString error;
};

bool stableVersion(const QString &version);
bool validVersion(const QString &version);
bool newer(const QString &candidate, const QString &installed);
bool trustedUrl(const QUrl &url);
std::optional<Release> readMetadata(const QByteArray &json, Channel channel = Channel::Stable);
QString archivePath(const Release &release, const QString &directory);
void cleanInstalledCaches(const QString &installed, const QString &directory);
CachedInstaller cacheInstaller(const QByteArray &archive, const Release &release, const QString &directory,
                               bool keepArchive = true);
} // namespace livehime::update
