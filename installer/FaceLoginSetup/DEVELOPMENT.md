# FaceLoginSetup 开发文档

FaceLogin 图形化安装/卸载程序（Wails v2 + Vue 3 + Go）。

本文档面向后续维护者，重点说明如何**发布新版本**时正确使用两个可扩展机制：**自定义操作执行** 与 **新版本公告弹窗**。

---

## 目录

1. [项目结构](#项目结构)
2. [构建与运行](#构建与运行)
3. [发布新版本的检查清单](#发布新版本的检查清单)
4. [机制一：自定义操作执行（Config Upgrade）](#机制一自定义操作执行)
5. [机制二：新版本公告弹窗（Upgrade Notice）](#机制二新版本公告弹窗)
6. [安装流程（后端 Install 各步骤）](#安装流程)
7. [注册表键](#注册表键)

---

## 项目结构

```
FaceLoginSetup/
├── main.go                  # 入口：提权、per-release 自定义动作区、Wails 启动
├── app.go                   # App 结构体：绑定到前端的方法（Install/Uninstall/...）
├── wails.json               # Wails 项目配置（输出名、平台）
├── go.mod / go.sum
├── internal/                # 与前端无关的后端逻辑
│   ├── config.go            # 机制一：配置升级（强制同步默认参数）
│   ├── notice.go            # 机制二：新版本公告弹窗
│   ├── com.go               # COM DLL 注册/注销
│   ├── scm.go               # Windows 服务安装/停止/删除
│   ├── extract.go           # 内嵌资源解压（resources/ → 安装目录）
│   ├── elevate.go           # 管理员提权
│   └── util.go              # 注册表读写、文件/目录、路径等工具
├── resources/               # 内嵌资源：exe/dll/模型/dll 依赖（编译时打包）
└── frontend/                # Vue 3 + Tailwind 前端
    └── src/
        ├── App.vue          # 全部 UI 逻辑（含公告弹窗）
        ├── main.ts / style.css
        └── wailsjs/         # 自动生成的 Go ↔ JS 绑定（勿手改）
```

---

## 构建与运行

前置：Go 1.25+、Node/npm、Wails CLI v2。

```bash
# 安装 Wails CLI（若未装）
go install github.com/wailsapp/wails/v2/cmd/wails@latest

# 开发模式（前端热重载，后端方法可在浏览器联调）
cd FaceLoginSetup
wails dev

# 生产构建（输出到 build/bin/FaceLoginSetup.exe）
wails build -clean -platform windows/amd64
```

> `wails build` 会自动：生成 Go→JS 绑定（frontend/wailsjs/）、安装前端依赖、编译前端（`vue-tsc --noEmit && vite build`）、编译 Go、将 `resources/` 内嵌打包。**改完 Go 方法或前端后，必须重新 `wails build` 才能生效。**

---

## 发布新版本的检查清单

每次发布新版本安装包时，按此顺序操作：

1. **编译最新 C++ 产物**，并同步到 `resources/`（见下）。
2. **决定本版本是否需要机制一 / 机制二**，在 `main.go` 的自定义动作区开启对应开关。
3. `wails build -clean -platform windows/amd64` 重新打包。
4. （可选）验证：把安装包放到一个已安装旧版的环境跑升级，确认弹窗/参数同步行为符合预期。

### C++ 产物同步

`resources/` 里需要手动同步的二进制（构建不会自动更新）：

| 资源文件 | 来源（MSBuild Release 产物） |
|---|---|
| `FaceLoginService.exe` | `build/face_service/Release/FaceLoginService.exe` |
| `FaceLoginCredentialProvider.dll` | `build/credential_provider/Release/FaceLoginCredentialProvider.dll` |
| `FaceLoginConsole.exe` | `installer/FaceLoginSetup/resources/`（由 MSBuild 自动输出） |
| `onnxruntime.dll` 及 libgcc/lapack 等 | 运行时依赖，通常不变 |

> **`FaceLoginConsole.exe` 的构建目标直接输出到 `resources/`**，其余两个需手动复制。改 C++ 代码后务必核对时间戳，避免把旧二进制打进安装包。

---

## 机制一：自定义操作执行

**用途**：某版本改变了软件的默认参数，需要让已安装的老用户（有旧 `config.json`）也同步到新默认值。只对**本版本**生效，防止未来版本反复覆盖用户调整过的值。

### 相关文件

- 声明/逻辑：`internal/config.go`
- 开关与参数：`main.go` 自定义动作区

### 工作原理

1. `ConfigUpgradeEnabled` 默认 `false`。发布某版本时在 `main.go` 手动置 `true`。
2. 安装执行到 **Step 4.5** 时调用 `EnsureConfigDefaults(configPath)`（见 `app.go`）。
3. 该函数：
   - 读取已有 `config.json`（不存在则写一份完整默认值，保证首装可用）。
   - 仅当开关开启时，把 `ConfigUpgradeForcedDefaults` 里列出的键**强制覆盖**为新默认值。
   - **未列出的键原样保留**（用户的其它自定义设置不受影响）。
4. 下个版本把开关关闭后，老用户调整回去的值不会被再次覆盖。

### 如何为某版本启用

```go
// main.go — 自定义动作区
internal.ConfigUpgradeEnabled = true
internal.ConfigUpgradeForcedDefaults = map[string]any{
    "match_threshold":      0.30,   // 本版本调整过的键
    "anti_spoof_threshold": 0.30,
}
```

### 注意事项

- **键必须与 `config.json` 实际键名完全一致**（`config_util.cpp` 中的 `AppConfig` 字段）。
- 值类型需与 C++ 端解析兼容：整数/浮点用数字，字符串用字符串。
- 只覆盖"本版本确实调整过默认值"的键；没调整的键**不要**列进来，否则会把用户调过的值悄悄改回。

---

## 机制二：新版本公告弹窗

**用途**：用户使用新版本安装包做**升级安装**（已装过旧版）成功后，弹出一个"更新说明"弹窗。**首次全新安装不弹**。

### 相关文件

- 声明/逻辑：`internal/notice.go`
- 开关与内容：`main.go` 自定义动作区
- 前端渲染：`frontend/src/App.vue`（`showNotice` / `noticeLines`）
- 前端触发：`app.go` 的 `GetUpgradeNotice()`（绑定到前端）

### 工作原理（三层判断）

```
① 开关        main.go: NoticeEnabled = true，且 NoticeVersion/Title 非空
② 升级场景    后端 GetUpgradeNotice() 检查 InstallPath 注册表 + 目录存在 → 判定"已安装"
③ 安装成功    前端只在 Install() 返回 success 后查询公告
```

| 场景 | 结果 |
|---|---|
| 升级安装 + 成功 + 本版本开启公告 | ✅ 弹窗 |
| 首次安装（无旧版本） | ❌ 不弹（②拦截） |
| 安装失败 | ❌ 不弹（③拦截） |
| 本版本未开启公告 | ❌ 不弹（①拦截） |

### 如何为某版本启用

```go
// main.go — 自定义动作区
internal.NoticeEnabled  = true
internal.NoticeVersion  = "1.1.0"            // 弹窗右上角版本徽标
internal.NoticeTitle    = "FaceLogin 1.1.0 更新说明"  // 标题
// 正文：每行一个 \n 分隔，前端渲染为一条条圆点列表
internal.NoticeBody     = "升级后请重新录入人脸，以适配新识别引擎\n" +
                           "修复了旧版数据不兼容导致解锁失败的问题\n" +
                           "优化了冷启动识别速度"
```

### 前端数据流

```js
// App.vue — doInstall() 内，安装成功后
if (result.success && alreadyInstalled.value) {
  const n = await GetUpgradeNotice()
  if (n && n.title) {
    notice.value = n
    showNotice.value = true
  }
}
```

`noticeLines` 为 computed 属性，将 `notice.body` 按 `\n` 拆分为数组并过滤空行，模板中逐条渲染。

### 注意事项

- **正文每行一个要点**，用 `\n` 分隔；空行会被前端过滤。
- 弹窗内容存在**安装包二进制内**（Go 字符串），改文案后必须重新 `wails build`。
- 下个版本不需要弹窗时，把 `NoticeEnabled` 置 `false` 即可，旧公告不会残留。
- 判断"是否升级"目前基于 `InstallPath` 注册表 + 目录存在性。如需精确到"从某版本起才提示"，可扩展为对比已安装版本号（当前未实现）。

---

## 安装流程

后端 `App.Install(installDir)`（`app.go`）按序执行，前端通过 `setup:progress` 事件显示进度：

| 步骤 | 动作 | 备注 |
|---|---|---|
| 1 | 停止并删除已有服务 | `internal.StopAndDeleteService()` |
| 2 | 创建安装目录 | `os.MkdirAll` |
| 3 | 写注册表 InstallPath / DataPath | 见[注册表键](#注册表键) |
| 4 | 解压内嵌资源到安装目录 | `internal.ExtractAll` |
| **4.5** | **确保 config.json 默认值** | **机制一挂载点：`EnsureConfigDefaults`** |
| 5 | 设置数据目录 ACL | `internal.SetDirectoryACL` |
| 6 | 注册 COM DLL | `internal.RegisterCOMDLL` |
| 7 | 安装并启动服务 | `internal.InstallService` |
| 8 | 额外解压 FaceLoginConsole.exe | 结尾补充 |
| — | 返回结果 | 前端据此判断是否查公告（机制二） |

---

## 注册表键

所有注册表操作均位于 `HKLM\SOFTWARE\FaceLogin`：

| 键 | 用途 |
|---|---|
| `InstallPath` | 安装目录（`IsInstalled` / 公告升级判断依据） |
| `DataPath` | 数据目录（C++ 端追加 `\models`、`\data`、`\log`） |

---

## 常见问题

**改了前端/Go 代码但弹窗没变？**
→ 重新 `wails build`。`resources/` 内嵌和前端绑定都在构建时生成/打包。

**公告弹窗没弹，但确认开启了开关？**
→ 检查是否首次安装（无旧版）。可在 `service.log` 或注册表 `InstallPath` 确认已安装状态。

**升级后用户 config 被意外改动？**
→ 检查 `ConfigUpgradeForcedDefaults` 是否列入了本版本**未调整**的键。只应列出本版本真正改过默认值的键。
