# FaceLogin 技术开发文档

## 一、项目概述

### 1.1 项目简介

FaceLogin 是一个 Windows 人脸识别登录系统，允许用户通过摄像头人脸识别解锁 Windows 桌面。项目基于 Windows Credential Provider 框架实现锁屏/登录界面集成，使用 dlib 深度学习模型与 ONNX Runtime 进行人脸检测、识别与活体检测。

### 1.2 技术栈

| 层面 | 技术 |
|---|---|
| 编程语言 | C++20, Go (安装程序) |
| 构建系统 | CMake 3.20+ |
| 包管理 | vcpkg |
| 人脸检测 | dlib HOG + SCRFD ONNX |
| 地标提取 | dlib 68点 Shape Predictor |
| 人脸识别 | dlib ResNet-34 (128维) + InsightFace buffalo_s ONNX (128维) |
| 活体检测 | EAR眨眼检测 + MiniFASNetV2 静默反欺诈 |
| 相机采集 | Media Foundation / DirectShow |
| 凭据提供 | Windows Credential Provider COM (ICredentialProvider) |
| 进程通信 | 命名管道 (Named Pipe), UTF-16LE 编码 |
| 凭据加密 | DPAPI (CRYPTPROTECT_LOCAL_MACHINE) |
| 安装程序 | Go Wails v2 + Vue 3 |
| 控制台UI | WebView2 + HTML/CSS/JS (嵌入式资源) |
| 编译器 | MSVC 2022 (Visual Studio 2022) |

### 1.3 系统要求

- Windows 10 21H2+ / Windows 11
- x64 处理器
- USB 摄像头（或内置摄像头），支持 1280×720 分辨率
- 管理员权限（用于安装、注册COM组件、服务管理）
- WebView2 运行时（Windows 11 内置，Windows 10 自动安装）

---

## 二、项目结构

```
FaceLogin/
├── CMakeLists.txt                  # 根构建文件
├── vcpkg.json                      # vcpkg 依赖定义 (dlib, onnxruntime)
├── .gitignore
├── common/                         # 公共库 (facelogin_common)
│   ├── CMakeLists.txt
│   ├── logger.cpp/h                # 文件日志系统（线程安全）
│   ├── ipc_protocol.cpp/h          # 命名管道 IPC 协议定义与解析
│   ├── dpapi_util.cpp/h            # DPAPI 加密/解密工具
│   ├── secure_buffer.cpp/h         # 安全内存缓冲区 (RAII 自动清零)
│   ├── config_util.cpp/h           # 应用配置 JSON 序列化
│   └── registry_util.h             # 注册表读写工具
├── face_service/                   # 人脸识别 Windows 服务
│   ├── CMakeLists.txt
│   ├── main.cpp                    # 服务入口 (SCM / standalone)
│   ├── FaceService.cpp/h           # 服务核心逻辑 + 认证主循环
│   ├── face_detector.cpp/h         # HOG人脸检测 + 68点地标提取
│   ├── face_recognizer.cpp/h       # dlib ResNet-34 128维嵌入
│   ├── liveness_detector.cpp/h     # EAR眨眼活体检测
│   ├── liveness_types.h            # 活体检测方法枚举
│   ├── onnx_models.cpp/h           # ONNX 模型封装 (SCRFD / buffalo_s / MiniFASNet)
│   ├── webcam_capture.cpp/h        # Media Foundation 摄像头
│   ├── webcam_capture_dshow.cpp/h  # DirectShow 摄像头 (Session 0 服务模式)
│   ├── pipe_server.cpp/h           # 命名管道服务端 (DACL安全)
│   └── credential_store.cpp/h      # 用户凭据数据库 (V2格式, SID/UPN)
├── credential_provider/            # Windows 登录界面 COM 组件
│   ├── CMakeLists.txt
│   ├── dllmain.cpp                 # DLL 入口 + COM 注册/注销
│   ├── FaceLoginProvider.cpp/h     # ICredentialProvider 实现
│   ├── FaceLoginCredential.cpp/h   # ICredentialProviderCredential 状态机
│   ├── pipe_client.cpp/h           # 命名管道客户端 (异步状态推送)
│   ├── credential_provider.def     # DLL 导出定义
│   ├── resource.h                  # 资源ID定义 (含CLSID GUID)
│   └── resource.rc                 # 资源文件
├── enrollment_app/                 # 人脸注册控制台 (WebView2 GUI)
│   ├── CMakeLists.txt
│   ├── main.cpp                    # WinMain 入口 + 管理员权限检查
│   ├── EnrollmentWizard.cpp/h      # 注册向导后端 (摄像头/检测/活体/存储)
│   ├── CameraPreview.cpp/h         # 摄像头预览辅助
│   ├── WebviewHost.cpp/h           # WebView2 宿主 + IDispatch 桥接 (19个JS接口)
│   ├── index.html                  # 嵌入式前端 UI (录入/设置/日志)
│   ├── FaceLoginEnrollment.manifest # 高DPI感知清单
│   ├── resource.h                  # 资源ID
│   ├── resource.rc                 # 资源 (嵌入 index.html)
│   └── webview2/                   # WebView2 SDK 头文件
├── installer/                      # 安装程序 (Go Wails v2)
│   ├── FaceLoginSetup/
│   │   ├── app.go                  # 安装/卸载/文件夹选择逻辑
│   │   ├── main.go                 # Wails 入口
│   │   ├── frontend/src/App.vue    # Vue 3 安装界面
│   │   ├── internal/               # 内部工具包
│   │   │   ├── com.go              # COM DLL 注册/注销
│   │   │   ├── elevate.go          # 管理员权限提权
│   │   │   ├── extract.go          # 嵌入式资源提取
│   │   │   ├── scm.go              # Windows 服务管理
│   │   │   └── util.go             # 注册表操作 + 目录权限
│   │   └── resources/              # 部署文件 (编译时嵌入)
│   └── wails.json                  # Wails 项目配置
├── scripts/                        # 辅助脚本
│   ├── download_models.ps1         # dlib 模型文件下载
│   ├── install.bat                 # 命令行安装
│   ├── uninstall.bat               # 命令行卸载
│   ├── render_svg.js               # SVG → tile 像素数据转换
│   └── start_standalone.bat        # 开发模式快速启动
└── assets/                         # 静态资源 (图标等)
```

