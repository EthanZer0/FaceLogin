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
	internal.NoticeVersion = "1.7.0"
	internal.NoticeTitle = "FaceLogin 1.7.0 更新说明"
	internal.NoticeBody = "新功能：\n" +
		"- 锁屏陌生人即时反馈：人脸匹配失败约 1 秒内提示，不再干等 15 秒；匹配失败与超时提示区分\n" +
		"- 服务崩溃自动重启，服务未运行时锁屏磁贴即时提示\n" +
		"- 眼镜佩戴场景识别优化\n" +
		"解锁提速：\n" +
		"- 认证总耗时进一步缩短（匹配共识 2 帧、管道轮询 10ms、按键触发门限 300ms）\n" +
		"修复：\n" +
		"- 录入卡 80/90% 无响应彻底根治（#11）\n" +
		"- 反欺诈误拒修复：裁剪越界黑边 + 多帧投票 + 阈值重校准\n" +
		"- 日志轮转误删修复\n" +
		"提示：\n" +
		"- 已录入的人脸数据不受影响，无需重新录入"

	// Initialize the embedded resource filesystem in the internal package
	internal.EmbeddedFS = resources

	app := NewApp()

	err := wails.Run(&options.App{
		Title:        "FaceLogin 安装程序",
		Width:        640,
		Height:       520,
		DisableResize: true, // fixed-size window — no edge resize, no maximize
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
