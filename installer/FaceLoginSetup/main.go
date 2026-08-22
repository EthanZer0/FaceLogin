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
	//
	// v1.8.0: match_threshold 0.65 → 0.75 (illumination-drift fix). NOTE: this
	// assignment OVERRIDES the config.go default — keep the two in sync and
	// set BOTH back to false in later releases.
	// v1.9.0: no forced config overrides needed — reverted to OFF (threshold
	// ship-in with 1.8.0; later releases preserve user-tuned values).
	internal.ConfigUpgradeEnabled = false

	// =========================================================================
	// B) Upgrade notice — per-release announcement shown only on UPGRADE.
	// =========================================================================
	internal.NoticeEnabled = true
	internal.NoticeVersion = "1.9.0"
	internal.NoticeTitle = "FaceLogin 1.9.0 更新说明"
	internal.NoticeBody = "新功能：\n" +
		"- 陌生人未匹配人脸记录（可选开启）：锁屏出现未匹配人脸时保存照片与记录，可在 Console 日志页浏览/删除（仅本机存储）\n" +
		"- 开机登录按键触发可选：默认开机自动识别，可在设置中改为按任意键开始（与锁屏一致）\n" +
		"识别可靠性：\n" +
		"- 匹配阈值重校准 0.65 → 0.75：环境光照变化（如换教室/宿舍）导致解锁失败的问题显著改善，陌生人拦截不受影响\n" +
		"- 内置 USB 摄像头帧率修复：优先压缩格式取流，720p 下稳定 30fps，人脸录入不再卡住\n" +
		"交互完善：\n" +
		"- 冷启动仅首次自动识别：识别失败/超时、或切换到其他登录磁贴再切回后，按任意键才重新识别\n" +
		"- 锁屏识别失败不再循环启动摄像头，切换密码磁贴时识别立即停止，密码输入不再被打断\n" +
		"修复：\n" +
		"- 修复微软账户重置 PIN 时摄像头反复调用、PIN 显示不可用的问题\n" +
		"- 修复从本地账号切换/绑定到微软账户后 Console 仍显示本地账号的问题\n" +
		"- 修复升级安装后匹配阈值未同步为新默认值的问题\n" +
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