---

## 三、模块架构

### 3.1 整体架构图

```mermaid
graph TB
    subgraph LockScreen["Windows 锁屏界面"]
        LogonUI["LogonUI.exe"]
        CP["FaceLoginCredentialProvider.dll<br/>ICredentialProvider"]
    end

    subgraph Enrollment["人脸注册控制台"]
        Console["FaceLoginConsole.exe<br/>WebView2 + HTML"]
        subgraph EFuncs[" "]
            direction LR
            E1["摄像头预览 · 采集"]
            E2["活体检测"]
            E3["密码验证"]
            E4["DPAPI 加密存储"]
            E5["设置管理 · 日志"]
        end
    end

    subgraph Service["人脸认证服务"]
        Svc["FaceLoginService.exe<br/>Windows Service"]
        subgraph SFuncs[" "]
            direction LR
            S1["摄像头采集"]
            S2["人脸检测"]
            S3["活体检测"]
            S4["人脸识别"]
            S5["凭据回传"]
        end
    end

    subgraph Storage["数据存储"]
        direction LR
        UsersDB["users.dat<br/>V2 加密凭据"]
        Models["models/<br/>*.dat + *.onnx"]
        Config["config.json<br/>热配置"]
        Logs["*.log<br/>日志文件"]
    end

    LogonUI -->|"COM 调用"| CP
    CP <==>|"命名管道<br/>\\.\pipe\FaceLoginPipe"| Svc
    Console <==>|"命名管道<br/>RELOAD_DB / CONFIG_RELOAD / GET_LOGS"| Svc
    Console -->|"读写"| Storage
    Svc -->|"读写"| Storage

    style LockScreen fill:#f0f9ff,stroke:#0284c7
    style Enrollment fill:#f0fdf4,stroke:#16a34a
    style Service fill:#fefce8,stroke:#ca8a04
    style Storage fill:#fdf2f8,stroke:#db2777
```

### 3.2 数据流

#### 认证流程 (Login / Unlock)

```mermaid
sequenceDiagram
    participant User as 👤 用户
    participant LogonUI as LogonUI
    participant CP as Credential Provider DLL
    participant Pipe as 命名管道
    participant Svc as FaceLoginService
    participant DB as users.dat
    participant LSA as Windows LSA

    User->>LogonUI: Win+L 锁屏 / 开机
    LogonUI->>CP: 加载 COM 组件
    CP->>DB: 读取已注册用户数
    DB-->>CP: userCount > 0
    CP->>LogonUI: 显示 "人脸登录" 磁贴 (自动选中)
    LogonUI->>CP: SetSelected → GetSerialization (轮询)
    CP->>Pipe: AUTH_REQUEST
    Pipe->>Svc: 转发请求
    Svc->>Svc: 初始化摄像头
    loop 每帧 (~30fps, 最长15s)
        Svc->>Svc: 抓帧 → 人脸检测 → 地标提取
        Svc->>Svc: 活体检测 (眨眼 / 反欺诈)
        Svc->>Svc: 计算128维嵌入
        Svc->>DB: 匹配嵌入向量 (欧氏距离)
        alt 匹配成功
            DB-->>Svc: user + 加密密码
            Svc->>Svc: DPAPI 解密密码
            Svc-->>Pipe: STATUS: 识别成功
            Svc-->>Pipe: AUTH_SUCCESS:SID:UPN:DOMAIN\USER:PASSWORD
            Pipe-->>CP: 凭据
            CP->>CP: CredPackAuthenticationBufferW 打包
            CP-->>LogonUI: KerbInteractiveLogon 序列化
            LogonUI->>LSA: 验证凭据
            LSA-->>LogonUI: STATUS_SUCCESS
            LogonUI->>User: ✅ 桌面解锁
        else 超时
            Svc-->>Pipe: AUTH_TIMEOUT
            Pipe-->>CP: 超时
            CP->>LogonUI: "未识别到人脸，请重试"
        end
    end
```

#### 注册流程 (Enrollment)

```mermaid
sequenceDiagram
    participant User as 👤 用户
    participant App as FaceLoginConsole
    participant Cam as 摄像头 (MF)
    participant Detector as 人脸检测+识别
    participant Store as users.dat
    participant Pipe as 命名管道
    participant Svc as FaceLoginService

    User->>App: 以管理员运行
    App->>App: 获取用户身份<br/>(GetUserNameW + IdentityStore)
    App->>Cam: StartPreview()
    loop 渲染循环 (~30fps)
        App->>Cam: GrabFrame()
        App->>Detector: 人脸检测 + 地标
        App-->>User: Canvas 实时预览 + 人脸框叠加
    end
    User->>App: 点击 "开始采集"
    App->>Detector: 活体检测 (眨眼 / 反欺诈)
    Detector-->>App: 活体通过 ✅
    loop 采集 10 帧
        App->>Cam: GrabFrame()
        App->>Detector: 计算128维嵌入
        App->>App: 保存嵌入向量
    end
    App->>App: 计算平均嵌入 + 一致性检查
    App->>User: 显示 UPN / 账户类型 / SID
    User->>App: 输入 Windows 密码
    App->>App: LogonUserW 验证密码
    App->>App: DPAPI 加密密码
    App->>Store: 写入 V2 记录 (username/UPN/SID/password/embedding)
    App->>Pipe: RELOAD_DB
    Pipe->>Svc: 通知重载
    Svc->>Store: 重新加载数据库
    Svc-->>Pipe: RELOAD_OK
    App-->>User: ✅ 录入成功
```

