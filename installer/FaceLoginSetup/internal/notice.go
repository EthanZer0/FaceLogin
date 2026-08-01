package internal

// UpgradeNotice is the version-scoped "what's new" popup shown after an
// upgrade install completes.
//
// This mirrors the ConfigUpgradeEnabled pattern in config.go:
//   - NoticeEnabled defaults to OFF. Each release that wants to surface an
//     announcement to existing users turns it ON in main.go's custom action
//     area (and fills in NoticeVersion / NoticeTitle / NoticeBody).
//   - It is shown ONLY when the install is an UPGRADE (a previous install
//     already exists), never on a fresh first-time install.
//   - Later releases leave the switch OFF so no stale announcement ever
//     pops up again.
var NoticeEnabled = false

// NoticeVersion is the release version this announcement belongs to
// (e.g. "1.2.0"). Shown as a badge in the popup.
var NoticeVersion = ""

// NoticeTitle is the one-line headline, e.g. "FaceLogin 1.2.0 更新说明".
var NoticeTitle = ""

// NoticeBody is the announcement text. Lines separated by "\n" are rendered
// as separate bullet items in the popup.
var NoticeBody = ""

// GetUpgradeNotice returns the announcement content ONLY when:
//   1. This release opted in (NoticeEnabled), AND
//   2. FaceLogin is already installed (i.e. this run is an upgrade, not a
//      first-time install).
//
// Returns an empty map when no notice should be shown, so the frontend can
// simply skip rendering the popup.
func GetUpgradeNotice() map[string]interface{} {
	if !NoticeEnabled || NoticeVersion == "" || NoticeTitle == "" {
		return map[string]interface{}{}
	}

	// Only show on upgrade. A fresh install has no previous version to
	// "announce" the update to. Use the same existence check IsInstalled()
	// relies on: the InstallPath registry value + directory present.
	installDir := ReadRegString("InstallPath", "")
	if installDir == "" || !DirExists(installDir) {
		return map[string]interface{}{}
	}

	return map[string]interface{}{
		"version": NoticeVersion,
		"title":   NoticeTitle,
		"body":    NoticeBody,
	}
}
