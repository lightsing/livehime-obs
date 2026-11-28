#include "plugin-updater.hpp"
#include "update-package.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <cstring>
#include <zlib.h>

using namespace livehime;
namespace {
void expect(bool pass, const char *what) {
    if (!pass) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
QByteArray sha(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha512); }
QByteArray manifest(const QString &version) {
    return QJsonDocument(QJsonObject{{"name", QLatin1String(update::PackageName)}, {"version", version}})
        .toJson(QJsonDocument::Compact);
}
QByteArray image() {
    QByteArray pe(512, '\0');
    pe.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(128, pe.data()+60);
    pe.replace(128, 4, QByteArray("PE\0\0",4));
    qToLittleEndian<quint16>(0x14c, pe.data()+132);
    qToLittleEndian<quint16>(224, pe.data()+148);
    qToLittleEndian<quint16>(2, pe.data()+150);
    qToLittleEndian<quint16>(0x10b, pe.data()+152);
    return pe;
}
struct Member { QByteArray name, body; char type = '0'; QByteArray link, prefix; };
QByteArray tar(const QList<Member> &items) {
    QByteArray out;
    for (const auto &item : items) {
        QByteArray h(512, '\0');
        h.replace(0, item.name.size(), item.name);
        h.replace(124, 11, QByteArray::number(item.body.size(), 8).rightJustified(11, '0'));
        h.replace(148, 8, "        ");
        h[156] = item.type;
        h.replace(157, item.link.size(), item.link);
        h.replace(257, 5, "ustar");
        h.replace(345, item.prefix.size(), item.prefix);
        unsigned sum = 0;
        for (char c : h) sum += static_cast<unsigned char>(c);
        h.replace(148, 6, QByteArray::number(sum, 8).rightJustified(6, '0'));
        h[154] = '\0';
        out += h + item.body;
        out.append((512-item.body.size()%512)%512, '\0');
    }
    return out + QByteArray(1024, '\0');
}
QByteArray gzip(const QByteArray &input) {
    z_stream stream{};
    expect(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS+16, 8, Z_DEFAULT_STRATEGY) == Z_OK,
          "gzip init");
    QByteArray result(deflateBound(&stream, static_cast<uLong>(input.size())), '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(result.data());
    stream.avail_out = static_cast<uInt>(result.size());
    expect(deflate(&stream, Z_FINISH) == Z_STREAM_END, "gzip complete");
    result.resize(static_cast<qsizetype>(stream.total_out));
    deflateEnd(&stream);
    return result;
}
QByteArray read(const QString &path) {
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "read file");
    return file.readAll();
}
QByteArray metadata(const QString &version, const QByteArray &digest, const QString &address) {
    const auto name = QLatin1String(update::PackageName);
    QJsonObject dist{{"tarball",address}, {"integrity", QString::fromLatin1("sha512-" + digest.toBase64())}};
    QJsonObject item{{"name", name}, {"version", version}, {"dist", dist}};
    return QJsonDocument(QJsonObject{{"name",name}, {"dist-tags",QJsonObject{{"latest",version}}},
                                     {"versions",QJsonObject{{version,item}}}}).toJson(QJsonDocument::Compact);
}