---

## 四、公共库 — `common/`

### 4.1 日志系统 (`logger.h/cpp`)

单例模式日志系统，线程安全（CRITICAL_SECTION）。

```cpp
namespace facelogin {
enum class LogLevel { Debug, Info, Warning, Error };

class Logger {
public:
    static Logger& Instance();
    void SetLogFile(const std::wstring& path);
    void SetMinLevel(LogLevel level);
    void SetEnableDebugOutput(bool enable);  // 同时输出到 DebugOutput
    void Log(LogLevel level, const wchar_t* format, ...);
};
}

// 便捷宏 (自动携带 __FUNCTION__ 和 __LINE__)
FACELOGIN_DEBUG(L"...");
FACELOGIN_INFO(L"...");
FACELOGIN_WARN(L"...");
FACELOGIN_ERROR(L"...");
```

**特性**：
- 同时输出到文件和控制台 (Debug 模式)
- 时间戳精度到毫秒
- 线程安全写入
- 每个进程独立日志文件 (service.log / credential_provider.log / enrollment.log)

### 4.2 IPC 协议 (`ipc_protocol.h/cpp`)

传输层基于 Windows 命名管道 `\\.\pipe\FaceLoginPipe`。

| 消息 | 格式 | 说明 |
|---|---|---|
| `AUTH_REQUEST` | 纯文本 | 凭据提供方发起认证请求 |
| `AUTH_SUCCESS:SID:UPN:DOMAIN\USER:PASSWORD` | 冒号分隔 (≥3个) | 认证成功，返回凭据（V2格式含SID/UPN） |
| `AUTH_SUCCESS:DOMAIN\USER:PASSWORD` | 冒号分隔 (1个) | 旧格式（V1向后兼容） |
| `AUTH_TIMEOUT` | 纯文本 | 15秒内未检测到匹配人脸 |
| `AUTH_NO_FACE` | 纯文本 | 检测超时无匹配 |
| `AUTH_ERROR:message` | 前缀+消息 | 错误状态 |
| `AUTH_CANCELLED` | 纯文本 | 用户取消 |
| `STATUS:text` | 前缀+消息 | 实时状态推送 |
| `RELOAD_DB` / `RELOAD_OK` | 纯文本 | 重载用户数据库 |
| `CONFIG_RELOAD` / `CONFIG_RELOAD_OK` | 纯文本 | 重载配置文件 |
| `GET_LOGS` / `GET_LOGS_OK:json` | 纯文本/JSON | 获取服务端日志 |
| `PING` / `PONG` | 纯文本 | 连接存活检测 |

**安全措施**：
- DACL: 仅 SYSTEM + Administrators 可连接
- `PIPE_REJECT_REMOTE_CLIENTS`: 拒绝远程客户端
- 缓冲区大小: 4096 字节
- 超时: 30 秒
- 密码传输后立即 `SecureZeroMemory` 擦除

**AuthResult 结构**:
```cpp
struct AuthResult {
    enum class Status { Success, Timeout, NoFace, Error, Cancelled };
    Status status;
    std::wstring sid;      // S-1-5-21-... (V2)
    std::wstring upn;      // user@domain (V2, 可为空)
    std::wstring domain;
    std::wstring username;
    std::wstring password; // 使用后清零!
    std::wstring errorMessage;
};
```

### 4.3 DPAPI 加密 (`dpapi_util.h/cpp`)

使用 Windows Data Protection API。

- **Protect()**: `CRYPTPROTECT_LOCAL_MACHINE` — 机器范围加密，SYSTEM 账户和服务均可解密
- **Unprotect()**: 解密已保护的数据
- 加密后数据以二进制格式存入 `users.dat`

### 4.4 安全缓冲区 (`secure_buffer.h/cpp`)

RAII 自动清零内存管理。

```cpp
template<typename T>
class SecureBuffer {
    // 析构时自动调用 SecureZeroMemory
    // 禁用拷贝 (non-copyable)
};
```

### 4.5 配置系统 (`config_util.h/cpp`)

```cpp
struct AppConfig {
    std::string    recognition_model      = "both";   // "dlib" / "onnx" / "both"
    std::string    detector               = "scrfd";  // "dlib_hog" / "scrfd"
    LivenessMethod liveness_method        = LivenessMethod::Blink;
    float          match_threshold        = 0.45f;
    float          anti_spoof_threshold   = 0.50f;
};

enum class LivenessMethod {
    Blink,       // EAR 眨眼检测
    AntiSpoof,   // ONNX 静默反欺诈 (MiniFASNetV2)
    None         // 无活体检查 (不安全)
};
```

配置文件位置: `%PROGRAMDATA%\FaceLogin\data\config.json`

---

## 五、人脸识别服务 — `face_service/`

### 5.1 服务入口 (`main.cpp`)

```
用法:
  FaceLoginService.exe                   作为 Windows 服务运行 (SCM)
  FaceLoginService.exe -install          安装服务
  FaceLoginService.exe -uninstall        卸载服务
  FaceLoginService.exe -standalone       前台运行 (开发测试)
```

**单实例保护**: 全局命名互斥体 `Global\FaceLoginService_SingleInstance`

### 5.2 服务核心 (`FaceService.h/cpp`)

**生命周期**:

