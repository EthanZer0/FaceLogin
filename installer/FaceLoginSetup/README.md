# FaceLoginSetup

FaceLoginSetup 是 FaceLogin 的 Windows 图形安装器，使用 Go、Wails v2、Vue 3、TypeScript 和 WebView2 构建。它负责安装、升级和卸载 FaceLogin 的服务、Credential Provider、Console、模型、语言包及运行库。

## 开发

完整的安装器开发说明请参阅 [DEVELOPMENT.md](DEVELOPMENT.md)，整体项目架构请参阅仓库根目录的 [DEVELOPMENT.md](../../DEVELOPMENT.md)。

前置条件：Windows x64、Go 1.25+、Node.js/npm 和 Wails CLI v2。

```powershell
cd installer\FaceLoginSetup

# 本地前端开发
wails dev

# 发布构建：先生成轻量 Uninstall.exe，再生成完整 FaceLoginSetup.exe
.\build-installer.ps1
```

`build-installer.ps1` 会将 `resources/` 中的 C++ 程序、模型、运行库和语言包嵌入完整安装器；独立卸载器使用 `uninstaller` build tag 构建，不包含这些安装资源，因此体积更小。