struct Response { QByteArray body; int status = 200; QUrl redirect; qint64 declared = -1; };
class Reply final : public QNetworkReply {
  public:
    Reply(const QNetworkRequest &req, Response response, QObject *parent)
        : QNetworkReply(parent), bytes_(std::move(response.body)) {
        setUrl(req.url()); setRequest(req); setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (response.redirect.isValid())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        if (response.declared >= 0)
            setHeader(QNetworkRequest::ContentLengthHeader, response.declared);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0,this,[this] {
            emit metaDataChanged();
            if (!isFinished()) emit readyRead();
            if (!isFinished()) { setFinished(true); emit finished(); }
        });
    }
    void abort() override {
        if (!isFinished()) {
            setError(OperationCanceledError, "aborted");
            setFinished(true);
            emit finished();
        }
    }
    qint64 bytesAvailable() const override { return bytes_.size() - at_ + QIODevice::bytesAvailable(); }
  protected:
    qint64 readData(char *dst,qint64 max) override {
        const auto count = qMin<qint64>(max,bytes_.size()-at_);
        if (!count) return -1;
        std::memcpy(dst,bytes_.constData()+at_,size_t(count));
        at_ += count; return count;
    }
  private:
    QByteArray bytes_;
    qsizetype at_ = 0;
};
class OfflineUpdater final : public PluginUpdater {
  public:
    using PluginUpdater::PluginUpdater;
    QList<Response> queued;
    QList<QUrl> seen;
    void wait(State expected) {
        QElapsedTimer timer;
        timer.start();
        while ((state() == State::Checking || state() == State::Downloading) && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        if (state() != expected)
            std::fprintf(stderr, "Unexpected state %d: %s\n", int(state()), qPrintable(status()));
        expect(state() == expected && queued.isEmpty(), "updater reaches expected state");
    }
  protected:
    QNetworkReply *get(const QNetworkRequest &req) override {
        expect(!queued.isEmpty(),"unexpected network request");
        expect(req.attribute(QNetworkRequest::CookieLoadControlAttribute) == QNetworkRequest::Manual &&
              req.attribute(QNetworkRequest::CookieSaveControlAttribute) == QNetworkRequest::Manual &&
              req.attribute(QNetworkRequest::RedirectPolicyAttribute) == QNetworkRequest::ManualRedirectPolicy &&
              !req.hasRawHeader("Cookie") && !req.hasRawHeader("Authorization"), "isolated update request");
        seen.append(req.url());
        return new Reply(req,queued.takeFirst(),this);
    }
};
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    expect(dir.isValid(),"temporary cache");
    if (argc == 4) {
        const QString version = QString::fromLocal8Bit(argv[1]);
        const auto data = read(QString::fromLocal8Bit(argv[2]));
        const auto installer = update::cacheInstaller(data, {version, {}, sha(data)}, dir.path());
        expect(installer.error.isEmpty() && read(installer.path) == read(QString::fromLocal8Bit(argv[3])),
              "release tarball contains original installer");
        return 0;
    }
    using State = PluginUpdater::State;
    const QString mirror = "https://registry.npmmirror.com/fixture.tgz";
    const QString origin = "https://registry.npmjs.org/fixture.tgz";
    const QByteArray pe = image();
    const Member setup{"package/livehime-obs-setup.exe",pe}, json{"package/package.json",manifest("1.10.0")};
    const auto normal = tar({json,setup}), archive = gzip(normal), digest = sha(archive);
    const auto mirrored = metadata("1.10.0",digest,mirror), official = metadata("1.10.0",digest,origin);
    expect(update::newer("1.10.0","1.9.9") && !update::newer("1.9.9","1.10.0"),"numeric semver");
    for (auto invalid : {"01.2.3","1.0","1.0.0-beta","1.0.0+test","9007199254740992.0.0","1.0.0\n"})
        expect(!update::stableVersion(invalid),"only canonical stable semver");
    for (auto invalid : {"http://registry.npmjs.org/x","https://registry.npmjs.org.example/x",
                         "https://x@registry.npmjs.org/x","https://registry.npmjs.org:81/x","file:///update"})
        expect(!update::trustedUrl(QUrl(invalid)),"reject untrusted origin");
    expect(!update::readMetadata(metadata("1.10.0","bad",origin)), "strict SHA512 metadata");
    auto invalidArchive = [&](QByteArray bad) {
        const auto result = update::cacheInstaller(bad, {"1.10.0", {}, sha(bad)}, dir.path());
        expect(!result.error.isEmpty() && result.path.isEmpty(), "invalid archive rejected");
    };
    for (const Member &bad : QList<Member>{{"package/../outside",pe}, {"package/livehime-obs-setup.exe",pe,'2',"../outside"},
                            {"package/livehime-obs-setup.exe",pe,'1',"package/package.json"},
                            {"package/livehime-obs-setup.exe",pe,'0',{},"elsewhere"},
                            {"package/livehime-obs-setup.exe","text"}})
        invalidArchive(gzip(tar({json,bad})));
    invalidArchive(gzip(tar({json, json, setup})));
    invalidArchive(gzip(tar({json, setup, setup})));
    invalidArchive(gzip(tar({json})));
    invalidArchive(gzip(tar({Member{"package/package.json",manifest("1.9.0")},setup})));
    invalidArchive(gzip(tar({Member{"package/package.json",R"({"name":"wrong","version":"1.10.0"})"},setup})));
    invalidArchive(gzip(normal.chopped(512)));
    invalidArchive(gzip(normal + QByteArray(512,'a')));
    auto brokenHeader = normal; brokenHeader[0] = 'Q';
    invalidArchive(gzip(brokenHeader));
    invalidArchive(archive.chopped(1));
    invalidArchive(archive + archive);
    invalidArchive(gzip(QByteArray(update::MaxArchiveBytes + 256 * 1024, '\0')));
    expect(QDir(dir.path()).entryList(QDir::Files).isEmpty(), "failed verification writes nothing");