```
ServiceMain()
  ├─ RegisterServiceCtrlHandlerEx()
  ├─ Initialize()
  │   ├─ 创建数据目录 + 加载配置
  │   ├─ 加载凭据数据库 (CredentialStore, V2 with SID/UPN)
  │   ├─ 初始化人脸检测器 (FaceDetector / OnnxDetector)
  │   ├─ 初始化人脸识别器 (FaceRecognizer / OnnxRecognizer)
  │   ├─ 初始化活体检测器 (LivenessDetector / OnnxAntiSpoof)
  │   ├─ 初始化摄像头 (DirectShow 用于服务模式 / MF 用于 standalone)
  │   └─ 创建管道服务端 (PipeServer)
  └─ Run()
      └─ 循环: WaitForClient → ReadMessage → ProcessAuthRequest → Disconnect

服务控制:
  - SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
  - 故障恢复: 3次重启, 间隔60秒, 重置周期24小时
```

**认证流程 (`ProcessAuthRequest`)**:

```
1. 检查注册用户数 > 0
2. 根据配置选择检测器/识别器/活体方法
3. 延时初始化摄像头 (仅在收到认证请求时打开，避免摄像头占用)
4. 丢弃前10帧 (自动曝光调整)
5. 重置活体检测器
6. 循环 (最长时间 m_authTimeoutSeconds = 15秒):
   a. 抓取一帧
   b. 人脸检测 (SCRFD ONNX 或 dlib HOG)
   c. 检测最大人脸
   d. 提取68点地标
   e. 活体检测 (眨眼EAR 或 静默反欺诈)
   f. 计算128维嵌入向量 (ONNX buffalo_s 或 dlib ResNet)
   g. 数据库匹配 (欧氏距离 < 阈值 + 最佳/次佳比)
   h. 匹配成功 → 发送 STATUS: 识别成功 → 构建 AUTH_SUCCESS → 发送凭据 → 退出
   i. 匹配失败 → 继续循环
7. 超时 → 发送 AUTH_TIMEOUT
8. 完成后关闭摄像头，释放资源
```

**双模式摄像头**:

| 属性 | Media Foundation (MF) | DirectShow (DS) |
|---|---|---|
| 使用场景 | standalone 模式 + enrollment | 服务模式 (Session 0) |
| COM线程模型 | MTA | COINIT_MULTITHREADED |
| 颜色格式 | NV12 → RGB | RGB24 |
| Session 0 支持 | ❌ | ✅ |
| 分辨率 | 1280×720 | 1280×720 |

**配置项** (通过 config.json + `CONFIG_RELOAD` 热加载):

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `recognition_model` | `"both"` | 识别模型: dlib / onnx / both |
| `detector` | `"scrfd"` | 检测器: dlib_hog / scrfd |
| `liveness_method` | `"blink"` | 活体方法: blink / antispoof / none |
| `match_threshold` | 0.45 | 欧氏距离阈值 (越小越严格) |
| `anti_spoof_threshold` | 0.50 | 反欺诈阈值 (越高越严格) |

### 5.3 人脸检测 (`face_detector.h/cpp`)

```cpp
class FaceDetector {
    dlib::frontal_face_detector m_hogDetector;  // HOG检测器 (内置)
    dlib::shape_predictor m_shapePredictor;       // 68点地标模型
};
```

**初始化**: 加载 `shape_predictor_68_face_landmarks.dat` (~97 MB)

**方法**:
- `Detect()`: 返回所有检测到的人脸 + 68点地标
- `DetectLargestFace()`: 返回面积最大的人脸（离摄像头最近）
- `GetLandmarks()`: 对给定矩形提取地标

### 5.4 人脸识别 (`face_recognizer.h/cpp`)

```cpp
class FaceRecognizer {
    FaceNetType m_net;  // ResNet-34 架构 (dlib_face_recognition_resnet_model_v1.dat)
};
```

**初始化**: 加载 `dlib_face_recognition_resnet_model_v1.dat` (~22 MB)

**嵌入计算**: 输入 RGB 帧 + 地标 → 输出 128 维浮点向量

**匹配**: 欧氏距离比对，默认阈值 0.45。同时检查最佳匹配 / 次佳匹配比 < 0.75（防误匹配）。

### 5.5 活体检测 (`liveness_detector.h/cpp`)

基于 **Eye Aspect Ratio (EAR)** 的眨眼检测:

```
EAR = (||P2-P6|| + ||P3-P5||) / (2 * ||P1-P4||)

地标索引 (dlib 68点):
  左眼: 36-41, 右眼: 42-47
  EAR_avg = (EAR_left + EAR_right) / 2
```

**参数**:
- 闭眼阈值: EAR < 0.20
- 确认帧数: 连续 4 帧
- 正常 EAR 范围: 睁开 ~0.22-0.30, 闭合 ~0.12-0.17

### 5.6 ONNX 模型 (`onnx_models.h/cpp`)

封装三个 ONNX 推理引擎:

| 类 | 模型 | 输入 | 输出 | 用途 |
|---|---|---|---|---|
| `OnnxDetector` | SCRFD (`det_500m.onnx`) | 图像 (letterbox) | 检测框+5点关键点 | 人脸检测 |
| `OnnxRecognizer` | InsightFace buffalo_s (`w600k_mbf.onnx`) | 112×112 对齐人脸 | 128维嵌入 | 人脸识别 |
| `OnnxAntiSpoof` | MiniFASNetV2 (`OULU_Protocol_2_model_0_0.onnx`) | 80×80 对齐人脸 | 活体分数 [0,1] | 静默反欺诈 |

