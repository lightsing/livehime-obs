#include <Windows.h>
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>

#include "login-dialog.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace livehime {
namespace {
constexpr auto loginUrl = "https://live.bilibili.com/p/html/live-pc-blink/mini-login-v2/";

struct ComString {
    LPWSTR value = nullptr;
    ~ComString() { CoTaskMemFree(value); }
    QString text() const { return value ? QString::fromWCharArray(value) : QString(); }
};

bool allowedUrl(const QUrl &url)
{
    const auto host = url.host().toLower();
    return url.isValid() && url.scheme() == QStringLiteral("https") &&
           (url.port(-1) == -1 || url.port() == 443) && url.userInfo().isEmpty() &&
           (host == QStringLiteral("bilibili.com") || host.endsWith(QStringLiteral(".bilibili.com")));
}

bool loginOrigin(const QUrl &url)
{
    return allowedUrl(url) && url.host() == QStringLiteral("live.bilibili.com") &&
           url.path() == QStringLiteral("/p/html/live-pc-blink/mini-login-v2/");
}

QString bridgeScript()
{
    const QJsonObject environment{{QStringLiteral("appkey"), QStringLiteral("aae92bc66f3edfab")},
                                  {QStringLiteral("device_name"), QStringLiteral("OBS Studio")},
                                  {QStringLiteral("device_platform"), QStringLiteral("Windows")}};
    // No credential/token payload crosses the script bridge. CookieManager reads
    // this private browser's actual cookie store after the official success event.
    return QStringLiteral(R"JS((() => {
      if (location.origin !== 'https://live.bilibili.com' ||
          location.pathname !== '/p/html/live-pc-blink/mini-login-v2/') return;
      window.browser = %1;
      // Native request-header buvid is a separate identity slot. Retain the
      // browser's actual cookie instead of substituting a local device id.
      Object.defineProperty(window.browser, 'buvid3', { get: () => {
        const cookie = document.cookie.split(';').map(v => v.trim())
          .find(v => v.startsWith('buvid3='));
        return cookie ? cookie.slice(7) : '';
      }});
      window.livehime_login = {
        LoginSuccess: () => window.chrome.webview.postMessage('livehime-obs:login-success'),
        SecondaryValidationResult: () => window.chrome.webview.postMessage('livehime-obs:login-success'),
        Cancel: () => window.chrome.webview.postMessage('livehime-obs:cancel'),
        SwitchLogin: () => {}
      };
    })();
    )JS").arg(QString::fromUtf8(QJsonDocument(environment).toJson(QJsonDocument::Compact)));
}

QList<QNetworkCookie> readCookies(ICoreWebView2CookieList *list)
{
    QList<QNetworkCookie> result;
    UINT count = 0;
    if (!list || FAILED(list->get_Count(&count)) || count > 512)
        return result;
    for (UINT index = 0; index < count; ++index) {
        ComPtr<ICoreWebView2Cookie> source;
        if (FAILED(list->GetValueAtIndex(index, &source)))
            continue;
        ComString name, value, domain, path;
        if (FAILED(source->get_Name(&name.value)) || FAILED(source->get_Value(&value.value)) ||
            FAILED(source->get_Domain(&domain.value)) || FAILED(source->get_Path(&path.value)))
            continue;
        QString domainName = domain.text().toLower();
        if (domainName.startsWith('.'))
            domainName.remove(0, 1);
        if (domainName != QStringLiteral("bilibili.com") &&
            !domainName.endsWith(QStringLiteral(".bilibili.com")))
            continue;
        BOOL secure = FALSE, httpOnly = FALSE, session = TRUE;
        double expires = 0;
        COREWEBVIEW2_COOKIE_SAME_SITE_KIND sameSite = COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
        if (FAILED(source->get_IsSecure(&secure)) || FAILED(source->get_IsHttpOnly(&httpOnly)) ||
            FAILED(source->get_IsSession(&session)) || FAILED(source->get_Expires(&expires)))
            continue;
        QNetworkCookie cookie(name.text().toUtf8(), value.text().toUtf8());
        if (cookie.name().isEmpty() || cookie.name().size() > 4096 || cookie.value().size() > 32768)
            continue;
        cookie.setDomain(domain.text());
        cookie.setPath(path.text());
        cookie.setSecure(secure);
        cookie.setHttpOnly(httpOnly);
        if (!session) {
            if (!std::isfinite(expires) || expires <= 0 || expires > 253402300799.0)
                continue;
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(expires), QTimeZone::UTC));
            if (cookie.expirationDate() <= QDateTime::currentDateTimeUtc())
                continue;
        }
        if (SUCCEEDED(source->get_SameSite(&sameSite))) {
            if (sameSite == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX)
                cookie.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
            else if (sameSite == COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT)
                cookie.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
            else
                cookie.setSameSitePolicy(QNetworkCookie::SameSite::None);
        }
        result.append(cookie);
    }
    return result;
}
} // namespace

