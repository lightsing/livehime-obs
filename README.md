# Livehime OBS · Windows

[![Windows CI](https://github.com/lightsing/livehime-obs/actions/workflows/windows.yml/badge.svg)](https://github.com/lightsing/livehime-obs/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/lightsing/livehime-obs)](https://github.com/lightsing/livehime-obs/releases)

在 OBS 的「B站直播」面板内扫码登录、自动同步直播标题、裁切上传主封面，并使用 OBS 原生「开始直播 / 停止直播」按钮开播与下播。OBS 使用 B 站开播响应中返回的 RTMP 地址直接推流，不需要运行直播姬或本地中转服务。

这是基于直播姬 **7.64.0.10819** 协议分析实现的首个版本，面向 **Windows x64 + OBS Studio 33.0.1（Qt 6.11.1）**。已在独立 OBS 中验证用户扫码、登录恢复、房间信息读取、分区名称搜索和当前封面显示。真实开播授权、标题封面修改、音视频效果及长时间推流仍需实际使用验证；离线测试不代表平台已经接受这些操作。

## 安装

1. 关闭所有 OBS 实例。
2. 从 [Releases](https://github.com/lightsing/livehime-obs/releases) 下载 `livehime-obs-0.1.0-windows-x64-setup.exe` 并运行，接受 Windows 管理员授权。向导提供简体中文和英文，自动识别 OBS 安装目录，也可手动选择，用于检查 OBS 33.0.1 x64 与 Qt 6.11.1。
3. 启动 OBS，在「停靠窗口 / Docks」菜单启用「B站直播」。
4. 如果扫码窗口提示缺少运行时，安装或修复 [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)。Windows 11 通常已经具备此运行时。

标准安装器将插件放入 **`C:\ProgramData\obs-studio\plugins\livehime-obs`**，这是所有用户共享的独立插件目录，不提供“当前用户 / 所有用户”选择。DLL 位于该目录根部，资源位于 `data`，说明和许可位于 `data/docs`。共享插件安装、升级与卸载前要求退出所有 OBS；安装程序只提示关闭，不结束 OBS 进程。用户各自的登录缓存、场景和配置仍保存在各自 OBS 配置目录。

同版本重装可以修复插件文件，且不会产生重复卸载项；升级保留原插件目录和关联的 OBS 目录。卸载入口位于 Windows「已安装的应用」中的 **Livehime OBS**，仅移除安装器登记的文件，保留场景、配置、其他插件和登录缓存。

如果装过旧版安装器，先卸载旧版，再安装标准目录版本。手动安装过 ZIP 的用户应先移走 OBS 根目录 `obs-plugins/64bit/livehime-obs.dll`，避免加载重复副本；不要删除 OBS 用户配置。安装器发现旧版或重复副本会给出路径，要求先处理。

Releases 还提供 `livehime-obs-0.1.0-windows-x64.zip`，保留旧目录结构供便携 OBS 手动安装：将其中 `obs-plugins` 和 `data` 合并到便携 OBS 根目录。普通安装版建议使用 EXE；不要同时安装 ZIP 与 EXE 两份插件。

安装包只在插件私有数据目录携带匹配 Qt 6.11.1 的 Schannel TLS 后端，用于 HTTPS。其他 OBS 主版本或 Qt 版本应重新构建，不能直接假定 ABI 兼容。当前安装器尚未做 Authenticode 代码签名，旁边提供 `.sha256` 和 `.json` 记录包与 DLL 的校验值；这不等同于发布者签名。

## 使用

建议为 B 站建立独立 OBS 配置文件，使用普通 H.264 视频 / AAC 音频输出，先配置好场景、分辨率和码率。关闭增强广播 / 多轨视频；当前插件处理普通单路 RTMP。插件沿用 OBS 的编码、画面、音轨和网络重连设置。

1. 点击「扫码登录」，用哔哩哔哩 App 扫码并确认。插件从独立 WebView2 会话取得 Cookie，随后请求 `nav` 验证身份。
2. 登录后读取直播间、当前标题、分区与审核信息。分区分为「父分类」和「子分区」两个下拉框，默认选中当前房间的分类与分区。切换父分类时只显示其子分区；子分区支持输入名称搜索，再从候选项中选择。
3. 修改标题后停止输入约 1 秒，或让标题输入框失去焦点，插件会自动同步；标题下方显示同步进度。保存期间继续输入会保留最新草稿，失败后保留文字并提示原因。
4. 鼠标移到当前封面上，点击「选择图片」。在裁切窗口拖动图片调整位置，使用滑块或滚轮缩放，确认 **4:3** 构图后点击「保存并上传」。插件立即上传 **720 × 540** 的裁切结果并回读平台封面；取消裁切不上传图片。
5. 点击 OBS「控件」停靠窗口中的原生「开始直播」。插件等待标题和已确认的封面同步完成，再重新检查房间状态、向 B 站申请开播，将本场地址交给 OBS。保存失败时不会继续开播。同步资料及读取房间阶段，原按钮显示「取消开播」；平台开播请求发出后按钮暂时禁用，等待服务器结果。若平台要求实名、人脸等核验，请按面板提示到官方页面完成后刷新。
6. 点击同一个原生「停止直播」按钮。插件等待 OBS 媒体输出停止，再请求 B 站下播，成功后恢复本场前的 OBS 推流服务。插件面板不再提供单独的开播、下播按钮；下播结果未确认时，原生按钮显示「重试 / 确认停止直播」。

网络断线时，OBS 自动重连复用当前场次；连接失败后可点面板中的「重试 OBS 连接」，不会再次申请开播。首次连接尚未完成时，可以使用原生「停止直播」取消连接；底层连接线程结束可能需要等待网络超时。如果开播响应丢失或没有返回普通 RTMP，面板保留“结果未确认”状态，要求先刷新或停止直播，不会自动重发开播请求。下播失败会保留重试入口。

原生按钮接管针对 OBS 33.0.1 的 Qt 控件连接实现，包含延迟推流菜单的开始、停止入口；原有强制停止入口保留。OBS 原生热键、托盘菜单和其他插件直接调用前端 API 的开始操作不经过此按钮，请使用「控件」中的按钮发起 B 站开播。自动重连仍由 OBS 输出模块处理，不经过按钮，也不会重新申请平台开播。其他输出通过上述入口启动后，原生按钮继续由 OBS 处理其停止操作。

请先下播成功，再退出 OBS。进程强制关闭、崩溃或断电时无法保证发送下播请求；再次登录后可刷新房间，并结束尚未关闭的直播。未结束当前场次前不能退出登录或切换账号；会话失效时可以重新登录原账号。

## 登录信息与日志

- 插件只使用自己的 WebView2 登录会话，不读取直播姬或现有浏览器的账号文件。
- WebView2 登录使用独立 InPrivate profile；已验证的 Cookie 使用当前 Windows 用户的 DPAPI 加密，保存在 OBS 插件配置目录的 `session.dpapi`。
- `device-id` 是该插件生成并持久保存的设备标识，与 Cookie `buvid3` 分开处理。
- 启动时复验会话；失效后重新扫码。此版本没有实现网页登录令牌自动续期。
- 本场推流密钥保留在进程内，插件不调用 OBS 保存服务配置的接口，也不记录 Cookie、请求体或推流密钥。直播过程中请勿主动在 OBS 推流设置中保存本场临时服务。
- 分区列表与封面预览使用独立的匿名网络通路，不向图片 CDN 发送账号 Cookie 或设备标识。
- 退出登录删除插件保存的会话。卸载插件不自动删除 OBS 插件配置目录。

## 从源码构建

需要 Visual Studio 2022 的 C++ 桌面开发组件、Windows SDK、CMake 3.28+ 和 PowerShell 7。以下命令下载固定版本的官方 OBS 归档供编译和测试使用，无需提前安装 OBS。

```powershell
git clone https://github.com/lightsing/livehime-obs.git
Set-Location livehime-obs
$obsRoot = & .\scripts\bootstrap-obs.ps1
.\scripts\build.ps1 -ObsRoot $obsRoot
.\scripts\verify-load.ps1 -ObsRoot $obsRoot
.\scripts\package.ps1
.\scripts\build-installer.ps1
.\scripts\test-installer.ps1 -InstallerPath .\dist\livehime-obs-0.1.0-windows-x64-setup.exe
```

`bootstrap-obs.ps1` 按 `deps/obs-runtime.lock.json` 下载并校验官方 OBS 归档，解压到项目 `.deps`，不启动或安装 OBS。`build.ps1` 按 `deps/dependencies.lock.json` 下载并验证 SHA-256：OBS 对应版本头文件、OBS 官方 Qt 开发包、Microsoft WebView2 SDK。OBS 导入库从指定的同版本 DLL 导出表生成；不需要重新编译整个 OBS。默认 `RelWithDebInfo`、x64、单节点编译，调试信息嵌入对象文件以避免共享 PDB 服务限制。

也可以使用本机已经安装的匹配版本 OBS。已有依赖归档时加 `-Offline` 避免再次下载：

```powershell
.\scripts\build.ps1 -ObsRoot 'C:\Program Files\obs-studio' -DependenciesRoot .\.deps -Offline
```

构建完成后，DLL 位于 `build/RelWithDebInfo/livehime-obs.dll`，ZIP 位于 `dist/`。`verify-load.ps1` 验证依赖加载与 OBS 模块导出，不会登录账号或开播。

`build-installer.ps1` 从已生成的同版本 ZIP 中校验并提取四个运行时文件，将同一组字节映射为独立插件目录，再用 Inno Setup 7.1.1 编译标准安装器。`bootstrap-installer.ps1` 按 `deps/installer-toolchain.lock.json` 下载官方构建工具，校验 SHA-256 与发布者签名，以便携模式安装到项目 `.tools`；不设置文件关联或系统 PATH。可用 `-ToolsRoot .\.tools -Offline` 复用已验证缓存。安装脚本位于 `installer/livehime-obs.iss`。

安装器离线验收使用 `scripts/test-installer.ps1 -InstallerPath <setup.exe>`，生成合成的 OBS 宿主和账号哨兵，在独立目录以当前用户模式测试安装、升级、版本拒绝、运行中保护和卸载；不向真实 OBS 安装插件。测试日志保留在指定的 `-Workspace` 中。

## CI 与自动发布

[Windows 工作流](.github/workflows/windows.yml) 在推送到 `main`、提交 Pull Request 和手动触发时执行：下载并校验依赖、编译、运行 6 组 CTest、检查 DLL 加载、生成 ZIP 和 EXE，以及运行 28 组安装器验收。成功构建的安装包和验证记录可从 Actions artifacts 下载；真实账号登录和推流不属于 CI。

发布新版本时，先修改 `CMakeLists.txt` 中的项目版本并更新说明，提交到 `main`，等待 CI 通过，再推送对应标签：

```powershell
git tag -a v0.1.0 -m 'Livehime OBS 0.1.0'
git push origin v0.1.0
```

标签版本必须与 CMake 项目版本一致。标签工作流重新构建并完成全部验证后，自动创建 GitHub Release，上传安装器、ZIP、SHA-256 校验文件、构建元数据与验收报告。发布使用仓库内置的 `GITHUB_TOKEN`，无需额外配置个人访问令牌；只有发布作业具有 `contents: write` 权限。下载依赖使用固定版本和 SHA-256，GitHub Actions 使用固定提交并由 Dependabot 提交更新。

## 实现与验证

| 文件 | 职责 |
| --- | --- |
| `src/protocol.*` | 精确参数编码、排序、签名和响应解析 |
| `src/bili-client.*` | 独立 Cookie 容器、异步 HTTPS、超时、封面上传与保存 |
| `src/login-dialog.*` | 官方扫码网页与 WebView2 Cookie 导入 |
| `src/session-store.*` | DPAPI 会话缓存与设备标识 |
| `src/live-controller.*` | 身份、房间、开停播状态与异步操作隔离 |
| `src/obs-bridge.*` | 临时 RTMP 服务、输出事件、恢复原服务 |
| `src/native-stream-controls.*` | 接管原生开停播按钮、联动状态与恢复原信号连接 |
| `src/room-dock.*` | OBS 停靠面板 |
| `src/cover-crop-dialog.*` | 拖动与缩放封面，输出固定 4:3 裁切结果 |

离线测试使用合成数据，覆盖签名向量、请求字段及顺序、Cookie 范围、响应错误、RTMP 选择、DPAPI 往返与损坏文件，以及真实控制器在替身网络 / OBS 后端上的开停播时序。测试不接触真实账号、不修改平台房间、不推送音视频。

运行时验收仍应覆盖：手机扫码确认、标题与封面审核、服务器实际开播授权、OBS 画面/声音、主动下播、断网恢复、长播与会话到期。平台接口可能变更，更新协议 profile 应同时更新向量和回归测试。

## 许可

作者与维护者：[lightsing](https://github.com/lightsing)，详见 [AUTHORS.md](AUTHORS.md)。项目代码按 GPL-2.0-or-later 发布，见 `LICENSE`。第三方组件与链接见 `THIRD-PARTY-NOTICES.md` 和 `deps/README.md`。本项目为独立实现，不附带直播姬二进制，不代表哔哩哔哩官方产品。