所有 ONNX 模型放置在 `%PROGRAMDATA%\FaceLogin\models\` 下。

### 5.7 凭据存储 (`credential_store.h/cpp`)

**V2 二进制文件格式** (`users.dat`):

```
[Header]
  magic:     uint32_t  0x474F4C46 ("FLOG")
  version:   uint32_t  2
  count:     uint32_t  (用户数量)

[Records] × count
  usernameLen:    uint32_t
  username:       wchar_t[usernameLen]    (UTF-16LE)
  upnLen:         uint32_t                (V2 new)
  upn:            wchar_t[upnLen]         (V2 new, e.g. "user@outlook.com")
  sidLen:         uint32_t                (V2 new)
  sid:            wchar_t[sidLen]         (V2 new, e.g. "S-1-5-21-...")
  passwordLen:    uint32_t
  encryptedPass:  uint8_t[passwordLen]    (DPAPI 加密)
  embedding:      float[128]              (512 bytes)
```

**V1 向后兼容**: V1 数据库加载时，通过 `LookupAccountNameW` 和 IdentityStore 注册表自动补充 SID/UPN 字段。

**线程安全**: 所有操作在调用者持有锁的前提下执行。服务端在主循环中串行处理请求，无并发写入场景。

**MatchResult**: 匹配时返回 `username / upn / sid / password(解密后) / distance`，密码使用后立即 `SecureZeroMemory` 擦除。

### 5.8 命名管道服务端 (`pipe_server.h/cpp`)

```cpp
class PipeServer {
    bool WaitForClient(DWORD timeoutMs = 30000);
    bool ReadMessage(std::wstring& outMessage, DWORD timeoutMs = 30000);
    bool WriteMessage(const std::wstring& message);
    void Disconnect();
    void Close();
};
```

**安全措施**:
- `SECURITY_ATTRIBUTES` 带自定义 DACL：仅 SYSTEM + Administrators
- `PIPE_REJECT_REMOTE_CLIENTS`
- 管道实例: 1（单客户端模型，串行服务）
- 缓冲区: 4096 字节

---

## 六、凭据提供方 — `credential_provider/`

### 6.1 COM 注册 (`dllmain.cpp`)

**CLSID**: `{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}`

**注册路径**:
```
HKEY_CLASSES_ROOT\CLSID\{GUID}\InprocServer32 → DLL 路径 (Apartment 模型)
HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\
  Authentication\Credential Providers\{GUID} → "FaceLogin Credential Provider"
```

**导出函数**: `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`

### 6.2 凭据提供方 (`FaceLoginProvider.h/cpp`)

实现 `ICredentialProvider` 接口。

**磁贴字段** (4个):

| 字段ID | 类型 | 标签 | 说明 |
|---|---|---|---|
| 0 | CPFT_LARGE_TEXT | 人脸登录 | 磁贴标题 |
| 1 | CPFT_SMALL_TEXT | 状态 | 实时状态信息 |
| 2 | CPFT_SUBMIT_BUTTON | 提交 | 隐藏的提交按钮 |
| 3 | CPFT_COMMAND_LINK | 切换到密码登录 | 备用登录方式 |

**自动登录**: `GetCredentialCount()` 返回 `pbAutoLogonWithDefault = TRUE`，系统自动选中此凭据。

**场景支持**: 支持 `CPUS_LOGON` 和 `CPUS_UNLOCK_WORKSTATION`。

**用户检测**: `ReadUserCountFromDatabase()` 读取 `users.dat` (支持 V1/V2)，无注册用户时返回 `E_NOTIMPL` 隐藏磁贴。

**MSA 支持**: `GetMSAUpnFromIdentityStore()` 从注册表 `HKLM\SOFTWARE\Microsoft\IdentityStore\LogonCache\...\Name2Sid` 读取 MSA UPN。

### 6.3 凭据磁贴 (`FaceLoginCredential.h/cpp`)

实现 `ICredentialProviderCredential` 接口，核心状态机:

```
状态转换:
  Waiting ──→ Authenticating ──→ Ready (认证成功, 凭据回传)
     │              │
     └──────────────┴────→ Failed (识别失败, 超时)
                           Error  (服务不可用)
