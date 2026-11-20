# 构建依赖说明

本项目以 **Windows x64**、**OBS Studio 33.0.1**、**Qt 6.11.1** 作为编译和运行基线。所有依赖的下载地址、固定版本和 SHA-256 校验值都记录在 `dependencies.lock.json` 里。构建时 `scripts/bootstrap-deps.ps1`（需要 PowerShell 7）会逐一校验压缩包哈希再解压，不会修改或覆盖本机的 OBS。

```powershell
pwsh -NoProfile -File scripts/build.ps1 -DependenciesRoot D:/REA/livehime-obs-deps
pwsh -NoProfile -File scripts/package.ps1
```

- 省略 `-DependenciesRoot` 时，依赖默认缓存到项目下的 `.deps`。
- 首次构建需要联网拉取官方依赖；如果本地已经有下好的压缩包，可以加 `-Offline`；不想每次跑依赖检查可以直接加 `-SkipBootstrap`。
- 构建脚本只允许 `Release` 和 `RelWithDebInfo` 两种配置，避免把 Debug 版的 Qt/CRT 运行库塞进正式版 OBS 里引起崩溃。

## OBS 头文件与导入库

OBS 官方没有单独发布面向插件的独立 SDK 包，因此这里的做法是：**源码拿头文件 + 本机 DLL 导出表生成导入库**：

- `bootstrap-deps.ps1` 从官方 `33.0.1` 源码包提取 `libobs` 和 `frontend/api` 公开头文件；
- 用 MSVC 的 `dumpbin /EXPORTS` 读取本机同版本 `obs.dll` 和 `obs-frontend-api.dll` 的导出表，生成 `.def` 再用 `lib /MACHINE:X64 /DEF` 转成 `.lib`。整个过程不需要自己编译 OBS，也不会把 OBS 的 DLL 打进插件包里。
- 生成目录 `obs-sdk-33.0.1-x64/sdk-manifest.json` 会记录本机 DLL 的版本、SHA-256 和导出函数数量，方便核对。里面的 `obsconfig.h` 只是引用公开头文件所需的最小配置，不是完整编译 OBS 的配置。

当前验证基线使用的本机运行库哈希：

| 文件 | SHA-256 |
| --- | --- |
| `obs.dll` 33.0.1 | `34d3213b1ed9386cb0c2072fafb8946b2fced699f22219c496fcaba5bf1de6bb` |
| `obs-frontend-api.dll` 33.0.1 | `bd19b9d25da23de9242b15631b51ae43a22476b33999f0b2eef0c3c55ea2d439` |

## Qt 与 WebView2

- **Qt（6.11.1）**：使用 OBS `33.0.1` 发布标签在 `CMakePresets.json` 中锁定的 `obs-deps` **2026-10-23** Windows x64 开发包。插件动态链接 Core/Gui/Widgets/Network，运行时直接复用 OBS 自带的 Qt DLL。
- **TLS 后端**：由于 OBS 自带的 Qt 没有附带 TLS 后端，插件只在自己的数据目录 `qt-plugins/tls/` 下携带一份同版本未修改的 `qschannelbackend.dll`（SHA-256 为 `0d1a73019c732551f3b1fca1391435fc1f7e551e50cd22d2b273d0495e7ff3dd`，CMake 配置阶段会核对），并在创建网络管理器前注册该路径。它直接走 Windows 原生加密库，不替换 OBS 原有的 Qt 主库，也不需要捆绑 OpenSSL。相关源码与许可见根目录 `THIRD-PARTY-NOTICES.md`。
- **WebView2**：使用 Microsoft NuGet 稳定包 **1.0.4314.35** 的头文件和静态加载库 `WebView2LoaderStatic.lib`。插件本身只内嵌静态加载器，不捆绑浏览器内核，运行时调用系统里的 Microsoft Edge WebView2 Runtime。原始微软许可保存在 `licenses/`。

## 实际验证使用的工具链

- Visual Studio 2022 Community 17.14（MSVC 14.44.35207，编译器 19.44.35229.0）
- Windows SDK 10.0.26100.0
- CMake 3.28+（生成器 `Visual Studio 17 2022`）
- C++20、动态发行版 MSVC CRT（`/MD`）、UTF-8 源码编码

`package.ps1` 生成的压缩包采用传统的 OBS 根目录结构（`obs-plugins/64bit/livehime-obs.dll` 和 `data/obs-plugins/livehime-obs/`），同时附上 `SHA256SUMS.json`，不会往你的 OBS 目录里写文件。

## 排查 HTTPS 连接的小工具

如果遇到 HTTPS 请求异常，可以手动跑一次网络探测脚本：

```powershell
pwsh -NoProfile -File scripts/probe-network.ps1 -ObsRoot "<你的OBS目录>"
```

它会调用真实 OBS 目录里的 `QtNetwork.dll` 和插件自带的 TLS 目录，向 `nav` 接口发一次**不带 Cookie** 的 GET 请求，然后打印 Qt 运行库路径、TLS 后端、HTTP 状态码和网络错误号（不会读取本地账号，也不属于默认离线测试）。只要返回 `HTTP 200` 且业务码 `-101`（未登录），就说明 HTTPS 链路正常。
