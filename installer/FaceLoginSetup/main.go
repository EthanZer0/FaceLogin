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
	internal.ConfigUpgradeEnabled = false

	// =========================================================================
	// B) Upgrade notice — per-release announcement shown only on UPGRADE.
	// =========================================================================
	internal.NoticeEnabled = true
	internal.NoticeVersion = "1.3.0"
	internal.NoticeTitle = "FaceLogin 1.3.0 更新说明"
	internal.NoticeBody = "新增功能：\n" +
		"- 多账户支持：每个 Windows 账号可录入多张人脸\n" +
		"- 人脸管理：可给每张脸命名、删除单张脸或清空全部\n" +
		"- 首次使用自动引导选择摄像头\n" +
		"- 卸载时彻底清除（程序文件、人脸数据、日志）\n\n" +
		"修复：\n" +
		"- 修复锁屏时摄像头无法打开、指示灯不亮的问题\n" +
		"- 修复安装到中文路径时无法启动摄像头的问题\n\n" +
		"说明：\n" +
		"- 已有的人脸数据自动兼容，无需重新录入\n" +
		"- 同一账号重新采集人脸 = 新增一张脸，不再覆盖旧脸（每账号最多 5 张）\n" +
		"- 卸载会删除所有人脸数据且不可恢复，请谨慎操作"

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