struct LoginDialog::Impl {
    QString browserDataDir;
    QWidget *surface = nullptr;
    QLabel *status = nullptr;
    QTimer *poll = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> browser;
    ComPtr<ICoreWebView2CookieManager> cookies;
    EventRegistrationToken navigationToken{}, navigationCompletedToken{}, messageToken{}, newWindowToken{}, permissionToken{};
    bool initialized = false;
    bool closed = false;
    bool delivered = false;
    bool reading = false;
    bool comInitialized = false;
};

LoginDialog::LoginDialog(QString browserDataDir, QString nativeBuvid, QWidget *parent)
    : QDialog(parent), impl_(std::make_unique<Impl>())
{
    impl_->browserDataDir = std::move(browserDataDir);
    // Native device id belongs only in native request headers, never cookies.
    Q_UNUSED(nativeBuvid);
    setWindowTitle(QStringLiteral("扫码登录哔哩哔哩"));
    resize(860, 660);
    setMinimumSize(680, 550);
    auto *layout = new QVBoxLayout(this);
    impl_->status = new QLabel(QStringLiteral("请使用哔哩哔哩 App 扫码，并在手机上确认登录。"), this);
    impl_->status->setWordWrap(true);
    layout->addWidget(impl_->status);
    impl_->surface = new QWidget(this);
    impl_->surface->setAttribute(Qt::WA_NativeWindow);
    impl_->surface->setMinimumSize(640, 440);
    impl_->surface->installEventFilter(this);
    layout->addWidget(impl_->surface, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto *reload = buttons->addButton(QStringLiteral("重新加载"), QDialogButtonBox::ResetRole);
    connect(reload, &QPushButton::clicked, this, [this] {
        if (impl_->browser && !impl_->closed)
            impl_->browser->Reload();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    impl_->poll = new QTimer(this);
    impl_->poll->setInterval(2000);
    connect(impl_->poll, &QTimer::timeout, this, &LoginDialog::collectCookies);
}

LoginDialog::~LoginDialog()
{
    closeBrowser();
    if (impl_->comInitialized)
        CoUninitialize();
}

void LoginDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!impl_->initialized && !impl_->closed) {
        impl_->initialized = true;
        QTimer::singleShot(0, this, &LoginDialog::initialize);
    }
}

void LoginDialog::initialize()
{
    if (impl_->closed)
        return;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) {
        fail(QStringLiteral("无法初始化网页登录线程（%1）。").arg(static_cast<quint32>(apartment), 8, 16, QLatin1Char('0')));
        return;
    }
    impl_->comInitialized = true;
    if (!QDir().mkpath(impl_->browserDataDir)) {
        fail(QStringLiteral("无法初始化插件登录目录。"));
        return;
    }
    const auto folder = QDir::toNativeSeparators(QDir(impl_->browserDataDir).absolutePath()).toStdWString();
    const QPointer<LoginDialog> guard(this);
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [guard](HRESULT error, ICoreWebView2Environment *environment) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                if (FAILED(error) || !environment) {
                    guard->fail(QStringLiteral("无法启动登录浏览器，请安装或修复 Microsoft Edge WebView2 Runtime。"));
                    return S_OK;
                }
                guard->impl_->environment = environment;
                guard->initializeController();
                return S_OK;
            }).Get());
    if (FAILED(started))
        fail(QStringLiteral("无法启动登录浏览器，请安装或修复 Microsoft Edge WebView2 Runtime。"));
}

