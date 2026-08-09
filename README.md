<p align="center">
  <img src="assets/logo.png" alt="FaceLogin Logo" width="200">
</p>

<p align="center">
  基于 Windows Credential Provider 框架的摄像头人脸识别解锁系统。<br>
  在锁屏界面集成"人脸登录"磁贴，看一眼即可解锁 — 支持本地账户和微软在线账户 (MSA)。
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"></a>
  <a href="DEVELOPMENT.md"><img src="https://img.shields.io/badge/platform-Windows%2010%2B%20x64-blue" alt="Platform"></a>
  <a href="DEVELOPMENT.md"><img src="https://img.shields.io/badge/language-C%2B%2B20%20%7C%20Go-orange" alt="Language"></a>
  <a href="https://github.com/EthanZer0/FaceLogin/releases"><img src="https://img.shields.io/badge/version-1.7.0-green" alt="Version"></a>
</p>

---

## 特性

<div align="center">

| 锁屏人脸解锁 | 双重活体检测 | ONNX 识别 |
|:---:|:---:|:---:|
| Windows 原生锁屏集成<br>无需额外操作 | EAR 眨眼 + MiniFASNetV2<br>防照片/视频/面具攻击 | SCRFD 检测 + InsightFace<br>ONNX 人脸识别 |
| **多账户支持** | **安全存储** | **热配置** |
| 本地 SAM + 微软在线<br>账户全兼容，每账号可录多张人脸 | DPAPI 机器范围加密<br>管道 DACL 访问控制 | 运行时修改识别参数<br>无需重启服务 |

</div>

---

## 系统架构

```mermaid
flowchart TB
    subgraph LockScreen["Windows 锁屏界面"]
        LogonUI["LogonUI.exe"]
        CP["FaceLogin<br/>CredentialProvider.dll"]
    end

    subgraph Service["人脸认证服务"]
        Svc["FaceLoginService.exe"]
    end

    subgraph Enrollment["注册控制台"]
        Console["FaceLoginConsole.exe"]
    end

    subgraph Storage["数据存储"]
        direction LR
        UsersDB[("data/<br/>users.dat")] ~~~ Config[("data/<br/>config.json")] ~~~ Models[("models/<br/>ONNX + landmarks")] ~~~ Logs[("log/<br/>日志文件")]
    end

    LogonUI -->|"COM 接口"| CP
    CP -->|"命名管道"| Svc
    Svc -->|"凭据回传"| CP
    Console -->|"RELOAD_DB / GET_LOGS"| Svc
    Console --> Storage
    Svc --> Storage

    style LockScreen fill:#eff6ff,stroke:#3b82f6
    style Service fill:#fefce8,stroke:#eab308
    style Enrollment fill:#f0fdf4,stroke:#22c55e
    style Storage fill:#fdf2f8,stroke:#ec4899
```

> 所有跨进程通信通过命名管道 `\\.\pipe\FaceLoginPipe`（DACL 保护）。

---

## Star History

<p align="center">
  <a href="https://github.com/EthanZer0/FaceLogin/stargazers">
    <img alt="Star History Chart" src="https://raw.githubusercontent.com/EthanZer0/StarHistory/main/svg/EthanZer0-FaceLogin.svg" width="80%">
  </a>
</p>

> 图表由独立项目 [StarHistory](https://github.com/EthanZer0/StarHistory) 的 GitHub Actions 每日自动更新，数据与渲染完全自托管，不依赖第三方服务。

---

## 快速开始

### 第一步：安装

从 [Releases](https://github.com/EthanZer0/FaceLogin/releases) 下载 `FaceLoginSetup.exe`，运行后选择安装目录，点击 **安装**。

### 第二步：录入人脸

以管理员身份运行 `FaceLoginConsole.exe`，按提示完成活体检测，输入密码，点击 **保存并录入**。

### 第三步：解锁

`Win + L` 锁屏后，注视摄像头，系统自动识别并解锁。

### 卸载

运行安装程序切换到 **卸载** 标签页，或手动执行 `regsvr32 /u FaceLoginCredentialProvider.dll`。

---

## 系统要求

| 要求 | 详情 |
|---|---|
| 操作系统 | Windows 10 21H2+ / Windows 11 (x64) |
| 摄像头 | USB 或内置，支持 1280×720 |
| 运行时 | WebView2（Windows 11 内置，Win10 自动安装） |
| 权限 | 管理员权限（安装和注册需要） |
| 磁盘空间 | ~200 MB（含模型文件 ~28 MB） |

---

## 安全设计

| 层面 | 措施 |
|---|---|
| 进程通信 | 命名管道 DACL：仅 SYSTEM + Administrators，拒绝远程 |
| 凭据存储 | DPAPI `CRYPTPROTECT_LOCAL_MACHINE` 机器范围加密 |
| 内存保护 | 密码使用后 `SecureZeroMemory` 即时擦除 |
| 活体检测 | EAR 眨眼 + MiniFASNetV2 双重验证 |
| 匹配安全 | 欧氏距离阈值 + 最佳/次佳匹配比双重校验 |
| 编译加固 | ASLR、DEP、CFG、64位高熵地址随机化 |

---

## 项目结构

```
FaceLogin/
├── common/                 # 公共库（日志、IPC协议、DPAPI、配置）
├── credential_provider/    # Windows 凭据提供程序 COM DLL
├── face_service/           # 人脸识别 Windows 服务
├── enrollment_app/         # 人脸录入控制台（WebView2 GUI）
├── installer/              # Go Wails 图形安装程序
├── scripts/                # 辅助脚本（模型下载、安装/卸载）
└── assets/                 # 图标资源
```

详细技术文档请参阅 [DEVELOPMENT.md](DEVELOPMENT.md)。

---

## 从源码构建

> 仅当需要自行编译时才需关注本节。

### 前置条件

- **Visual Studio 2022**（含 C++ 工作负载）
- **vcpkg** — dlib（图像工具库：matrix/rectangle/变换）、onnxruntime
- **Go 1.21+** + **Wails v2**（仅安装程序）
- **CMake 3.20+**

### C++ 组件

```powershell
# vcpkg 依赖（识别/检测/地标全部用 ONNX）
vcpkg install dlib[core] onnxruntime --triplet x64-windows

# 构建
cmake -B build -S . -G "Visual Studio 17 2022" `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

### Go 安装程序

```powershell
cd installer/FaceLoginSetup
# 将 C++ 产物和模型放入 resources/ 后编译
wails build -clean -platform windows/amd64
```

### 模型文件

| 文件 | 用途 | 下载 |
|---|---|---|
| `2d106det.onnx` | 106点面部地标提取 | [InsightFace](https://github.com/deepinsight/insightface) |
| `det_500m.onnx` | SCRFD 人脸检测 | [InsightFace](https://github.com/deepinsight/insightface) |
| `w600k_mbf.onnx` | InsightFace 人脸识别 | [InsightFace](https://github.com/deepinsight/insightface) |
| `minifas_quantized.onnx` | 静默反欺诈 | [facenox/face-antispoof-onnx](https://github.com/facenox/face-antispoof-onnx) |

---

## 贡献

欢迎提交 Issue 和 Pull Request！

- 代码规范：C++20、`/W4` 警告级别
- 提交前请确保构建通过
- 重大改动请先创建 Issue 讨论

---

## 开源协议

[MIT License](LICENSE) © 2026 美国伐木工&EthanZer0

---

## 免责声明

本软件通过人脸识别辅助 Windows 登录，但 **不能替代** 密码。人脸识别为便捷方式，系统始终保留密码登录作为后备。请勿在安全要求极高的环境中单独依赖人脸识别。
