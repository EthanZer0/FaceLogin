package main

import (
	"embed"
	"fmt"
	"os"

	"FaceLoginSetup/internal"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

//go:embed all:frontend/dist
var assets embed.FS

//go:embed all:resources
var resources embed.FS

const SERVICE_NAME = "FaceLoginService"

// Registry paths
const REG_KEY = `SOFTWARE\FaceLogin`
const REGVAL_DATA_PATH = "DataPath"
const REGVAL_INSTALL_PATH = "InstallPath"

func main() {
	// Check administrator — if not elevated, relaunch as admin
	if !internal.IsAdmin() {
		err := internal.Elevate()
		if err != nil {
			fmt.Fprintf(os.Stderr, "Elevation failed: %v\n", err)
		}
		os.Exit(0)
	}

	// =========================================================================
	// Custom action area — PER-RELEASE upgrade actions.
	//
	// Two independent, version-scoped switches live here:
	//
	//   A) Config upgrade  — force-sync changed default parameters onto
	//      existing installs (see internal/config.go).
	//   B) Upgrade notice  — show a "what's new" popup after an upgrade
	//      install completes (see internal/notice.go). Only shown when the
	//      install is an upgrade (a previous version is already installed),
	//      never on a fresh first-time install.
	//
	// Both default to OFF. Each release that needs an action turns the
	// relevant switch ON here; future releases leave them OFF so stale
	// overrides/announcements never re-apply.
	//
	//   internal.ConfigUpgradeEnabled = true   // A: sync thresholds
	//   internal.ConfigUpgradeForcedDefaults = map[string]any{
	//       "match_threshold":      0.30,   // threshold changed in this release
	//       "anti_spoof_threshold": 0.30,
	//   }
	//
	//   internal.NoticeEnabled  = true            // B: announcement popup
	//   internal.NoticeVersion  = "1.2.0"         // badge shown in the popup
	//   internal.NoticeTitle    = "FaceLogin 1.2.0 更新说明"
	//   internal.NoticeBody     = "行1\n行2\n行3"  // one bullet per line
	//
	// v1.0.1: threshold defaults changed (match strictness 70 / anti-spoof 0.30).
	// v1.2.0: no forced config overrides needed (thresholds unchanged; the new
	// camera_device field defaults to "" = first device automatically). The
	// V2→V3 database migration cannot be automated (requires re-enrollment),
	// so it is surfaced via the upgrade notice below instead.
	// v1.3.0: no forced config overrides needed. The V3→V4 database migration
	// (multi-face support) is fully backward compatible — old data is upgraded
	// in memory on load, no re-enrollment required.
	// v1.4.0: no forced config overrides needed. Removed the unused legacy dlib
	// models (recognizer + HOG detector) — smaller installer, no re-enrollment.
	internal.ConfigUpgradeEnabled = false

	// =========================================================================
	// B) Upgrade notice — per-release announcement shown only on UPGRADE.
	// =========================================================================
	internal.NoticeEnabled = true
	internal.NoticeVersion = "1.5.0"
	internal.NoticeTitle = "FaceLogin 1.5.0 更新说明"
	internal.NoticeBody = "新功能：\n" +
		"- 控制台界面全面重构：浅色新设计，新增实时读数条（活体/采样/分辨率）\n" +
		"- 设置页下拉框、勾选框、调节条全部定制化，与界面统一\n" +
		"- 日志页改进：来源下拉框定制化、空日志占位、级别显示更准、始终显示最新日志\n" +
		"- 锁屏一次按键即触发人脸识别，不再需要按两次\n" +
		"- 账号身份弹窗新增「仅清理账号邮箱」，一键清除残留邮箱、恢复锁屏登录\n" +
		"- 冷启动人脸识别加速：锁屏出现即可识别，模型后台加载、曝光预热自适应\n" +
		"- 控制台新增「关于」卡片：点击右下角版本号弹出，含贡献者名单与 GitHub 链接，带星屑飘落特效\n\n" +
		"修复：\n" +
		"- 修复 MSA 身份误判导致的\"账号身份已变更\"误报与锁屏登录失败\n" +
		"- 修复锁屏后 Console 摄像头不恢复、画面卡死的问题\n" +
		"- 修复控制台打开时模型加载阻塞、无法切换页面的问题\n" +
		"- 修复服务日志频繁出现 WriteFile failed: 232 误报的问题\n" +
		"- 修复录入过程中切换页面导致采集完成页重叠、标签栏不同步的问题\n" +
		"- 修复提示横幅长文本溢出的问题\n\n" +
		"说明：\n" +
		"- 现有的人脸数据和配置完全兼容，无需重新录入"

	// Initialize the embedded resource filesystem in the internal package
	internal.EmbeddedFS = resources

	app := NewApp()

	err := wails.Run(&options.App{
		Title:            "FaceLogin 安装程序",
		Width:            640,
		Height:           520,
		WindowStartState: options.Normal,
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		BackgroundColour: &options.RGBA{R: 255, G: 255, B: 255, A: 1},
		OnStartup:        app.startup,
		Bind: []interface{}{
			app,
		},
	})
	if err != nil {
		println("Error:", err.Error())
	}
}
