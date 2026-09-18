# FaceLoginSetup 开发文档

FaceLoginSetup 是 FaceLogin 的 Windows 图形安装器和卸载器，使用 Wails v2、Vue 3、TypeScript 和 Go 编写。本文档对应当前 `dev-2.0.0` 开发线，并以仓库根目录的 `DEVELOPMENT.md` 为整体架构说明。

## 目录

1. [项目结构](#项目结构)
2. [构建与运行](#构建与运行)
3. [资源与构建产物](#资源与构建产物)
4. [安装流程](#安装流程)
5. [卸载流程](#卸载流程)
6. [配置升级与版本公告](#配置升级与版本公告)
7. [注册表与目录](#注册表与目录)
8. [发布检查清单](#发布检查清单)

---

## 项目结构

```
FaceLoginSetup/
├── main.go                         # 完整安装器入口（非 uninstaller build tag）
├── main_uninstaller.go             # 轻量独立卸载器入口（uninstaller build tag）
├── app.go                          # Wails 绑定：安装、卸载、快捷方式、升级公告
├── constants.go                    # 注册表键、安装器常量
├── build-installer.ps1             # 先构建 Uninstall.exe，再构建 FaceLoginSetup.exe
├── frontend_assets.go              # 完整安装器前端资源入口
├── embedded_resources.go           # 完整安装器 resources/ 的 embed 入口
├── embedded_resources_uninstaller.go # 卸载器不嵌入安装资源的占位入口
├── wails.json                      # Wails 项目配置
├── go.mod / go.sum
├── internal/
│   ├── config.go                   # 首装默认配置和按版本配置动作
│   ├── notice.go                   # 升级公告数据
│   ├── com.go                      # Credential Provider 注册/注销
│   ├── scm.go                      # Windows 服务停止、安装、删除
│   ├── extract.go                  # resources/ 解压和轻量卸载文件清单
│   ├── elevate.go                  # 管理员提权
│   ├── shortcut.go                 # Windows 原生 IShellLinkW 快捷方式
│   ├── uninstall_cleanup.go        # 卸载器退出后的自删除与目录清理
│   └── util.go                     # 注册表、目录、文件及路径工具
├── resources/                      # 完整安装器的内嵌部署资源
└── frontend/
    └── src/
        ├── App.vue                 # 安装/卸载页面及自定义弹窗
        ├── i18n.ts                 # 安装器语言包和动态消息翻译
        ├── notice-zh.json          # 中文升级公告正文
        ├── notice-en.json          # 英文升级公告正文
        ├── main.ts / style.css
        └── wailsjs/                 # Wails 自动生成绑定，勿手工修改
```

`Uninstall.exe` 与完整安装器共用前端和卸载逻辑，但使用 `uninstaller` build tag 构建。它不包含 `resources/`，因此不会把模型、服务、Console 和运行库再次打进卸载程序。

---

## 构建与运行

前置条件：Go 1.25+、Node/npm、Wails CLI v2、Windows x64 构建环境。

```powershell
cd installer\FaceLoginSetup

# 安装 Wails CLI（尚未安装时执行）
go install github.com/wailsapp/wails/v2/cmd/wails@v2.13.0

# 开发模式：前端热重载，适合编辑器内调试
wails dev

# 发布构建：推荐使用仓库脚本
.\build-installer.ps1
```

脚本的固定顺序是：

1. 使用 `-tags uninstaller` 构建不嵌入安装资源的 `Uninstall.exe`。
2. 将该文件复制到 `resources/Uninstall.exe`。
3. 构建完整的 `FaceLoginSetup.exe`，把 `resources/` 一起嵌入。

如需只构建完整安装器，可以在资源已经就位时执行：

```powershell
wails build -clean -platform windows/amd64
```

但发布版本必须确认 `resources/Uninstall.exe` 是由当前源码和当前脚本重新生成的轻量卸载器。修改 Go 绑定或前端后应使用 `-clean`，避免生成文件和旧绑定影响结果。

---

## 资源与构建产物

### 资源目录

完整安装器需要将 C++ 构建产物和模型放在 `resources/`：

| 文件 | 来源 |
|---|---|
| `FaceLoginService.exe` | `build/face_service/Release/` |
| `FaceLoginCredentialProvider.dll` | `build/credential_provider/Release/` |
| `FaceLoginConsole.exe` | CMake 目标直接输出到 `resources/`，或从 Release 产物同步 |
| `onnxruntime.dll`、OpenBLAS、protobuf 等运行库 | C++ 构建/依赖部署产物 |
| `models/*.onnx` | 检测、106 点地标、识别、活体和姿态模型 |
| `models/head_pose_mobilenetv2.LICENSE.txt` | 姿态模型许可证和来源记录 |
| `locales/*.json` | 根目录三语言包的部署副本 |
| `Uninstall.exe` | `build-installer.ps1` 第一步生成 |

当前随包模型约 31 MB，其中 `head_pose_mobilenetv2.onnx` 约 8.9 MB。`jpeg62.dll`、`libpng16.dll`、`z.dll` 和 `diag_glass` 不属于当前资源清单。

### 构建产物

| 目标 | 输出 |
|---|---|
| C++ 服务 | `build/face_service/Release/FaceLoginService.exe` |
| Credential Provider | `build/credential_provider/Release/FaceLoginCredentialProvider.dll` |
| Console | `installer/FaceLoginSetup/resources/FaceLoginConsole.exe` |
| 独立卸载器 | `installer/FaceLoginSetup/resources/Uninstall.exe` |
| 完整安装器 | `installer/FaceLoginSetup/build/bin/FaceLoginSetup.exe` |

---

## 安装流程

前端通过 `App.Install(installDir, locale)` 调用后端，后端通过 `setup:progress` 事件向页面推送步骤和状态。

| 阶段 | 后端行为 |
|---|---|
| 1 | 停止并删除已有 `FaceLoginService` |
| 2 | 规范化并校验最终安装目录；用户选择目录后自动追加 `FaceLogin`，末级目录判断不区分大小写 |
| 3 | 创建目录，写入 `HKLM\SOFTWARE\FaceLogin` 的 `InstallPath` 和 `DataPath` |
| 4 | 从完整安装器内嵌的 `resources/` 解压程序、运行库、模型、语言包和 `Uninstall.exe` |
| 4.5 | 创建 `data/`、`log/`，写入首装默认配置或保留现有用户配置 |
| 5 | 设置安装目录及数据目录 ACL |
| 6 | 注册 `FaceLoginCredentialProvider.dll` |
| 7 | 安装并启动 Windows 服务 |
| 8 | 可选使用 Windows 原生 Shell Link API 创建桌面 `FaceLogin Console.lnk` |
| 9 | 完成安装并显示升级公告（仅升级安装且本版本启用公告时） |

安装器使用自定义 Vue 弹窗显示升级公告、安装错误和卸载确认，不调用 WebView 的原生 `alert`、`confirm` 或 `prompt`。目录选择器仍然使用 Windows 原生文件夹选择器，这是有意保留的系统交互。

安装器默认把 `InstallPath` 和 `DataPath` 写成同一个用户选择的目录，目录结构为：

```
<installDir>\
├── FaceLoginService.exe
├── FaceLoginCredentialProvider.dll
├── FaceLoginConsole.exe
├── Uninstall.exe
├── models\
├── locales\
├── data\config.json
├── data\users.dat
└── log\*.log
```

如果注册表路径不可用，C++ 公共路径逻辑才会回退到 `%PROGRAMDATA%\FaceLogin`。

---

## 卸载流程

完整安装器的卸载页和安装目录中的独立 `Uninstall.exe` 共用同一套后端清理逻辑：

1. 停止并删除 Windows 服务。
2. 注销 Credential Provider COM DLL。
3. 删除安装程序部署的可执行文件、DLL、模型和语言包。
4. 删除 `data/` 和 `log/`，即删除 `users.dat`、配置和日志；页面会在执行前明确提示这是彻底删除。
5. 删除由当前安装生成、且目标指向当前安装目录的桌面快捷方式。
6. 删除整个 `HKLM\SOFTWARE\FaceLogin` 注册表键。
7. 删除空的安装子目录和安装目录；如果存在用户自行放入的未知文件或目录，则保留目录，不扩大删除范围。

独立卸载器正在运行时不能直接删除自身。`FinalizeStandaloneUninstall()` 会启动临时目录中的清理副本，等待 UI 进程退出后删除安装目录中的 `Uninstall.exe`，最后再尝试移除空安装目录。清理辅助进程使用隐藏窗口启动，避免卸载完成时闪出控制台窗口。

轻量卸载器只维护固定的安装文件名清单和 `models/`、`locales/` 目录约定，不依赖完整安装资源，因此体积不包含 ONNX 模型和运行库。

---

## 配置升级与版本公告

### 配置升级

`internal.ConfigUpgradeEnabled` 和 `ConfigUpgradeForcedDefaults` 是按版本使用的一次性机制。默认关闭；只有本版本确实要强制同步既有用户配置时才在 `main.go` 打开，并且只列出本版本改变的键。当前 2.0.0 不强制覆盖用户配置。

首装默认配置包括：

```json
{
  "liveness_method": "none",
  "match_threshold": 0.75,
  "anti_spoof_threshold": 0.30,
  "unload_models_after_auth": false,
  "capture_unknown_faces": false,
  "cold_boot_key_trigger": false,
  "ui_language": "auto"
}
```

当前版本不再提供软件曝光、暗光增强或光照归一化配置。旧配置中残留的曝光字段不会被当前 C++ 配置结构使用，保存配置时也不会重新写出。

### 升级公告

升级公告由 `internal.NoticeEnabled`、`NoticeVersion`、`NoticeTitle` 和 `NoticeBody` 控制。前端只有在“已有安装 + 本次安装成功”时查询并显示公告，全新安装不显示。当前公告正文位于 `frontend/src/notice-zh.json` 和 `notice-en.json`，中文界面使用中文，其余语言使用英文。

正文支持“分类标题行 + 条目行”的格式，分类标题以中文或英文冒号结尾；修改后必须重新执行 `build-installer.ps1` 才会进入安装器二进制。当前公告按识别管线、姿态检测、认证交互、Console/安装器和兼容性分类。

---

## 注册表与目录

所有安装器注册表操作位于 `HKLM\SOFTWARE\FaceLogin`：

| 键 | 用途 |
|---|---|
| `InstallPath` | 当前安装目录、安装检测和升级判断 |
| `DataPath` | C++ 端追加 `models`、`data` 和 `log` 的根目录 |
| `LoginEntryGeneration` / `AutoAttemptGeneration` | 服务与 Credential Provider 共享的一次性冷启动/注销登录入口代次 |
| `LoginEntryActive` / `LoginEntrySession` / `LoginEntryBootRecord` | 当前登录入口是否有效、所属控制台会话和对应 Kernel-Boot 记录 |
| `LastKernelBootRecord` | 已处理的最新 Kernel-Boot 事件记录号，避免重复创建登录入口 |
| `ColdBootKeyTrigger` | Console 将 `cold_boot_key_trigger` 镜像给 Provider 的运行时开关 |
| `AboutSeenVersion` | Console 关于卡片的版本提示状态 |

不要在安装器中硬编码资源版本或删除旧配置字段。当前配置由 C++ `config_util` 直接序列化；安装器只负责首装配置文件和明确启用的强制升级动作。冷启动代次由服务根据 Kernel-Boot/会话事件维护，不由安装器推断。

---

## 发布检查清单

1. 在仓库根目录编译 C++ 服务、Credential Provider 和 Console。
2. 将最新 C++ 二进制、运行库、模型和三语言包同步到 `resources/`。
3. 确认当前默认活体方式为 `none`，冷启动按键触发为 `false`，且不再生成曝光/光照归一化配置项。
4. 执行 `node scripts/check-locales.mjs`，确保三语言 key、占位符和 Console 文本一致。
5. 运行 `.\build-installer.ps1`，同时生成 `resources/Uninstall.exe` 和完整 `build/bin/FaceLoginSetup.exe`。
6. 检查完整安装器包含 `Uninstall.exe`、姿态模型和姿态模型许可证；检查独立卸载器不包含 `resources/` 内容。
7. 手动验证全新安装、升级安装、安装目录追加、大小写兼容、桌面快捷方式、安装器自定义弹窗和独立卸载。
8. 卸载测试后确认服务、COM 注册、快捷方式、空安装目录和注册表键按预期清理；有未知文件时确认目录被保留。

---

## 常见问题

**修改前端或 Go 代码后界面没有变化？**

使用 `wails build -clean` 或 `.\build-installer.ps1`。Wails 前端绑定和完整 `resources/` 都是在构建时生成/嵌入的。

**独立卸载器为什么比完整安装器小？**

独立卸载器通过 `uninstaller` build tag 排除 `embedded_resources.go` 中的安装资源，只保留 Wails UI、卸载逻辑和固定文件清单；它不需要模型、服务和运行库。

**卸载后安装目录没有消失？**

只有目录为空时才会删除。检查是否存在用户自行放入的文件、目录或仍被其他进程占用的文件；卸载器不会为删除安装目录而扩大删除范围。