void LoginDialog::initializeController()
{
    ComPtr<ICoreWebView2Environment10> environment10;
    ComPtr<ICoreWebView2ControllerOptions> options;
    if (FAILED(impl_->environment.As(&environment10)) ||
        FAILED(environment10->CreateCoreWebView2ControllerOptions(&options)) ||
        FAILED(options->put_ProfileName(L"LivehimeObsLogin")) ||
        FAILED(options->put_IsInPrivateModeEnabled(TRUE))) {
        fail(QStringLiteral("当前 WebView2 Runtime 过旧，请更新后重试登录。"));
        return;
    }
    const QPointer<LoginDialog> guard(this);
    const HRESULT started = environment10->CreateCoreWebView2ControllerWithOptions(
        reinterpret_cast<HWND>(impl_->surface->winId()), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [guard](HRESULT error, ICoreWebView2Controller *controller) -> HRESULT {
                if (!guard || guard->impl_->closed) {
                    if (controller)
                        controller->Close();
                    return S_OK;
                }
                if (FAILED(error) || !controller) {
                    guard->fail(QStringLiteral("无法创建登录窗口。"));
                    return S_OK;
                }
                guard->impl_->controller = controller;
                if (FAILED(controller->get_CoreWebView2(&guard->impl_->browser))) {
                    guard->fail(QStringLiteral("无法访问登录浏览器。"));
                    return S_OK;
                }
                guard->configureBrowser();
                return S_OK;
            }).Get());
    if (FAILED(started))
        fail(QStringLiteral("无法创建登录窗口。"));
}

void LoginDialog::configureBrowser()
{
    ComPtr<ICoreWebView2_2> browser2;
    if (FAILED(impl_->browser.As(&browser2)) || FAILED(browser2->get_CookieManager(&impl_->cookies))) {
        fail(QStringLiteral("当前浏览器不支持登录会话，请更新 WebView2 Runtime。"));
        return;
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(impl_->browser->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_IsWebMessageEnabled(TRUE);
    }
    const QPointer<LoginDialog> guard(this);
    HRESULT registered = impl_->browser->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [guard](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                ComString uri;
                if (FAILED(args->get_Uri(&uri.value)) || !allowedUrl(QUrl(uri.text()))) {
                    args->put_Cancel(TRUE);
                    guard->impl_->status->setText(QStringLiteral("登录窗口仅允许哔哩哔哩官方 HTTPS 页面。"));
                }
                return S_OK;
            }).Get(), &impl_->navigationToken);
    if (FAILED(registered)) {
        fail(QStringLiteral("无法设置登录页面安全边界。"));
        return;
    }
    impl_->browser->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [guard](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                BOOL success = FALSE;
                if (SUCCEEDED(args->get_IsSuccess(&success)) && !success)
                    guard->impl_->status->setText(QStringLiteral("登录页面加载失败，请检查网络后点击“重新加载”。"));
                else
                    guard->impl_->status->setText(QStringLiteral("请使用哔哩哔哩 App 扫码，并在手机上确认登录。"));
                return S_OK;
            }).Get(), &impl_->navigationCompletedToken);
    registered = impl_->browser->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [guard](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                ComString origin, message;
                if (FAILED(args->get_Source(&origin.value)) || !loginOrigin(QUrl(origin.text())) ||
                    FAILED(args->TryGetWebMessageAsString(&message.value)))
                    return S_OK;
                if (message.text() == QStringLiteral("livehime-obs:login-success"))
                    guard->collectCookies();
                else if (message.text() == QStringLiteral("livehime-obs:cancel"))
                    QTimer::singleShot(0, guard, &QDialog::reject);
                return S_OK;
            }).Get(), &impl_->messageToken);
    if (FAILED(registered)) {
        fail(QStringLiteral("无法设置扫码完成通知。"));
        return;
    }
    registered = impl_->browser->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [guard](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE);
                if (!guard || guard->impl_->closed)
                    return S_OK;
                BOOL initiated = FALSE;
                ComString uri;
                if (SUCCEEDED(args->get_IsUserInitiated(&initiated)) && initiated &&
                    SUCCEEDED(args->get_Uri(&uri.value)) && allowedUrl(QUrl(uri.text())))
                    QDesktopServices::openUrl(QUrl(uri.text()));
                return S_OK;
            }).Get(), &impl_->newWindowToken);
    if (FAILED(registered)) {
        fail(QStringLiteral("无法设置登录弹窗策略。"));
        return;
    }
    impl_->browser->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(), &impl_->permissionToken);
    const auto script = bridgeScript().toStdWString();
    const HRESULT added = impl_->browser->AddScriptToExecuteOnDocumentCreated(
        script.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [guard](HRESULT error, LPCWSTR) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                if (FAILED(error)) {
                    guard->fail(QStringLiteral("无法准备登录页面。"));
                    return S_OK;
                }
                const auto url = QString::fromLatin1(loginUrl).toStdWString();
                if (FAILED(guard->impl_->browser->Navigate(url.c_str()))) {
                    guard->fail(QStringLiteral("无法加载哔哩哔哩登录页面。"));
                    return S_OK;
                }
                guard->resizeBrowser();
                guard->impl_->controller->put_IsVisible(TRUE);
                guard->impl_->poll->start();
                return S_OK;
            }).Get());
    if (FAILED(added))
        fail(QStringLiteral("无法准备登录页面。"));
}

