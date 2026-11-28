#include "update-package.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#include <array>
#include <limits>
#include <zlib.h>

namespace livehime::update {
namespace {
constexpr qsizetype Block = 512, MaxManifest = 64 * 1024;
constexpr qsizetype MaxExpanded = MaxArchiveBytes + MaxManifest + 64 * 1024;

std::optional<std::array<quint64, 4>> parts(const QString &version) {
    static const QRegularExpression pattern(QStringLiteral(R"(\A(0|[1-9][0-9]{0,15})\.(0|[1-9][0-9]{0,15})\.(0|[1-9][0-9]{0,15})(?:-beta\.(0|[1-9][0-9]{0,15}))?\z)"));
    const auto match = pattern.match(version);
    if (!match.hasMatch())
        return {};
    std::array<quint64, 4> values{};
    for (size_t i = 0; i < values.size(); ++i) {
        if (i == 3 && !match.hasCaptured(4)) {
            values[i] = std::numeric_limits<quint64>::max(); // Stable follows every beta of the same core version.
            break;
        }
        values[i] = match.captured(static_cast<int>(i + 1)).toULongLong();
        if (values[i] > 9007199254740991ULL) // npm semver's maximum safe integer
            return {};
    }
    return values;
}

std::optional<QByteArray> gunzip(const QByteArray &input) {
    if (input.isEmpty() || input.size() > MaxArchiveBytes)
        return {};
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK)
        return {};
    QByteArray output;
    char buffer[64 * 1024];
    int rc;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        rc = inflate(&stream, Z_NO_FLUSH);
        const qsizetype count = sizeof buffer - stream.avail_out;
        if (count > MaxExpanded - output.size())
            break;
        output.append(buffer, count);
        if (rc == Z_STREAM_END && stream.avail_in == 0) {
            inflateEnd(&stream);
            return output;
        }
    } while (rc == Z_OK);
    inflateEnd(&stream);
    return {};
}

bool zeros(QByteArrayView bytes) {
    for (char byte : bytes)
        if (byte != 0)
            return false;
    return true;
}
std::optional<quint64> octal(QByteArrayView bytes) {
    qsizetype p = 0;
    while (p < bytes.size() && bytes[p] == ' ')
        ++p;
    quint64 value = 0;
    const auto start = p;
    while (p < bytes.size() && bytes[p] >= '0' && bytes[p] <= '7')
        value = value * 8 + (bytes[p++] - '0');
    if (p == start)
        return {};
    for (; p < bytes.size(); ++p)
        if (bytes[p] != '\0' && bytes[p] != ' ')
            return {};
    return value;
}
bool peImage(QByteArrayView bytes) {
    if (bytes.size() < 64 || bytes.first(2) != "MZ")
        return false;
    const qsizetype at = qFromLittleEndian<quint32>(bytes.data() + 60);
    if (at < 64 || at > bytes.size() - 26 || bytes.sliced(at, 4) != QByteArrayView("PE\0\0", 4))
        return false;
    const auto *p = bytes.data() + at;
    const auto machine = qFromLittleEndian<quint16>(p + 4);
    const auto optionalSize = qFromLittleEndian<quint16>(p + 20);
    const auto magic = qFromLittleEndian<quint16>(p + 24);
    return (machine == 0x14c || machine == 0x8664) && (qFromLittleEndian<quint16>(p + 22) & 2) &&
           optionalSize >= 2 && optionalSize <= bytes.size() - at - 24 && (magic == 0x10b || magic == 0x20b);
}

std::optional<QByteArrayView> installerInTar(QByteArrayView tar, const QString &version) {
    if (tar.size() % Block)
        return {};
    QByteArrayView installer;
    bool manifest = false;
    for (qsizetype at = 0; at <= tar.size() - Block; ) {
        const auto header = tar.sliced(at, Block);
        if (zeros(header)) {
            if (manifest && !installer.isEmpty() && tar.size() - at >= 2 * Block && zeros(tar.sliced(at)))
                return installer;
            return {};
        }
        const auto checksum = octal(header.sliced(148, 8));
        const auto size = octal(header.sliced(124, 12));
        unsigned sum = 0;
        for (int i = 0; i < Block; ++i)
            sum += i >= 148 && i < 156 ? 32 : static_cast<unsigned char>(header[i]);
        if (!checksum || *checksum != sum || !size || *size > quint64(MaxArchiveBytes) ||
            (header[156] != 0 && header[156] != '0') || !zeros(header.sliced(157, 100)) ||
            !zeros(header.sliced(345, 155)) || header.sliced(257, 5) != "ustar")
            return {};
        const auto fileName = header.first(100);
        const auto end = fileName.indexOf('\0');
        if (end < 0 || !zeros(fileName.sliced(end)))
            return {};
        const auto name = fileName.first(end);
        at += Block;
        const auto length = static_cast<qsizetype>(*size);
        const auto padded = (length + Block - 1) / Block * Block;
        if (padded > tar.size() - at || !zeros(tar.sliced(at + length, padded - length)))
            return {};
        const auto body = tar.sliced(at, length);
        if (name == "package/package.json") {
            if (manifest || length > MaxManifest)
                return {};
            const auto info = QJsonDocument::fromJson(body.toByteArray()).object();
            if (info.value("name").toString() != QLatin1String(PackageName) ||
                info.value("version").toString() != version)
                return {};
            manifest = true;
        } else if (name == "package/livehime-obs-setup.exe") {
            if (!installer.isEmpty() || !peImage(body))
                return {};
            installer = body;
        } else
            return {};
        at += padded;
    }
    return {};
}
} // namespace