    const auto registry = [&](const QString &stable, const QString &beta) {
        auto root = QJsonDocument::fromJson(metadata(stable, digest, origin)).object();
        auto versions = root["versions"].toObject();
        versions.insert(beta, QJsonDocument::fromJson(metadata(beta, digest, origin)).object()["versions"].toObject().value(beta));
        root.insert("versions", versions);
        root.insert("dist-tags", QJsonObject{{"latest", stable}, {"beta", beta}});
        return QJsonDocument(root).toJson();
    };
    const auto channels = registry("1.10.0", "1.11.0-beta.10");
    using update::Channel;
    expect(update::newer("1.11.0-beta.10","1.11.0-beta.2") && update::newer("1.11.0","1.11.0-beta.10"), "beta precedence");
    for (const auto invalid : {"1.11.0-beta.01", "1.11.0-beta.", "1.11.0-rc.1", "1.11.0-beta.10\n"})
        expect(!update::validVersion(invalid), "reject invalid prerelease");
    expect(update::readMetadata(channels)->version == "1.10.0", "stable ignores beta");
    expect(update::readMetadata(registry("1.11.0", "1.11.0-beta.10"), Channel::Beta)->version == "1.11.0" &&
           update::readMetadata(official, Channel::Beta)->version == "1.10.0", "promotion and absent beta tag");
    expect(!update::readMetadata(metadata("1.11.0-beta.10", digest, origin)), "stable rejects mislabelled latest");
    OfflineUpdater subscriber("1.11.0-beta.2", dir.path());
    subscriber.setChannel(Channel::Beta);
    subscriber.queued = {Response{channels}, Response{channels}};
    subscriber.check(); subscriber.wait(State::Available);
    expect(subscriber.version() == "1.11.0-beta.10", "beta update selected");
    subscriber.queued = {Response{archive}};
    subscriber.download();
    subscriber.setChannel(Channel::Stable);
    subscriber.queued = {Response{channels}, Response{channels}};
    subscriber.check(); subscriber.wait(State::Available);
    expect(subscriber.version() == "1.10.0" && subscriber.installerPath().isEmpty(), "opt out during download offers lower stable");
    subscriber.queued = {Response{channels}};
    subscriber.check();
    subscriber.setChannel(Channel::Beta);
    QCoreApplication::processEvents();
    expect(subscriber.state() == State::Idle && subscriber.version().isEmpty(), "switch cancels pending check");

