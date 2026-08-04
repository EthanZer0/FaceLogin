# 从零到能跑：自定义 Windows Credential Provider 凭据

> 面向 Windows 开发者，从框架概念讲起，手把手带你写出一个能在锁屏上出现、能完成登录的 Credential Provider（CP）。核心章节「凭据打包」会讲透 LSA 认证包与 `CredPackAuthenticationBufferW`。

---

## 目录

1. [Credential Provider 是什么](#1-credential-provider-是什么)
2. [环境准备与概念铺垫](#2-环境准备与概念铺垫)
3. [Provider 生命周期：磁贴如何出现](#3-provider-生命周期磁贴如何出现)
4. [磁贴字段：定义 UI](#4-磁贴字段定义-ui)
5. [Credential 生命周期：认证如何发生](#5-credential-生命周期认证如何发生)
6. [核心：凭据打包全流程](#6-核心凭据打包全流程)
7. [常见坑与设计取舍](#7-常见坑与设计取舍)
8. [安全边界](#8-安全边界)
9. [注册、部署与调试](#9-注册部署与调试)
10. [结语](#10-结语)

---

## 1. Credential Provider 是什么

Windows 的**锁屏界面（LogonUI）**和**登录界面**上，你看到的"密码框""PIN 框""指纹图标"——这些不是 LogonUI 自己画的，而是由一个个 **Credential Provider**（凭据提供程序，下文简称 CP）插件贡献的。

CP 是一个 **COM 进程内 DLL**，由 LogonUI 在登录流程中加载：

```
      Win + L 锁屏
            │
            ▼
   ┌─────────────────┐
   │    LogonUI.exe  │
   │     安全桌面     │
   │ Desktop, SYSTEM │
   └────────┬────────┘
            │ 加载 CP DLL，调用 ICredentialProvider
            ▼
   ┌─────────────────────┐
   │    你的 CP DLL      │
   │ 提供磁贴 + 认证逻辑 │
   └────────┬────────────┘
            │ GetSerialization() 返回打包后的凭据
            ▼
   ┌──────────────────┐
   │  Winlogon / LSA  │
   │ 用认证包验证身份 │
   │     MSV1_0       │
   └──────────────────┘
```

关键点：

- **LogonUI 运行在安全桌面（Secure Desktop）**，与普通桌面隔离，`SYSTEM` 权限。
- CP 本身**不验证密码**。它只负责"**把用户想提交的凭据打包成 LSA 认识的结构**"，真正验证身份的是 **LSA（Local Security Authority）** 加载的**认证包（Authentication Package）**。
- 所以一个"自定义 CP"可以接入任意身份来源——指纹、人脸、智能卡、OTP、第三方账号……只要最后能把一个合法的 `用户名 + 密码`（或证书）打包交给 LSA。

> 微软官方把这个框架称作 "Windows Credential Provider Framework"。内置的密码 CP、PIN（Windows Hello）CP、智能卡 CP 都是这个框架的实例。

---

## 2. 环境准备与概念铺垫

### 2.1 环境

- Windows 10/11 专业版及以上（开发调试需要锁屏）。
- Visual Studio（推荐 2019/2022），安装"使用 C++ 的桌面开发"。
- 需要引用头文件 `<credentialprovider.h>`（Windows SDK 自带），链接 `credui.lib`。
- 建议在**虚拟机**里开发测试——调试过程中锁屏可能出现异常，真机容易把自己锁在门外（见 [第 9 节](#9-注册部署与调试)）。

### 2.2 需要的 COM 基础

CP 框架本质是 COM。你只需要理解三层：

| 概念 | 含义 |
|---|---|
| `IUnknown` | 所有 COM 接口的基类，含 `QueryInterface` / `AddRef` / `Release` |
| `ICredentialProvider` | **提供者**：LogonUI 用它枚举磁贴 |
| `ICredentialProviderCredential` | **凭据**：单个磁贴的认证逻辑 |

一个 DLL 里通常实现**一个 Provider + 一个 Credential**。Provider 决定"这个磁贴是否出现、有几个"，Credential 决定"这个磁贴显示什么、认证成功后交什么给系统"。

> **完整对象模型其实是三层**。除了 Provider 和 Credential，还常有一个 **Factory（类厂）** 和一个 **Filter（过滤器）**。DLL 入口 `DllGetClassObject` 先创建 Factory，再由 Factory 通过 `CreateInstance` 分别"造出"Provider 和 Filter 两个独立对象：

```
LogonUI 加载 DLL → DllGetClassObject(rclsid=你的CLSID)
    │ 创建 MyFactory（IClassFactory）
    ▼
MyFactory::QueryInterface
    │
    ├── MyFactory::CreateInstance(riid=ICredentialProvider)
    │         ▼
    │      MyProvider（提供磁贴）
    │
    └── MyFactory::CreateInstance(riid=ICredentialProviderFilter)
              ▼
           MyFilter（过滤其它 CP，可选）
```

其中 **Filter（`ICredentialProviderFilter`）是很多人忽略的一层**——它可以在登录界面上**隐藏系统里的其他 provider**（比如内置密码磁贴、PIN 磁贴），甚至可以决定哪些 provider 允许出现。Filter 有独立的注册键（见 [第 9 节](#9-注册部署与调试)），系统始终自带一个 `GenericFilter`。我们下面的示例以 Provider + Credential 为主线，Filter 单独在第 3.1 节讲。

### 2.3 三个要打交道的 LSA 概念

在进入正题前，先记住三个名词（第 6 节会用）：

| 名词 | 说明 |
|---|---|
| 认证包（Auth Package） | LSA 里负责把凭据转成登录会话的模块。常见：`MSV1_0`（本地/域密码）、`Kerberos`、`Negotiate` |
| 认证包 ID | 一个 `ULONG` 数字。打包凭据时必须告诉 LSA"这份凭据给哪个认证包用"，否则 LSA 不知道怎么解析 |
| `CredPackAuthenticationBuffer` | Win32 API，把 `用户名+密码` 打包成认证包能解析的字节结构 |

---

## 3. Provider 生命周期：磁贴如何出现

当你锁屏，LogonUI 会枚举注册表里所有已注册的 CP，依次实例化，然后调用 `ICredentialProvider` 的方法。调用顺序大致是：

```
DllGetClassObject(CLSID 你的 CP)
    │  （工厂创建 Provider 和 Filter）
    ▼
MyFilter::Filter(cpus, 全部provider列表)   ← 可选：决定放行/隐藏哪些 CP
    ▼
MyProvider::SetUsageScenario(cpus, flags)   ← 告诉它解锁/登录/还是别的场景
    ▼
（仅 RDP/CredUI）MyProvider::SetSerialization(pcpcs)  ← 处理外部传入的序列化凭据
    ▼
MyProvider::SetUserArray(用户列表)           ← 可选：拿到当前用户账号
    ▼
MyProvider::GetCredentialCount(&count,&default,&autoLogon)  ← 有几个磁贴
    ▼
MyProvider::GetCredentialAt(0, &pCredential) ← 取第 0 个磁贴（ICredentialProviderCredential）
    ▼
MyProvider::GetFieldDescriptorCount / GetFieldDescriptorAt   ← 描述磁贴长什么样
    ▼
MyCredential::GetStringValue / GetFieldState ← 填充每个字段的内容与可见性
    ▼
MyCredential::GetUserSid(实现 Credential2 时) ← 告诉系统这个磁贴属于哪个 SID
    ▼
MyCredential::Advise(ICredentialProviderCredentialEvents*)
    │  ← 磁贴可开始干活（连服务、开摄像头…）
    ▼
（用户交互…）
    ▼
MyCredential::GetSerialization → 打包凭据 → 认证
    ▼
MyCredential::ReportResult(status)          ← 登录结果
    ▼
MyCredential::UnAdvise → MyProvider::UnAdvise  ← 卸载（先凭据后提供者）
```

注意两点：
- **Filter 先于 Provider 执行**（`MyFilter::Filter` 在最前面）。它拿到所有已注册 CP 的 CLSID 列表，可以放行或隐藏其中任意一个——包括系统内置的密码/PIN 磁贴。
- **不支持某个场景时**（如 Filter 对 `CPUS_CHANGE_PASSWORD` 返回 `E_NOTIMPL`），你只会收到 `SetUsageScenario` 和 `UnAdvise` 两个调用，不会收到其他回调。

### 3.1 Filter：隐藏系统里的其他磁贴

`ICredentialProviderFilter` 是独立于 Provider 的第二个对象，由工厂在 `CreateInstance(riid=ICredentialProviderFilter)` 时创建。它的职责是**过滤登录界面上出现的其他 provider**：

- `Filter(cpus, dwFlags, rgclsidProviders, rgbAllow, cProviders)` —— 宿主把当前场景和**所有已注册 provider 的 CLSID 列表**传进来，你在 `rgbAllow` 数组里标记每个是否允许显示。返回 `E_NOTIMPL` 表示"不干预"。
- `UpdateRemoteCredential` —— 远程桌面（RDP）登录时，把远程序列化凭据重定向到正确的 provider。

常见用法：只保留你自己的磁贴 + 密码磁贴，隐藏 PIN、指纹等 Windows Hello 磁贴：

```cpp
STDMETHODIMP MyFilter::Filter(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags,
    GUID* rgclsidProviders, BOOL* rgbAllow, DWORD cProviders) {
    for (DWORD i = 0; i < cProviders; i++) {
        if (rgclsidProviders[i] == CLSID_PasswordProvider)
            rgbAllow[i] = TRUE;   // 保留密码磁贴
        else if (rgclsidProviders[i] == CLSID_MyProvider)
            rgbAllow[i] = TRUE;   // 保留我的磁贴
        else
            rgbAllow[i] = FALSE;  // 其余（PIN/指纹/…）隐藏
    }
    return S_OK;
}
```

Filter 有**独立的注册键**（`...\Credential Provider Filters\{你的CLSID}`，见 [第 9 节](#9-注册部署与调试)），系统始终自带一个 `GenericFilter`（GUID `{DDC0EED2-ADBE-40b6-A217-EDE16A79A0DE}`）。**多个自定义 Filter 可能互相冲突**——安装器应检查该注册表键避免叠加过滤把密码磁贴也藏了。

> 谨慎使用：隐藏过猛会把用户锁在外面。理想情况下至少保留一个"密码/其他登录方式"作为兜底。

### 3.2 场景开关 `SetUsageScenario`

LogonUI 会在不同的界面调用你的 Provider，用 `CREDENTIAL_PROVIDER_USAGE_SCENARIO` 区分：

| 场景 | 含义 | 我们通常 |
|---|---|---|
| `CPUS_LOGON` | 冷启动/注销后的登录 | 支持 |
| `CPUS_UNLOCK` | 锁屏解锁 | 支持 |
| `CPUS_CREDUI` | Windows 安全对话框（如 UAC 提权、Edge 看密码） | 可选择性忽略 |
| `CPUS_CHANGE_PASSWORD` | 修改密码 | 通常忽略 |
| `CPUS_PLAP` | 预登录访问（网络认证） | 通常忽略 |

> 实战建议：只响应 `LOGON`/`UNLOCK`。在 `SetUsageScenario` 里对其他场景直接返回 `E_NOTIMPL`，LogonUI 就会跳过你的 Provider，让内置密码 CP 接管。我们自己的实现就是这么做的——否则像"改密码""UAC 提权"这类多步对话框会被我们的自动登录逻辑干扰。

### 3.3 是否显示磁贴

`SetUsageScenario` 还可以做**隐藏判断**：比如"没有注册过任何用户的数据库"→ 直接 `return E_NOTIMPL`，磁贴就不出现。这是优化用户体验的关键位置。

> 有意思的细节：为了让 Provider 有机会在锁屏前就判断"要不要显示"，这里可以读注册表、读文件、检查服务状态——但**不要做耗时操作**，LogonUI 是同步调用你的。

### 3.4 枚举磁贴

`GetCredentialCount` 返回：

- `pdwCount`：磁贴数量（通常 1）。
- `pdwDefault`：默认选中的索引（通常 0）。
- `pbAutoLogonWithDefault`：是否**自动登录**（见第 5 节，这是整个框架最容易被误解的参数）。

### 3.5 小骨架

一个 Provider 的最小骨架（示意，省略 IUnknown 与错误处理）：

```cpp
class MyCredentialProvider : public ICredentialProvider {
public:
    // IUnknown ...
    STDMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus,
                                  DWORD dwFlags) override {
        // 只支持登录/解锁
        if (cpus != CPUS_LOGON && cpus != CPUS_UNLOCK)
            return E_NOTIMPL;
        m_credential = new MyCredential(this);
        return S_OK;
    }
    STDMETHODIMP GetCredentialCount(DWORD* count, DWORD* def,
                                    BOOL* autoLogon) override {
        *count = 1; *def = 0; *autoLogon = FALSE;
        return S_OK;
    }
    STDMETHODIMP GetCredentialAt(DWORD index,
                                 ICredentialProviderCredential** ppcpc) override {
        if (index != 0) return E_INVALIDARG;
        return m_credential->QueryInterface(IID_ICredentialProviderCredential,
                                            (void**)ppcpc);
    }
    // 其余纯虚方法（SetSerialization/Advise/UnAdvise/字段描述）...
};
```

---

## 4. 磁贴字段：定义 UI

一个磁贴不是一个控件，而是**一组字段（Field）**。每个字段有：

- `dwFieldID`：字段编号。
- `cpft`：字段类型。
- `pszLabel`：字段文字（可选）。

常用字段类型：

| 类型 | 用途 |
|---|---|
| `CPFT_LARGE_TEXT` | 大标题（如"人脸登录"） |
| `CPFT_SMALL_TEXT` | 状态文字（如"识别中…"） |
| `CPFT_EDIT_TEXT` | 文本框（用户名） |
| `CPFT_PASSWORD_TEXT` | 密码框 |
| `CPFT_SUBMIT_BUTTON` | 提交按钮 |
| `CPFT_COMMAND_LINK` | 命令链接（如"切换到密码登录"） |
| `CPFT_TILE_IMAGE` | 磁贴图标 |
| `CPFT_CHECKBOX` | 复选框 |
| `CPFT_COMBOBOX` | 下拉框 |

Provider 通过 `GetFieldDescriptorAt` 把字段描述交出去，Credential 通过 `GetFieldState` / `GetStringValue` 等控制**每个字段在选中/未选中时的可见性与内容**。

**我们的例子只需要 4 个字段**：

```cpp
// 字段 0：大标题
m_fields[0].dwFieldID = 0;
m_fields[0].cpft      = CPFT_LARGE_TEXT;
m_fields[0].pszLabel  = L"示例登录";

// 字段 1：状态文字
m_fields[1].dwFieldID = 1;
m_fields[1].cpft      = CPFT_SMALL_TEXT;
m_fields[1].pszLabel  = L"状态";

// 字段 2：提交按钮（隐藏，我们走自动登录）
m_fields[2].dwFieldID = 2;
m_fields[2].cpft      = CPFT_SUBMIT_BUTTON;
m_fields[2].pszLabel  = L"提交";

// 字段 3：命令链接（切换到密码登录）
m_fields[3].dwFieldID = 3;
m_fields[3].cpft      = CPFT_COMMAND_LINK;
m_fields[3].pszLabel  = L"使用密码登录";
```

字段的**可见性状态**（`CREDENTIAL_PROVIDER_FIELD_STATE`）在 `GetFieldState` 里返回：

| 状态 | 含义 |
|---|---|
| `CPFS_DISPLAY_IN_BOTH` | 选中/未选中都显示 |
| `CPFS_DISPLAY_IN_SELECTED_TILE` | 只在磁贴被选中时显示 |
| `CPFS_DISPLAY_IN_DESELECTED_TILE` | 只在磁贴未选中时显示 |
| `CPFS_HIDDEN` | 始终隐藏 |

```cpp
STDMETHODIMP GetFieldState(DWORD id, CREDENTIAL_PROVIDER_FIELD_STATE* state,
                           CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* is) {
    switch (id) {
    case 0: case 1: *state = CPFS_DISPLAY_IN_BOTH; break;      // 标题+状态始终显示
    case 2:          *state = CPFS_HIDDEN;        break;        // 提交按钮隐藏
    case 3:          *state = CPFS_DISPLAY_IN_DESELECTED_TILE; break; // 命令链接未选中时显示
    default: return E_INVALIDARG;
    }
    *is = CPFIS_NONE;
    return S_OK;
}
```

> 关于图标有个已知坑：`CPFT_TILE_IMAGE` + `GetBitmapValue` 返回的位图**不支持 alpha 透明**（微软自己的磁贴也不用透明图标）。想要好看的磁贴，用纯文字字段更稳妥。

### 4.1 UI 渲染限制（接受它，别跟它较劲）

LogonUI 的磁贴 UI **高度受限**，作者在实践中确认过的限制：

- **`CPFT_LARGE_TEXT` 在 Win10+ 实际按 `CPFT_SMALL_TEXT` 渲染**——大小标题视觉上无区别。别指望用 `LARGE_TEXT` 做醒目的大字标题。
- **头像和用户名由 LogonUI 强制显示**，你无法修改或隐藏（它显示的是当前用户账户信息）。
- **字段只能垂直排列**，不能横向布局；空间不足时 LogonUI 会加滚动条。
- **不能定位、不能缩放、不能自定义样式**。你唯一能控制的是字段的**显示顺序**和显隐。

换句话说，磁贴 UI 是"宿主提问、你回答"的模型——你决定**放哪些字段、每个字段显示什么文字**，但**怎么摆、长什么样**由 LogonUI 决定。设计 UI 时把精力放在文案和字段组合上，而不是样式。

---

## 5. Credential 生命周期：认证如何发生

磁贴被选中后，LogonUI 开始和 `ICredentialProviderCredential` 交互：

```
Advise(ICredentialProviderCredentialEvents*)
      │   ← 磁贴被创建，可启动认证（连服务、开摄像头…）
      ▼
SetSelected(&pbAutoLogon)   ← 决定是否自动登录
      │
      ▼
GetSerialization(...)        ← 系统（反复）调用，等你要凭据
      │
      ▼
[认证成功] ReportResult(STATUS_SUCCESS, ...)
```

### 5.1 状态机

一个认证型 CP 本质上是一个**状态机**。我们的实现用几个状态表达认证进程：

```
   ┌─────────────────┐
   │     Waiting     │
   └────────┬────────┘
            │ 开始认证
            ▼
   ┌─────────────────┐     失败/超时       ┌──────────┐
   │  Authenticating │────────────────────▶│  Failed  │
   └────────┬────────┘                     └────┬─────┘
            │ 认证成功                           │ 放弃
            ▼                                   ▼
   ┌─────────────────┐
   │      Ready      │──▶ 打包凭据交给系统
   └─────────────────┘
```

- `Waiting`：还没开始认证（可能在等用户按键）。
- `Authenticating`：认证进行中（在等服务端/摄像头返回）。
- `Ready`：凭据已就绪，可以打包。
- `Failed` / `Error`：认证失败或服务不可用。

### 5.2 自动登录与 `GetSerialization` 轮询

这是框架里**最核心也最反直觉**的机制：

- `GetSerialization` 会被 LogonUI **反复调用**（只要磁贴选中且用户点了提交，或 `pbAutoLogon` 为 TRUE）。
- 每次返回时，你要告诉系统"**我交没交凭据**"：
  - `CPGSR_NO_CREDENTIAL_NOT_FINISHED`：还没好，别催我（但系统会再来问）。
  - `CPGSR_RETURN_CREDENTIAL_FINISHED`：凭据已打包好，拿去用。
  - `CPGSR_NO_CREDENTIAL_FINISHED`：我不交了（用户转用密码 / 放弃）。

**自动登录的两阶段模式**是解锁型 CP 的典型手法：

1. 磁贴默认选中 → `SetSelected` 返回 `*pbAutoLogon = TRUE` → LogonUI 会**立刻且反复**调 `GetSerialization`。
2. `GetSerialization` 在认证未完成时返回 `CPGSR_NO_CREDENTIAL_NOT_FINISHED`，让系统继续等。
3. 后台认证线程拿到结果后，把状态切到 `Ready`，并调用 `ICredentialProviderEvents::CredentialsChanged()` 通知 LogonUI"重新枚举"。
4. 重新枚举后 `GetSerialization` 被再次调用，此时状态已是 `Ready`，打包返回 `CPGSR_RETURN_CREDENTIAL_FINISHED`。

下图把上述 1-4 步画成时序图：

```
  LogonUI                     CP Credential                   后台认证线程
    │                                   │                               │
    │ ① SetSelected 自动登录            │                               │
    ├───────────────────────────────────▶                               │
    │                                   │                               │
    │ ② GetSerialization() 反复         │                               │
    ├───────────────────────────────────▶                               │
    │◀──────────────────────────────────┤                               │
    │    （认证未完成，继续等）         │                               │
    │                                   │                               │
    │ ③ 后台认证完成 → 状态 Ready       │                               │
    │                                   ├───────────────────────────────▶
    │◀──────────────────────────────────────────────────────────────────┤
    │ ④ CredentialsChanged() 重枚举     │                               │
    │                                   │                               │
    │ ⑤ GetSerialization() 重调         │                               │
    ├───────────────────────────────────▶                               │
    │◀──────────────────────────────────┤                               │
    │    （打包凭据，进系统）           │                               │
    │                                   │                               │
    ▼                                   ▼                               ▼
```

```cpp
STDMETHODIMP GetSerialization(
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
    PWSTR* pStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* pStatusIcon) {

    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *pStatusText = nullptr;
    *pStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    if (m_state == Waiting)          return S_OK;  // 还在等人，继续等
    if (m_state == Failed)  { *pcpgsr = CPGSR_NO_CREDENTIAL_FINISHED; return S_OK; }
    if (m_state != Ready)            return S_OK;  // 认证中，继续等

    // 凭据就绪 → 打包
    HRESULT hr = PackCredentials(pcpcs);
    if (SUCCEEDED(hr)) *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
    return hr;
}
```

> 反复调用是**故意的**，别当成 bug。LogonUI 用这种方式实现"凭据最终会到达"的轮询。你只要保证：没就绪时返回 NOT_FINISHED，就绪时返回 RETURN_CREDENTIAL_FINISHED。

### 5.3 超时兜底

`GetSerialization` 被反复调用意味着它**必须自己能终结**。如果你的服务挂了、摄像头没开，状态会一直停在 `Authenticating`，用户会被锁屏卡死。

实战必须有超时：记录开始认证的时间，超过阈值（如 20 秒）强制切到 `Failed` 返回 FINISHED。我们的实现里用的是 `GetSystemTimeAsFileTime` 转 100ns 计差。

### 5.4 关键坑：`ReportResult` 成功后无法取消

`ReportResult` 接收登录结果（`ntsStatus`）。这里有个**设计缺陷**级的大坑：

> **如果登录成功（`ntsStatus == S_OK`），`ReportResult` 返回后 UI 立即开始卸载——无论你返回什么状态码，都无法阻止。**

也就是说，`ReportResult` **不能作为"第二道门"**。想实现 MFA（多因素认证：第一因素 + 第二因素校验）的开发者，被迫在 `GetSerialization` **之前**自行调用 `LsaLogonUser` 完成第二因素验证，再决定是否真正交凭据。这是 CP 框架的一个已知限制，微软没有提供从 `ReportResult` 取消成功登录的机制。

实践中意味着：

- 你的**真实认证逻辑**必须在 `GetSerialization` 里完成（或更早）。
- `ReportResult` 只用于**记录结果 / 清理**（失败时重置状态让用户重试），别指望它拦下已成功的登录。

### 5.5 高级接口：`IConnectableCredentialProviderCredential`

长耗时认证（如慢速的 802.1x 预登录）有专门的接口支持：

- **`IConnectableCredentialProviderCredential`** —— `Connect()` 在 `GetSerialization` **之前**被调用，可以在等待期间向用户显示**状态文本**和**取消按钮**。Windows 自身的慢速 PLAP 认证就用它。
- 实现它时 `Connect` / `Disconnect` 与 `GetSerialization` 的调用顺序是：`Connect → GetSerialization → ReportResult`（认证完成后 `Disconnect`）。

如果我们的自动登录流程要做"识别中…"的可取消版本，这个接口就是正道。

### 5.6 `SetDeselected` 也应清除敏感数据

微软官方建议：磁贴被切换走（`SetDeselected`）时，**清除内存中的敏感数据**——密码、PIN、密钥，与 `GetSerialization` 打包后清零同等重要。官方示例 `CSampleCredential.cpp` 里专门有一处安全擦除方法。别等到 `UnAdvise` 才清。

---

## 6. 核心：凭据打包全流程

这是全文**最重要**的一节。框架的最后一公里：把"用户名+密码"变成 LSA 能解析的 `CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION`。

```
  用户名 + 密码（来自服务端/硬件）
          │
          ▼
  ① 查认证包 ID
     LsaConnectUntrusted → LsaLookupAuthenticationPackage(MSV1_0)
          │
          ▼
  ② CredPackAuthenticationBufferW 两段式
     （先问尺寸 → 再真正打包）
          │
          ▼
  ③ 填 CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION
          │
          ▼
  ④ GetSerialization 返回 CPGSR_RETURN_CREDENTIAL_FINISHED
```

### 6.1 第一步：查询认证包 ID

打包结果必须携带"这份凭据给谁用"。我们用 `LsaConnectUntrusted` 建立到 LSA 的临时连接，再查 `MSV1_0` 认证包的 ID：

```cpp
ULONG ulAuthPackage = 0;
HANDLE hLsa = nullptr;

NTSTATUS st = LsaConnectUntrusted(&hLsa);
if (st == 0 && hLsa) {
    LSA_STRING pkg;
    char name[] = "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0";
    pkg.Buffer = name;
    pkg.Length = (USHORT)strlen(name);
    pkg.MaximumLength = pkg.Length;
    st = LsaLookupAuthenticationPackage(hLsa, &pkg, &ulAuthPackage);
    LsaDeregisterLogonProcess(hLsa);
}
if (st != 0) return E_FAIL;  // 查不到 MSV1_0 就放弃
```

> 为什么用"Untrusted"连接？因为 CP 运行在 LogonUI 里、可能是受限上下文。`LsaConnectUntrusted` 不需要任何特权，只是查个包 ID，足够了。

### 6.2 第二步：`CredPackAuthenticationBufferW` 两段式

这个 API 是打包的核心。它把 `用户名 + 密码` 转换成**认证包能解析的内存结构**（对 MSV1_0 来说内部是 `MSV1_0_INTERACTIVE_LOGON`，带各字段偏移）。

它有个经典的**两段式调用**模式：

1. **先问尺寸**：传 `nullptr` 缓冲 + `&cbPackedCreds`，调用失败但返回 `ERROR_INSUFFICIENT_BUFFER`，此时 `cbPackedCreds` 被填上需要的字节数。
2. **再真正打包**：`CoTaskMemAlloc` 那块内存，传入真正指针，成功后 `cbPackedCreds` 是实际长度。

```cpp
DWORD cbPackedCreds = 0;

// 第一段：问尺寸（一定失败，但拿到大小）
CredPackAuthenticationBufferW(0, user, password, nullptr, &cbPackedCreds);

// 第二段：分配 + 真打包
BYTE* pPacked = (BYTE*)CoTaskMemAlloc(cbPackedCreds);
if (!pPacked) return E_OUTOFMEMORY;
if (!CredPackAuthenticationBufferW(0, user, password, pPacked, &cbPackedCreds)) {
    CoTaskMemFree(pPacked);
    return HRESULT_FROM_WIN32(GetLastError());
}
```

> `rgbSerialization` 最终要交给 LogonUI，LogonUI 负责释放。**必须用 `CoTaskMemAlloc`**，别用 `new[]` / `LocalAlloc`——释放端按 COM 惯例是 `CoTaskMemFree`。

### 6.3 第三步：填序列化结构

```cpp
pcpcs->rgbSerialization          = pPacked;            // 打包后的字节
pcpcs->cbSerialization           = cbPackedCreds;      // 长度
pcpcs->ulAuthenticationPackage   = ulAuthPackage;      // 第①步查到的 MSV1_0 ID
pcpcs->clsidCredentialProvider   = CLSID_MyProvider;   // 你自己的 CLSID
```

字段含义：

| 字段 | 说明 |
|---|---|
| `rgbSerialization` | 打包字节，`CoTaskMemAlloc` 分配 |
| `cbSerialization` | 字节数 |
| `ulAuthenticationPackage` | 认证包 ID（MSV1_0） |
| `clsidCredentialProvider` | 你的 Provider CLSID，用于登录失败时的归属归因 |

### 6.4 用户名格式：本地账户 vs 微软账户

打包前，`user` 字符串的格式**必须正确**，否则 LSA 解析会失败：

| 账户类型 | 用户名格式 | 示例 |
|---|---|---|
| 本地/域账户 | `域\用户名` | `COMPUTER01\alice` |
| 微软账户 (MSA) | UPN 邮箱形式 | `alice@outlook.com` |

```cpp
std::wstring packedUser;
if (!upn.empty() && upn.find(L'@') != std::wstring::npos)
    packedUser = upn;                       // 微软账户：UPN
else
    packedUser = domain + L"\\" + username; // 本地账户：DOMAIN\user
```

> 为什么要有区别？MSV1_0 认证包解析 `DOMAIN\user` 时按"本地 SAM 账户"处理；而 MSA 在本地并没有 SAM 密码，LSA 收到 UPN 后会走云/缓存的账户体系。**格式错了 = 登录被拒**，这是自定义 CP 最典型的坑之一。

### 6.5 完整打包函数

把上面串起来：

```cpp
HRESULT PackCredentials(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) {
    // ① 查 MSV1_0 认证包 ID
    ULONG ulAuthPackage = 0;
    // ... LsaConnectUntrusted + LsaLookupAuthenticationPackage ...

    // ② 构造用户名字符串（本地 DOMAIN\user / MSA UPN）
    std::wstring packedUser = BuildPackedUsername();

    // ③ 两段式打包
    DWORD cb = 0;
    CredPackAuthenticationBufferW(0, packedUser.c_str(), m_password.c_str(),
                                  nullptr, &cb);
    BYTE* pPacked = (BYTE*)CoTaskMemAlloc(cb);
    if (!pPacked) return E_OUTOFMEMORY;
    if (!CredPackAuthenticationBufferW(0, packedUser.c_str(), m_password.c_str(),
                                       pPacked, &cb)) {
        CoTaskMemFree(pPacked);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // ④ 填序列化结构
    pcpcs->rgbSerialization        = pPacked;
    pcpcs->cbSerialization         = cb;
    pcpcs->ulAuthenticationPackage = ulAuthPackage;
    pcpcs->clsidCredentialProvider = CLSID_MyProvider;
    return S_OK;
}
```

### 6.6 密码安全处理

打包完成后，密码还在进程内存里。**立刻清零**：

```cpp
// 打包完后马上抹掉密码缓冲
SecureZeroMemory((void*)m_password.c_str(), m_password.size() * sizeof(wchar_t));
```

更稳妥的做法是**全程用专门的 SecureBuffer 类**持有密码（重载了 `operator[]` 等、析构时清零），而不是 `std::wstring`。至少做到：

- 密码不进日志（哪怕脱敏也尽量别打）。
- 打包后 / 对象销毁时清零。
- 磁贴被切走（`SetDeselected`）时清零（见 [5.6](#56-setdeselected-也应清除敏感数据)）。
- 不要把密码复制多份副本（能传引用就传引用）。

---

## 7. 常见坑与设计取舍

**① 磁贴不出现** —— 大概率是注册表没写对、CLSID 不匹配，或 `SetUsageScenario` 返回了 `E_NOTIMPL`。先确认 DLL 已注册（见第 9 节），再确认场景判断没把 `LOGON`/`UNLOCK` 也拒了。

**② 能显示但点击无反应** —— 检查 `GetCredentialCount` 的 `pbAutoLogonWithDefault` 与 `SetSelected` 的 `pbAutoLogon`。自动登录的时机错了，LogonUI 就不会主动调 `GetSerialization`。

**③ 锁屏卡死 / 一直转圈** —— `GetSerialization` 没有终结路径。状态停在 `Authenticating` 又不返回 FINISHED，就是死循环。务必加超时。

**④ 登录被拒** —— 用户名格式错（`DOMAIN\user` vs UPN）、认证包 ID 查错、或 `cbSerialization` 没填对。

**⑤ 认证完成后不自动进系统** —— 确认 `GetSerialization` 返回了 `CPGSR_RETURN_CREDENTIAL_FINISHED` 且 `pcpgsr` 结构完整。常见错误是返回了 NOT_FINISHED。

**⑥ 重复触发认证 / 无限循环** —— `Advise` 在重新枚举时会被反复调用。要有"已在认证 / 已就绪就跳过"的守卫，否则会出现"认证成功 → 触发重枚举 → Advise 又启动一次认证"的循环。我们的实现用状态 + 连接标志双重守卫。

**⑦ 自动登录过早触发** —— 在 `SetSelected` 里把 `*pbAutoLogon = TRUE` 设得太早，可能连用户操作的机会都不给。区分"冷启动登录"（应自动）和"锁屏解锁"（等用户按键）两个场景，分别决定是否自动登录。

**⑧ 释放问题** —— `rgbSerialization` 必须 `CoTaskMemAlloc`；字段描述符的字符串用 `SHStrDupW`；`GetFieldDescriptorAt` 分配的内存由 LogonUI 释放。COM 内存惯例别混用。

---

## 8. 安全边界

自定义 CP 相当于在锁屏上开了一扇门，安全设计直接影响系统信任：

- **密码是最敏感资产**：持有期间尽量用 `SecureBuffer`，打包后立即清零，绝不写日志。整个认证链路里，密码只在"服务 → 管道 → CP"这一段内存中短暂存在。
- **跨进程传输用命名管道时要限权限**：管道的 DACL 应只允许 `SYSTEM` 和管理员，开启 `PIPE_REJECT_REMOTE_CLIENTS` 拒绝远程连接。
- **序列化输入一律视为不可信**：无论是 RDP 的 `UpdateRemoteCredential`、CredUI 的 `SetSerialization`，还是其他来源传入的 `CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION`，都可能被构造/篡改。解码前做边界校验，别在信任这些数据的前提下做任何敏感操作。
- **服务端身份来源要可信**：你的人脸/指纹服务是 SYSTEM 还是用户权限？如果服务可被普通用户替换，等于把锁屏钥匙交给了用户。
- **认证包本身不做校验**：CP 只打包，不验证。别在自己代码里"信任"任何客户端声称的"验证通过"，把校验逻辑放在安全服务端。
- **降级路径**：服务不可用时，要能优雅地让用户转用密码（命令链接切回密码 CP），而不是把用户锁在门外。

---

## 9. 注册、部署与调试

### 9.1 注册

CP 的 COM 注册分两部分：

**COM 组件注册**（`regsvr32` 会帮你写，或手动写）：

```
HKLM\SOFTWARE\Classes\CLSID\{你的CLSID}\InprocServer32
    (Default) = 你的 DLL 路径
    ThreadingModel = Apartment
```

**Credential Provider 注册**（LogonUI 从这里枚举）：

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\
    Credential Providers\{你的CLSID}
    (Default) = "示例登录"
```

> LogonUI 枚举的是第二个键。只注册 COM CLSID 不注册 Provider 键，磁贴不会出现。

**如果实现了 Filter**（见 [3.1](#31-filter隐藏系统里的其他磁贴)），还需要**独立的注册键**，否则过滤器不生效：

```
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\
    Credential Provider Filters\{你的CLSID}
    (Default) = "示例登录过滤器"
```

两个键下都需要 `InprocServer32`（指向 DLL 绝对路径，`ThreadingModel = Apartment`）。系统自带 `GenericFilter`（GUID `{DDC0EED2-ADBE-40b6-A217-EDE16A79A0DE}`），多个自定义 Filter 并存时要检查该键避免冲突。

### 9.2 为什么建议在虚拟机调试

CP 运行在**安全桌面**，普通调试器（VS 的本地调试）经常没法附加。而一旦你的 CP 在真机锁屏上出问题，最坏情况是**每次解锁都蓝屏 / 卡死**，只能靠安全模式救。

虚拟机的好处：

- 快照随意回滚。
- 可以改"登录时不需要按 Ctrl+Alt+Del"等组策略，方便测试自动登录。
- 崩溃不伤真机。

### 9.3 调试技巧

- **日志**：CP DLL 在 LogonUI 进程里，`OutputDebugString` 不好看。最实用的办法是**写日志文件**（`C:\ProgramData\...`），在 Provider / Credential 每个关键方法进出都记一条，带状态与 HRESULT。这几乎是查 CP 问题的唯一手段。
- **用 `CredentialsChanged()` 看重枚举**：如果你怀疑"循环触发"，日志里数一下 `Advise` / `GetSerialization` 被调了几次。
- **测试锁屏**：虚拟机里 `Win+L`，看磁贴是否出现、点选后状态文字是否变化。
- **没有官方测试框架**：微软没有提供 LogonUI 的自动化测试 harness，实际就是"注册 → 锁屏 → 观察 → 改日志 → 再锁屏"这条最笨但最有效的循环。

### 9.4 一键脚本

调试期可以用一个批处理做"编译 → 注册 → 锁屏"循环：

```bat
@echo off
:: 先以管理员运行
regsvr32 /s /u "%~dp0Release\MyCredProvider.dll"
copy /y "%~dp0Release\MyCredProvider.dll" "%SystemRoot%\System32\" >nul
regsvr32 /s "%SystemRoot%\System32\MyCredProvider.dll"
echo Registered. Lock screen to test.
```

---

## 10. 结语

Credential Provider 框架的骨架并不复杂——一个 COM 对象，几个生命周期方法，最后把凭据打包交给 LSA。真正的复杂度在细节：状态机的终结性、自动登录的时机、用户名的格式、密码的生命周期管理、以及"服务挂了用户还能不能登录"的降级设计。

从本文的流程出发，你已经能实现：

1. 一个在锁屏出现的自定义磁贴；
2. 一个接入自己认证源的认证流程；
3. 一把把凭据"打包并交给 LSA"的钥匙——`CredPackAuthenticationBufferW`。

往后的扩展方向：接入智能卡 / TPM 签名的凭据（`KERB_CERTIFICATE_LOGON`）、支持 `CPUS_CREDUI` 的多步对话框、或者像本项目一样，把 CP 当作"薄壳"，把真正的身份识别放到独立的 SYSTEM 服务里，通过命名管道通信——锁屏壳子与识别服务解耦，安全边界也更清晰。

## 延伸阅读

Windows 上最完整的 CP 深度教程是 **Dennis Babkin** 的系列文章（建议按顺序读）：

- [Primer on Writing a Credential Provider in Windows](https://dennisbabkin.com/blog/?t=primer-on-writing-credential-provider-in-windows) —— 概念、对象模型、Filter、常见坑，本文很多内容（Factory 三层对象、Filter、`ReportResult` 不可取消、UI 限制）都源自它。
- [Sequence of Calls to a Credential Provider in Windows](https://dennisbabkin.com/blog/?t=sequence-of-calls-to-credential-provider-in-windows) —— 每个方法的确切调用时机与顺序，用日志逐条验证过，排查"为什么没被调用"必备。
- 微软官方示例 `V2CredentialProvider`（GitHub 可搜），看真实可编译的 Provider + Filter + 多磁贴结构。

---

*本文基于 Windows Credential Provider Framework 实战经验整理，部分要点参考了 Dennis Babkin 的系列文章。文中示例代码为通用写法，可直接套用到任意自定义认证源。*
