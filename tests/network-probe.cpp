// Explicit diagnostic only; not a CTest case. Uses no stored cookies or credentials.
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QTextStream>
#include <QTimer>
#include <Windows.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() > 1)
        QCoreApplication::setLibraryPaths({args.at(1)});
    QTextStream out(stdout);
    wchar_t runtimePath[32768]{};
    const auto runtimeLength = GetModuleFileNameW(GetModuleHandleW(L"Qt6Network.dll"), runtimePath, 32768);
    out << "qt=" << qVersion() << '\n'
        << "qtNetworkRuntime=" << QString::fromWCharArray(runtimePath, static_cast<int>(runtimeLength)) << '\n'
        << "libraryPaths=" << QCoreApplication::libraryPaths().join(';') << '\n'
        << "availableBackends=" << QSslSocket::availableBackends().join(',') << '\n'
        << "activeBackend=" << QSslSocket::activeBackend() << '\n'
        << "supportsSsl=" << (QSslSocket::supportsSsl() ? "true" : "false") << '\n';
    out.flush();
    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/nav")));
    request.setRawHeader("User-Agent", "livehime-obs-tls-probe/0.1.0");
    request.setTransferTimeout(15000);
    auto *reply = network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &app, [&] {
        const auto result = QJsonDocument::fromJson(reply->readAll()).object();
        out << "httpStatus=" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() << '\n'
            << "networkError=" << static_cast<int>(reply->error()) << '\n'
            << "networkErrorText=" << (reply->error() == QNetworkReply::NoError ? QStringLiteral("None") : reply->errorString()) << '\n'
            << "businessCode=" << result.value(QStringLiteral("code")).toInt(-999999) << '\n'
            << "isLogin=" << (result.value(QStringLiteral("data")).toObject().value(QStringLiteral("isLogin")).toBool() ? "true" : "false") << '\n';
        out.flush();
        app.exit(reply->error() == QNetworkReply::NoError ? 0 : 1);
    });
    QTimer::singleShot(20000, &app, [&] { out << "probeTimeout=true\n"; out.flush(); app.exit(2); });
    return app.exec();
}