```

**凭据打包 (`PackCredentials`)**:

1. 使用 `LsaConnectUntrusted` + `LsaLookupAuthenticationPackage("MICROSOFT_AUTHENTICATION_PACKAGE_V1_0")` 获取认证包
2. `CredPackAuthenticationBufferW(flags=0)` 打包 KERB_INTERACTIVE_LOGON
3. 本地账户: `Domain\Username` 格式
4. MSA 账户: 如有 UPN (含 `@`)，使用 UPN 格式
5. 打包后的凭据通过 `KerbInteractiveLogon` 序列化返回给 LSA

**多线程设计**:
- 主线程: LogonUI 调用 COM 接口方法
- 后台线程: 阻塞式 `ReadFile` 等待管道响应
- 同步: `CRITICAL_SECTION` 保护状态变量, `HANDLE m_hCredsReady` 事件通知
- 超时: 20 秒硬超时，防止阻塞 LogonUI

**状态文本** (中文):

| 状态 | 显示文本 |
|---|---|
| Waiting | 正在准备人脸识别... |
| Authenticating | 请注视摄像头以解锁 |
| Ready | 人脸识别成功，正在解锁... |
| Failed | 未识别到人脸，请重试或使用密码登录 |
| Error | 人脸登录服务不可用 |

### 6.4 管道客户端 (`pipe_client.h/cpp`)

```cpp
class PipeClient {
    bool Connect(DWORD timeoutMs = 5000);
    void StartBackgroundRead();      // 启动后台阻塞读取线程
    bool CheckResponse();            // 非阻塞轮询
};
```

**实时状态推送**: `STATUS:` 消息通过回调立即传递到 UI 更新显示文本，其余消息写入缓冲区并触发事件。

---

## 七、注册控制台 — `enrollment_app/`

### 7.1 程序入口 (`main.cpp`)

Win32 GUI 应用程序。

- 运行时检查管理员权限 (DPAPI 机器范围 + %PROGRAMDATA% 写入需要)
- 非管理员时自动通过 `ShellExecuteEx(runas)` 提权重启
- 检查模型文件是否存在（缺失时弹出提示）

### 7.2 注册向导 (`EnrollmentWizard.h/cpp`)

**身份获取** (构造函数):

```
1. GetUserNameW → SAM 用户名
2. GetUserNameExW(NameUserPrincipal) → UPN (secur32.dll 动态绑定)
3. GetUserNameExW(NameSamCompatible) → DOMAIN\User 格式
4. LookupAccountNameW → SID (通过 UPN 或 SAM 用户名)
5. 注册表 IdentityStore 回退 → MSA UPN (当 GetUserNameExW 失败时)
6. 账户类型判断: UPN 含 '@' → "msa", 否则 → "local"
```

**页面一：人脸采集**

- 摄像头 MF 采集，30fps 回调
- 实时人脸检测 (SCRFD ONNX 或 dlib HOG) + 68点地标叠加
- 采集流程:
  1. 活体检测 (眨眼 / 反欺诈，根据配置)
  2. 活体通过 → 采集 10 帧人脸嵌入向量
  3. 嵌入一致性检查 (最大最小距离 < 阈值)
  4. 计算 10 帧平均嵌入

**页面二：密码录入**

- WebView2 界面显示 UPN、账户类型 (local/msa)、SID
- 密码验证: `LogonUserW` 支持本地账户和 MSA UPN 回退
- DPAPI 加密密码 → 更新 `users.dat` V2 格式 (含 SID/UPN)
- 通过命名管道 `RELOAD_DB` 通知服务热加载

**JS 接口** (通过 COM IDispatch，共 19 个 dispId):

| dispId | 方法 | 说明 |
|---|---|---|
| 1 | StartPreview | 启动摄像头预览 |
| 2 | StopPreview | 停止摄像头预览 |
| 3 | GetSampleCount | 获取采集样本数 |
| 4 | GetUsername | 获取用户名 (UPN/DOMAIN\User) |
| 5 | CaptureFaceSamples | 触发采集 (阻塞) |
| 6 | ValidatePassword | 验证 Windows 密码 |
| 7 | SaveEnrollment | 保存注册数据 |
| 8 | GetLatestFrameBase64 | 获取当前帧 JPEG base64 |
| 9 | GetLatestFacesJson | 获取检测面部的 JSON |
| 10 | IsRunning | 预览是否运行中 |
| 11 | IsLivenessPassed | 活体检测是否通过 |
| 12 | IsLivenessChecking | 活体检测是否进行中 |
| 13 | GetConfig | 获取当前配置 JSON |
| 14 | SetConfig | 保存配置 JSON |
| 15 | GetLogLines | 获取控制台日志 JSON 数组 |
| 16 | GetServiceLogLines | 获取服务端日志 JSON 数组 |
| 17 | ClearLog | 清空日志 |
| 18 | GetUserSid | 获取当前用户 SID |
| 19 | GetAccountType | 获取账户类型 (local/msa) |

### 7.3 WebView2 宿主 (`WebviewHost.h/cpp`)

- 创建 `ICoreWebView2Environment` + `ICoreWebView2Controller`
- 从嵌入资源加载 `index.html`
- 注册 `HostObject` (COM IDispatch) 作为 JS `window.chrome.webview.hostObjects.sync.host`
- 处理 `WM_WTSSESSION_CHANGE`: 锁屏时释放摄像头，解锁时恢复
- 禁用右键菜单和开发者工具

### 7.4 前端界面 (`index.html`)

嵌入式单页应用，三个标签页:

| 标签 | 功能 |
|---|---|
| 录入 | 摄像头预览 + Canvas 渲染 + 人脸框叠加 + 活体提示 + 采集进度 |
| 设置 | 识别模型 / 检测器 / 活体方法 / 反欺诈阈值 / 匹配严格度 |
| 日志 | Console 日志 / Service 日志切换 + 自动刷新 + 彩色等级显示 |

---

## 八、安装程序 — `installer/`

### 8.1 技术架构

基于 **Go Wails v2** 构建，前端使用 **Vue 3** 单文件组件。

| 层面 | 技术 |
|---|---|
| 后端 | Go + Wails v2 Runtime |
| 前端 | Vue 3 + Tailwind CSS + TypeScript |
| 打包 | Wails 构建 (Go 编译 + WebView2 嵌入) |
| 资源 | Go embed.FS 嵌入所有部署文件 (~220 MB) |

### 8.2 命令行用法

```
FaceLoginSetup.exe          交互模式 (GUI)
```

### 8.3 安装流程

| 步骤 | 操作 | 进度 |
|---|---|---|
| 1 | 停止并删除已有服务 | 0-12% |
| 2 | 创建目标目录 | 12-25% |
| 3 | 写入注册表路径 (InstallPath, DataPath) | 25-30% |
| 4 | 提取所有嵌入文件 (~220MB) | 30-60% |
| 5 | 写入默认 config.json | 60% |
| 6 | 设置数据目录 ACL | 60-67% |
| 7 | 注册 COM DLL (regsvr32) | 67-75% |
| 8 | 安装并启动 Windows 服务 | 75-90% |
| 9 | 最终化 | 90-100% |

### 8.4 卸载流程

| 步骤 | 操作 | 进度 |
|---|---|---|
| 1 | 停止并删除服务 | 0-30% |
| 2 | 注销 COM DLL | 30-50% |
| 3 | 删除安装目录 (程序文件) | 50-70% |
| 4 | 清理注册表键值 | 70-85% |
| 5 | 完成 (保留用户数据) | 85-100% |

### 8.5 特殊功能

- **文件夹选择器**: 通过 `runtime.OpenDirectoryDialog` 调用原生文件夹选择器
- **安装检测**: 检查注册表 `InstallPath` 值 + 目录存在性，已安装时标签显示"更新"
- **进度推送**: 通过 Wails Events 实时推送安装进度到 Vue 前端

### 8.6 目录结构

```
C:\Program Files\FaceLogin\               # 安装目录 (用户可选)
├── FaceLoginService.exe
├── FaceLoginCredentialProvider.dll
├── FaceLoginConsole.exe
├── FaceLoginSetup.exe
├── openblas.dll
├── onnxruntime.dll
├── abseil_dll.dll
├── libprotobuf.dll
├── libprotobuf-lite.dll
├── re2.dll
├── libgfortran-5.dll
├── libquadmath-0.dll
├── libgcc_s_seh-1.dll
├── libwinpthread-1.dll
├── data/
│   ├── config.json
│   └── users.dat
├── log/
│   ├── service.log
│   ├── credential_provider.log
│   └── enrollment.log
└── models/
    ├── shape_predictor_68_face_landmarks.dat       (~97 MB)
    ├── dlib_face_recognition_resnet_model_v1.dat    (~22 MB)
    ├── det_500m.onnx                                 (~16 MB)
    ├── w600k_mbf.onnx                                 (~6 MB)
    └── OULU_Protocol_2_model_0_0.onnx                 (~1 MB)