    OfflineUpdater updater("1.9.0",dir.path());
    updater.queued = {Response{metadata("1.9.0",digest,mirror)}, Response{official}};
    updater.check(); updater.check(); updater.wait(State::Available);
    expect(updater.version()=="1.10.0" && updater.seen[0].host()=="registry.npmmirror.com" &&
          updater.seen[1].host()=="registry.npmjs.org","stale mirror rechecked with official registry");
    updater.queued = {Response{archive}};
    updater.download(); updater.download(); updater.wait(State::Ready);
    expect(read(updater.installerPath()) == pe &&
          updater.installerSha256() == QCryptographicHash::hash(pe,QCryptographicHash::Sha256).toHex(),
          "verified installer and handoff digest");

    const auto cachedArchive = update::archivePath({"1.10.0",{},digest},dir.path());
    expect(read(cachedArchive) == archive,"original authenticated archive retained");
    QFile actual(updater.installerPath()); expect(actual.open(QIODevice::WriteOnly),"mutate test cache");
    actual.write("corrupt"); actual.close();
    OfflineUpdater resumed("1.9.0",dir.path());
    resumed.queued = {Response{mirrored}, Response{official}};
    resumed.check(); resumed.wait(State::Ready);
    expect(resumed.seen.size()==2 && read(resumed.installerPath())==pe,"trusted cache revalidated at restart");

    QFile cached(cachedArchive); expect(cached.open(QIODevice::WriteOnly),"mutate tar cache");
    cached.write("bad"); cached.close();
    OfflineUpdater recover("1.9.0",dir.path());
    recover.queued = {Response{mirrored}, Response{official}};
    recover.check(); recover.wait(State::Available);
    recover.queued = {Response{"bad-tar"}, Response{archive}};
    recover.download(); recover.wait(State::Ready);
    expect(recover.seen[2].toString()==mirror && recover.seen[3].toString()==origin,
          "corrupt mirror download recovered using verified official bytes");
    QFile sentinel(QDir(dir.path()).filePath("keep.txt"));
    expect(sentinel.open(QIODevice::WriteOnly),"create unrelated test file"); sentinel.close();
    OfflineUpdater installed("1.10.0",dir.path());
    expect(sentinel.exists() && !QFileInfo::exists(cachedArchive) && !QFileInfo::exists(resumed.installerPath()),
          "remove only installed-version cache files");
    installed.queued = {Response{mirrored},Response{official}};
    installed.check(); installed.wait(State::UpToDate);
    OfflineUpdater untrusted("1.0.0",dir.path());
    QStringList updateFailures;
    QObject::connect(&untrusted, &PluginUpdater::updateFailed, &untrusted,
                     [&updateFailures](const QString &detail) { updateFailures.append(detail); });
    untrusted.queued = {Response{mirrored}, Response{{},302,QUrl(mirror)}};
    untrusted.check(); untrusted.wait(State::Error);
    expect(untrusted.seen.size()==2,"unsigned mirror cannot impersonate official metadata through redirect");
    untrusted.queued = {Response{{},500},Response{official}};
    untrusted.check(); untrusted.wait(State::Available);
    untrusted.queued = {Response{{},302,QUrl("https://example.org/x")},
                        Response{{},200,{},update::MaxArchiveBytes+1}};
    untrusted.download(); untrusted.wait(State::Available);
    expect(untrusted.installerPath().isEmpty(),"failed download remains retryable without installer");
    expect(updateFailures.size() == 2 && updateFailures.last() == untrusted.status(),
           "metadata and exhausted download failures are each reported once");
    untrusted.queued = {Response{archive}};
    untrusted.download(); untrusted.wait(State::Ready);
    untrusted.setChannel(Channel::Beta);
    expect(untrusted.installerPath().isEmpty() && untrusted.state() == State::Idle, "channel switch discards ready installer");
    std::puts("Offline npm resolution, verification, fallback and cache checks passed");
}
