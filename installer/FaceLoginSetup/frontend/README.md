# FaceLoginSetup 前端

这里是 `FaceLoginSetup` 安装器使用的 Vue 3 + TypeScript + Vite 前端。它与 Go/Wails 后端共同构成安装、升级、卸载界面，不是独立部署的网站。

## 开发与构建

在本目录执行：

```powershell
npm install
npm run dev
```

生产构建由 Wails 调用：

```powershell
npm run build
```

`prebuild` 会先运行仓库根目录的 `scripts/sync-locales.mjs`，把根目录语言包同步到安装器使用的位置。提交前运行：

```powershell
node ../../../scripts/check-locales.mjs
```

完整安装器流程（包含 Go 绑定、资源嵌入、轻量卸载器和最终安装器）请参阅上级目录的 [README.md](../README.md) 与 [DEVELOPMENT.md](../DEVELOPMENT.md)。

## 前端约定

- 安装器界面支持简体中文和 English；公告文本位于 `src/notice-zh.json` 与 `src/notice-en.json`。
- 业务调用通过 `wailsjs/go/main` 生成的 Wails 绑定完成，不在组件中直接执行系统命令。
- 完整安装器嵌入 `resources/`；使用 `uninstaller` build tag 构建的独立卸载器不嵌入安装资源。
- 修改公告、路径校验、快捷方式或自定义弹窗时，同时检查 `installer/FaceLoginSetup/DEVELOPMENT.md` 中的安装器流程和人工验收清单。

推荐使用 VS Code + Vue Language Tools（Volar）进行 Vue/TypeScript 编辑。