C:\ProgramData\FaceLogin\                   # 数据目录
├── data/
│   ├── config.json                        # 热配置
│   └── users.dat                          # 加密凭据数据库
└── log/
    ├── service.log
    ├── enrollment.log
    └── credential_provider.log
```

---

## 九、模型文件

| 文件 | 大小 | 用途 | 来源 |
|---|---|---|---|
| `shape_predictor_68_face_landmarks.dat` | ~97 MB | 68点面部地标提取 | dlib.net |
| `dlib_face_recognition_resnet_model_v1.dat` | ~22 MB | dlib ResNet-34 128维嵌入 | dlib.net |
| `det_500m.onnx` | ~16 MB | SCRFD 人脸检测 | InsightFace |
| `w600k_mbf.onnx` | ~6 MB | buffalo_s MobileFaceNet 128维嵌入 | InsightFace |
| `OULU_Protocol_2_model_0_0.onnx` | ~1 MB | MiniFASNetV2 静默反欺诈 | MiniFASNet |

下载脚本: `scripts/download_models.ps1` 可下载 dlib 模型文件。ONNX 模型文件需从 InsightFace 项目自行下载。

---

## 十、构建与部署

### 10.1 依赖

**vcpkg**:
```
dlib[core]
onnxruntime
```

**系统库**:
- Media Foundation: `mfplat`, `mf`, `mfreadwrite`, `mfuuid`
- DirectShow: `strmiids`, `strmif`
- COM: `ole32`, `oleaut32`
- 凭据: `credui`
- 安全: `advapi32`, `crypt32`
- LSA: `secur32`
- 图形: `gdi32`, `comctl32`, `windowscodecs`
- IPC: `kernel32`
- Shell: `shell32`, `shlwapi`, `shlobj`
- Network: `netapi32`

**Go 依赖** (安装程序):
- Wails v2 (`github.com/wailsapp/wails/v2`)
- `golang.org/x/sys/windows`

### 10.2 构建

```powershell
# === C++ 组件 ===

# 配置
cmake -B build -S . -G "Visual Studio 17 2022" `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# 编译所有目标
cmake --build build --config Release

# === Go 安装程序 ===

# 进入安装程序目录
cd installer\FaceLoginSetup

# 确保资源文件就位 (将构建产物 + 模型文件放入 resources\)
# 然后构建
wails build -clean -platform windows/amd64
```

### 10.3 构建产物

| 目标 | 输出路径 | 平台 |
|---|---|---|
| `FaceLoginService.exe` | `build/face_service/Release/` | C++ MSVC x64 |
| `FaceLoginCredentialProvider.dll` | `build/credential_provider/Release/` | C++ MSVC x64 |
| `FaceLoginConsole.exe` | `build/enrollment_app/Release/` | C++ MSVC x64 |
| `FaceLoginSetup.exe` | `installer/FaceLoginSetup/bin/` | Go Wails x64 |

### 10.4 部署

将所有构建产物和模型文件放入 `installer/FaceLoginSetup/resources/`，然后构建安装程序。

运行 `FaceLoginSetup.exe`，选择安装目录，点击安装即可。

---

## 十一、安全设计

| 层面 | 措施 |
|---|---|
| 进程通信 | 命名管道 DACL 限制 SYSTEM + Administrators |
| 凭据存储 | DPAPI 机器范围加密 (`CRYPTPROTECT_LOCAL_MACHINE`) |
| 内存保护 | SecureBuffer RAII 自动 `SecureZeroMemory` |
| 管道安全 | `PIPE_REJECT_REMOTE_CLIENTS` 拒绝远程连接 |
| 单实例 | 全局互斥体防止多个服务实例 |
| DLL 安全 | `/DYNAMICBASE` (ASLR), `/NXCOMPAT` (DEP), `/GUARD:CF` (CFG), `/HIGHENTROPYVA` (64位) |
| 密码验证 | `LogonUserW(LOGON32_LOGON_NETWORK)` 轻量验证，不缓存凭据 |
| 活体检测 | EAR 眨眼检测 + MiniFASNetV2 反欺诈，防止照片/视频攻击 |
| 匹配安全 | 欧氏距离阈值 + 最佳/次佳匹配比双重验证 |

