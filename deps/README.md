# 构建依赖

项目目前以 Windows x64、OBS Studio **33.0.1**、Qt **6.11.1** 为构建和运行基线。下载地址、固定版本和 SHA256 位于 `dependencies.lock.json`。PowerShell 7 的 `scripts/bootstrap-deps.ps1` 会逐个校验压缩包后再解压，不安装或覆盖 OBS。

```powershell
$obsRoot = & ./scripts/bootstrap-obs.ps1
./scripts/build.ps1 -ObsRoot $obsRoot -DependenciesRoot ./.deps
pwsh -NoProfile -File scripts/package.ps1
```

省略 `-DependenciesRoot` 时使用项目内 `.deps`。首次构建需要联网下载官方依赖。已有压缩包时可加 `-Offline`；只跳过依赖阶段可用 `-SkipBootstrap`。构建脚本只接受 `Release` 和 `RelWithDebInfo`，避免把使用调试版 Qt/CRT 的插件加载进发行版 OBS。

## OBS 头文件和导入库

本项目没有声称使用官方发布的独立 SDK 包。它使用官方 `33.0.1` 源码归档中的 `libobs` 和 `frontend/api` 公开头文件，以及指定的同版官方 `obs.dll` 和 `obs-frontend-api.dll` 的导出表。`obs-runtime.lock.json` 固定官方 Windows x64 ZIP，`bootstrap-obs.ps1` 校验后解压到 `.deps/obs-runtime-33.0.1`，供 CI 与本地无安装构建使用；也可以传入已有安装目录。

`bootstrap-deps.ps1` 用 MSVC `dumpbin /EXPORTS` 生成 `.def`，再用 `lib /MACHINE:X64 /DEF` 生成导入库。不会重新编译 OBS，也不会把 OBS DLL 打入插件包。生成的 `obs-sdk-33.0.1-x64/sdk-manifest.json` 记录本机 DLL 的版本、SHA256 和导出数量，以便复核。`obsconfig.h` 只是公开头文件所需的 Windows 消费方配置，不是 OBS 的完整构建配置。

当前机器验证的运行库：

| 文件 | SHA256 |
| --- | --- |
| `obs.dll` 33.0.1 | `34d3213b1ed9386cb0c2072fafb8946b2fced699f22219c496fcaba5bf1de6bb` |
| `obs-frontend-api.dll` 33.0.1 | `bd19b9d25da23de9242b15631b51ae43a22476b33999f0b2eef0c3c55ea2d439` |

## Qt 和 WebView2

Qt 来自 OBS 33.0.1 发布标签的 `CMakePresets.json` 锁定的 `obs-deps` **2026-10-23** Windows x64 包，实际 Qt 版本为 **6.11.1**。插件动态链接 Core/Gui/Widgets/Network，使用 OBS 自带的同版 Qt DLL。OBS 未部署 Qt TLS 后端，因此包内仅附带匹配版本的 `qschannelbackend.dll`，安装到插件数据的 `qt-plugins/tls/`，在创建网络管理器之前注册此私有目录；不替换任何宿主 Qt 主库。

Schannel 后端 SHA256 为 `0d1a73019c732551f3b1fca1391435fc1f7e551e50cd22d2b273d0495e7ff3dd`，CMake 在配置时校验。其依赖为 OBS 自带的 QtCore/QtNetwork、MSVC CRT 和 Windows 原生加密库，不需要捆绑 OpenSSL。对应源版本与许可见项目根目录 `THIRD-PARTY-NOTICES.md`。

WebView2 使用 Microsoft NuGet 稳定包 **1.0.4314.35** 的公开头文件和 x64 `WebView2LoaderStatic.lib`。静态 loader 嵌入插件；浏览器内核仍由用户系统的 Microsoft Edge WebView2 Runtime 提供。项目不捆绑浏览器内核。原始 Microsoft 许可和第三方声明保存在 `licenses/`。

## 已使用的工具链

- Visual Studio 2022 Community 17.14；MSVC 14.44.35207，编译器 19.44.35229.0。
- Windows SDK 10.0.26100.0；CMake 最低版本 3.28，生成器 `Visual Studio 17 2022`。
- C++20、动态发行版 MSVC CRT `/MD`、UTF-8 源码。

包使用 OBS 支持的传统目录结构：`obs-plugins/64bit/livehime-obs.dll` 和 `data/obs-plugins/livehime-obs/`。`package.ps1` 仅创建压缩包与 SHA256 清单，不向 OBS 安装目录写入文件。

手动排查 HTTPS 时，可运行 `scripts/probe-network.ps1 -ObsRoot <OBS目录>`。该命令使用实际 OBS QtNetwork.dll 和插件私有 TLS 目录，只向 `nav` 发一次不带 Cookie 的 GET，输出 Qt 运行库位置、TLS 后端、HTTP 状态和网络错误编号。它不读取会话，也不属于默认离线 CTest；正常匿名响应应为 HTTP 200、业务码 -101。