void LoginDialog::collectCookies()
{
    if (impl_->closed || impl_->reading || impl_->delivered || !impl_->cookies)
        return;
    impl_->reading = true;
    const QPointer<LoginDialog> guard(this);
    const HRESULT started = impl_->cookies->GetCookies(
        L"", Callback<ICoreWebView2GetCookiesCompletedHandler>(
            [guard](HRESULT error, ICoreWebView2CookieList *list) -> HRESULT {
                if (!guard || guard->impl_->closed)
                    return S_OK;
                guard->impl_->reading = false;
                if (FAILED(error))
                    return S_OK;
                const auto cookies = readCookies(list);
                bool hasSession = false, hasCsrf = false;
                for (const auto &cookie : cookies) {
                    if (cookie.name() == QByteArrayLiteral("SESSDATA") && !cookie.value().isEmpty())
                        hasSession = true;
                    if (cookie.name() == QByteArrayLiteral("bili_jct") && !cookie.value().isEmpty())
                        hasCsrf = true;
                }
                if (!hasSession || !hasCsrf)
                    return S_OK;
                guard->impl_->delivered = true;
                guard->impl_->poll->stop();
                // Leave the COM event before emitting user callbacks or closing.
                QTimer::singleShot(0, guard, [guard, cookies] {
                    if (!guard || guard->impl_->closed)
                        return;
                    guard->closeBrowser();
                    emit guard->cookiesReady(cookies);
                    if (guard)
                        guard->accept();
                });
                return S_OK;
            }).Get());
    if (FAILED(started))
        impl_->reading = false;
}

void LoginDialog::resizeBrowser()
{
    if (!impl_->controller || impl_->closed)
        return;
    RECT bounds{};
    GetClientRect(reinterpret_cast<HWND>(impl_->surface->winId()), &bounds);
    impl_->controller->put_Bounds(bounds);
    impl_->controller->NotifyParentWindowPositionChanged();
}

void LoginDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    resizeBrowser();
}

bool LoginDialog::eventFilter(QObject *object, QEvent *event)
{
    if (object == impl_->surface && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        resizeBrowser();
    return QDialog::eventFilter(object, event);
}

void LoginDialog::fail(const QString &message)
{
    impl_->status->setText(message);
    impl_->poll->stop();
    const QPointer<LoginDialog> guard(this);
    QTimer::singleShot(0, this, [guard, message] {
        if (guard && !guard->impl_->closed)
            emit guard->loginFailed(message);
    });
}

void LoginDialog::closeBrowser()
{
    if (impl_->closed)
        return;
    impl_->closed = true;
    impl_->poll->stop();
    if (impl_->browser) {
        impl_->browser->remove_NavigationStarting(impl_->navigationToken);
        impl_->browser->remove_NavigationCompleted(impl_->navigationCompletedToken);
        impl_->browser->remove_WebMessageReceived(impl_->messageToken);
        impl_->browser->remove_NewWindowRequested(impl_->newWindowToken);
        impl_->browser->remove_PermissionRequested(impl_->permissionToken);
        impl_->browser->Stop();
    }
    if (impl_->cookies)
        impl_->cookies->DeleteAllCookies();
    if (impl_->controller)
        impl_->controller->Close();
    impl_->cookies.Reset();
    impl_->browser.Reset();
    impl_->controller.Reset();
    impl_->environment.Reset();
}

void LoginDialog::done(int result)
{
    closeBrowser();
    QDialog::done(result);
}

} // namespace livehime