---

## 十二、账户兼容性

### 12.1 支持的账户类型

| 账户类型 | 登录 | 注册 | 说明 |
|---|---|---|---|
| 本地 SAM 账户 | ✅ | ✅ | `COMPUTERNAME\Username` 格式 |
| 微软在线账户 (MSA) | ✅ | ✅ | `user@outlook.com` UPN 格式 |
| 域账户 (Active Directory) | 理论支持 | 理论支持 | 使用 Kerberos 认证 |

### 12.2 MSA 实现细节

**身份获取**: `GetUserNameExW(NameUserPrincipal)` 在 MSA 关联机器上返回 `ERROR_NO_SUCH_USER` (1332)，因此增加了注册表回退方案：
- 读取 `HKLM\SOFTWARE\Microsoft\IdentityStore\LogonCache\D7F9888F-E3FC-49b0-9EA6-A85B5F392A4F\Name2Sid\{hash}` 中的 `IdentityName` 值
- 该值为 MSA 邮箱地址 (UPN 格式)

**凭据打包**: 本地账户使用 `Domain\Username` 格式，MSA 账户使用 UPN `user@domain.com` 格式。均使用 `MICROSOFT_AUTHENTICATION_PACKAGE_V1_0` 认证包。

**数据存储**: V2 数据库同时存储 username、UPN 和 SID，按 SID 优先匹配。

---

## 十三、故障处理

### 13.1 日志文件

所有日志位于 `%ProgramData%\FaceLogin\log\`:

| 日志文件 | 来源 |
|---|---|
| `service.log` | 人脸识别服务 |
| `credential_provider.log` | 登录界面组件 |
| `enrollment.log` | 注册控制台 |

### 13.2 常见问题

| 问题 | 可能原因 | 解决方法 |
|---|---|---|
| 服务启动超时 | 模型加载慢 (~30s) | 正常现象，后台继续启动 |
| 服务启动失败 | 缺少运行时 DLL | 安装时确保 DLL 与 EXE 同目录 |
| 锁屏不显示磁贴 | 未注册或已禁用 / 无注册用户 | 检查注册表 Disabled 键值，确认已录入人脸 |
| 识别率低 | 光照不足 / 嵌入质量差 | 重新注册人脸，确保光线均匀 |
| 摄像头不工作 | Session 0 权限 | 服务模式使用 DirectShow |
| 人脸登录后用户名密码错误 | MSA 账户凭据格式不对 | 确认 V2 数据库含正确 UPN |
| 注册时显示空白 UPN | MSA 账户 GetUserNameExW 失败 | 已通过 IdentityStore 回退解决 |

---

## 十四、开发指南

### 14.1 本地开发模式

```cmd
REM 1. 下载模型
powershell -File scripts\download_models.ps1

REM 2. 停止已有服务
sc stop FaceLoginService

REM 3. 以 standalone 模式运行服务 (前台 + Debug 输出)
FaceLoginService.exe -standalone

REM 4. 部署 DLL 并注册
regsvr32 build\credential_provider\Release\FaceLoginCredentialProvider.dll

REM 5. Win+L 锁屏测试
```

### 14.2 编码规范

- C++20 标准, `/W4 /WX-` 警告级别
- CRITICAL_SECTION 用于线程同步
- `FACELOGIN_*` 宏用于日志
- 中文字符串需要 MSVC `/utf-8` 编译选项
- 错误处理: 返回 `bool`，通过日志记录详细错误
- Go 代码遵循标准 Go 风格

---

## 附录A：IPC 消息格式详解

```
# 认证请求 (客户端 → 服务端)
AUTH_REQUEST

# 认证成功，V2 格式 (服务端 → 客户端)
AUTH_SUCCESS:S-1-5-21-xxx:user@outlook.com:DESKTOP-XXX\username:password123

# 认证成功，V1 格式 (向后兼容)
AUTH_SUCCESS:DESKTOP-XXX\username:password123

# 状态推送 (服务端 → 客户端)
STATUS:正在检测人脸...
STATUS:请注视摄像头，保持面部清晰可见
STATUS:眨眼验证通过，正在进行身份确认...
STATUS:识别成功

# 认证超时
AUTH_TIMEOUT

# 无匹配人脸
AUTH_NO_FACE

# 错误
AUTH_ERROR:没有注册用户

# 数据库重载 (注册程序 → 服务端)
RELOAD_DB
RELOAD_OK                       # 服务端响应

# 配置重载 (控制台 → 服务端)
CONFIG_RELOAD
CONFIG_RELOAD_OK                # 服务端响应

# 日志获取 (控制台 → 服务端)
GET_LOGS
GET_LOGS_OK:["line1","line2",...]  # JSON 字符串数组

# 心跳
PING
PONG
```

## 附录B：CLSID 和 GUID

| 标识符 | GUID |
|---|---|
| `CLSID_FaceLoginProvider` | `{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}` |
| IdentityStore MSA Provider | `{D7F9888F-E3FC-49b0-9EA6-A85B5F392A4F}` |

## 附录C：注册表键值

| 路径 | 值名 | 用途 |
|---|---|---|
| `HKLM\SOFTWARE\FaceLogin` | `InstallPath` | 安装目录 |
| `HKLM\SOFTWARE\FaceLogin` | `DataPath` | 数据目录 |
| `HKLM\SOFTWARE\...\Credential Providers\{CLSID}` | `Disabled` | 禁用凭据提供方 (REG_DWORD) |
| `HKLM\SOFTWARE\Microsoft\IdentityStore\LogonCache\{MSA_GUID}\Name2Sid\{hash}` | `IdentityName` | MSA UPN |