bool validVersion(const QString &version) { return parts(version).has_value(); }
bool stableVersion(const QString &version) { return validVersion(version) && !version.contains('-'); }
bool newer(const QString &candidate, const QString &installed) {
    const auto left = parts(candidate), right = parts(installed);
    return left && right && *left > *right;
}
bool trustedUrl(const QUrl &url) {
    const auto host = url.host();
    return url.isValid() && url.scheme() == "https" && url.authority().indexOf('@') < 0 &&
           !url.hasFragment() && (url.port() == -1 || url.port() == 443) &&
           (host == "registry.npmjs.org" || host == "registry.npmmirror.com" || host == "cdn.npmmirror.com");
}
std::optional<Release> readMetadata(const QByteArray &json, Channel channel) {
    const auto root = QJsonDocument::fromJson(json).object();
    const auto tags = root.value("dist-tags").toObject();
    auto version = tags.value("latest").toString();
    if (!stableVersion(version)) version.clear();
    if (channel == Channel::Beta) {
        const auto beta = tags.value("beta").toString();
        if (validVersion(beta) && beta.contains('-') && (version.isEmpty() || newer(beta, version)))
            version = beta;
    }
    const auto item = root.value("versions").toObject().value(version).toObject();
    if (root.value("name").toString() != QLatin1String(PackageName) || version.isEmpty() ||
        item.value("version").toString() != version ||
        (item.contains("name") && item.value("name").toString() != QLatin1String(PackageName)))
        return {};
    const auto dist = item.value("dist").toObject();
    const auto url = QUrl(dist.value("tarball").toString(), QUrl::StrictMode);
    if (!trustedUrl(url))
        return {};
    QByteArray sha512;
    const auto tokens = dist.value("integrity").toString().split(' ', Qt::SkipEmptyParts);
    for (const auto &token : tokens) {
        if (!token.startsWith("sha512-"))
            continue;
        const auto encoded = token.mid(7).toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != 64 || decoded.decoded.toBase64() != encoded ||
            (!sha512.isEmpty() && sha512 != decoded.decoded))
            return {};
        sha512 = decoded.decoded;
    }
    if (sha512.isEmpty())
        return {};
    return Release{version, url, sha512};
}

QString archivePath(const Release &release, const QString &directory) {
    return QDir(directory).filePath(QStringLiteral("livehime-obs-%1-%2.tgz")
        .arg(release.version, QString::fromLatin1(release.sha512.toHex())));
}
void cleanInstalledCaches(const QString &installed, const QString &directory) {
    const QDir cache(directory);
    if (directory.isEmpty() || !cache.isAbsolute() || QFileInfo(directory).isSymLink() || !validVersion(installed))
        return;
    static const QRegularExpression owned(QStringLiteral(
        R"(\Alivehime-obs-([0-9]+\.[0-9]+\.[0-9]+(?:-beta\.[0-9]+)?)-(?:[a-f0-9]{16}-setup\.exe|[a-f0-9]{128}\.tgz)\z)"));
    const auto files = cache.entryInfoList({QStringLiteral("livehime-obs-*")}, QDir::Files | QDir::NoSymLinks);
    for (const auto &file : files) {
        const auto match = owned.match(file.fileName());
        if (match.hasMatch() && (match.captured(1) == installed || newer(installed, match.captured(1))))
            QFile::remove(file.filePath());
    }
}
CachedInstaller cacheInstaller(const QByteArray &archive, const Release &release, const QString &directory,
                               bool keepArchive) {
    if (!validVersion(release.version) || archive.size() > MaxArchiveBytes ||
        QCryptographicHash::hash(archive, QCryptographicHash::Sha512) != release.sha512)
        return {{}, {}, QStringLiteral("安装包完整性校验失败")};
    const auto expanded = gunzip(archive);
    if (!expanded)
        return {{}, {}, QStringLiteral("安装包解压失败或超过大小限制")};
    const auto image = installerInTar(*expanded, release.version);
    if (!image)
        return {{}, {}, QStringLiteral("安装包内容或版本校验失败")};
    const auto digest = QCryptographicHash::hash(*image, QCryptographicHash::Sha256).toHex();
    const auto cache = QDir(directory);
    if (directory.isEmpty() || !cache.isAbsolute() || QFileInfo(directory).isSymLink() ||
        !QDir().mkpath(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return {{}, {}, QStringLiteral("无法创建更新缓存目录")};
    const auto path = cache.filePath(QStringLiteral("livehime-obs-%1-%2-setup.exe")
                                        .arg(release.version, QString::fromLatin1(digest.left(16))));
    QSaveFile exe(path);
    exe.setDirectWriteFallback(false);
    if (QFileInfo(path).isSymLink() || !exe.open(QIODevice::WriteOnly) ||
        exe.write(image->data(), image->size()) != image->size() || !exe.commit())
        return {{}, {}, QStringLiteral("无法保存已验证的更新安装包")};
    if (keepArchive) {
        const auto saved = archivePath(release, directory);
        QSaveFile tar(saved);
        tar.setDirectWriteFallback(false);
        if (!QFileInfo(saved).isSymLink() && tar.open(QIODevice::WriteOnly) && tar.write(archive) == archive.size())
            tar.commit();
    }
    return {path, digest, {}};
}
} // namespace livehime::update
