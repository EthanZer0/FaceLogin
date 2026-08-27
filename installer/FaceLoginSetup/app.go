package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"

	"FaceLoginSetup/internal"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

// App struct
type App struct {
	ctx context.Context
}

// NewApp creates a new App application struct
func NewApp() *App {
	return &App{}
}

// startup is called when the app starts. The context is saved so we can call runtime methods.
func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	// Set up the embedded FS reference for extraction
	internal.EmbeddedFS = resources
}

// ProgressEvent is sent to the frontend during install/uninstall.
type ProgressEvent struct {
	Percent int    `json:"percent"`
	Step    string `json:"step"`
	Status  string `json:"status"` // "running", "done", "warn", "fail"
	Detail  string `json:"detail,omitempty"`
}

func (a *App) emit(percent int, step, status, detail string) {
	runtime.EventsEmit(a.ctx, "setup:progress", ProgressEvent{
		Percent: percent,
		Step:    step,
		Status:  status,
		Detail:  detail,
	})
}

// i18nMessage keeps backend errors and variable progress details localizable
// without making the Go installer load a second copy of the locale catalog.
// The frontend decodes this small envelope and renders it through the shared
// JSON language pack. Plain strings remain supported for OS/library errors.
func i18nMessage(key string, values map[string]interface{}) string {
	payload, err := json.Marshal(map[string]interface{}{
		"key":    key,
		"values": values,
	})
	if err != nil {
		return key
	}
	return string(payload)
}

// GetDefaultPaths returns default install and data paths for the frontend.
func (a *App) GetDefaultPaths() map[string]string {
	return map[string]string{
		"installDir": internal.ReadRegString(REGVAL_INSTALL_PATH, internal.GetDefaultInstallDir()),
	}
}

// PickDirectory opens a native folder picker dialog and returns the selected path.
// Returns empty string if the user cancels.
func (a *App) PickDirectory(title string) string {
	dir, err := runtime.OpenDirectoryDialog(a.ctx, runtime.OpenDialogOptions{
		Title: title,
	})
	if err != nil {
		return ""
	}
	return dir
}

// IsInstalled checks whether FaceLogin is already installed by verifying
// the registry InstallPath exists and the install directory itself exists.
func (a *App) IsInstalled() bool {
	installDir := internal.ReadRegString(REGVAL_INSTALL_PATH, "")
	if installDir == "" {
		return false
	}
	return internal.DirExists(installDir)
}

// GetUpgradeNotice returns the per-release "what's new" announcement content,
// or an empty map when this release has no announcement (or this is a fresh
// first-time install, not an upgrade). The frontend calls this after an
// install completes and shows a popup if non-empty.
func (a *App) GetUpgradeNotice() map[string]interface{} {
	return internal.GetUpgradeNotice()
}

// Install runs the full installation.
func (a *App) Install(installDir string, locale string) map[string]interface{} {
	var err error

	a.emit(0, "installer.progress.startInstall", "running", "")
	installDir = filepath.Clean(installDir)

	// Step 1: Stop and delete existing service
	a.emit(0, "installer.progress.stopExistingService", "running", "")
	if err = internal.StopAndDeleteService(); err != nil {
		return result(false, err.Error())
	}
	a.emit(12, "installer.progress.stopExistingService", "done", "")

	// Step 2: Create directories
	a.emit(12, "installer.progress.createDirectory", "running", "")
	if err = os.MkdirAll(installDir, 0755); err != nil {
		return result(false, err.Error())
	}
	a.emit(25, "installer.progress.createDirectory", "done", "")

	// Step 3: Write registry paths — DataPath is the install dir itself, C++ appends \models
	a.emit(25, "installer.progress.configureRegistry", "running", "")
	if err = internal.WriteRegString(REGVAL_INSTALL_PATH, installDir); err != nil {
		return result(false, i18nMessage("installer.error.writeInstallPath", map[string]interface{}{"error": err.Error()}))
	}
	if err = internal.WriteRegString(REGVAL_DATA_PATH, installDir); err != nil {
		return result(false, i18nMessage("installer.error.writeDataPath", map[string]interface{}{"error": err.Error()}))
	}
	a.emit(30, "installer.progress.configureRegistry", "done", "")

	// Step 4: Extract all embedded resources
	a.emit(30, "installer.progress.copyFiles", "running", "")
	if err = internal.ExtractAll(installDir, func(step, total int, name string) {
		a.emit(30+step*30/total, "installer.progress.copyFiles", "running", name)
	}); err != nil {
		return result(false, i18nMessage("installer.error.copyFiles", map[string]interface{}{"error": err.Error()}))
	}
	// Step 4.5: Create data/ and log/ directories, ensure config.json defaults
	dataDir := filepath.Join(installDir, "data")
	logDir := filepath.Join(installDir, "log")
	os.MkdirAll(dataDir, 0755)
	os.MkdirAll(logDir, 0755)
	configPath := filepath.Join(dataDir, "config.json")
	// Selectively force the adjusted default parameters (match/anti-spoof
	// thresholds) onto any existing config, and create a default config when
	// none exists. Other user settings are preserved.
	if err := internal.EnsureConfigDefaults(configPath, locale); err != nil {
		a.emit(60, "installer.progress.writeDefaults", "warn", err.Error())
		// Persist the failure so a silent config miss (e.g. match_threshold
		// not force-synced) can be diagnosed from the install dir.
		logErr := os.WriteFile(filepath.Join(installDir, "data", "install.log"),
			[]byte("EnsureConfigDefaults failed: "+err.Error()+"\n"), 0644)
		if logErr != nil {
			a.emit(60, "installer.progress.writeDefaults", "warn", "install.log: "+logErr.Error())
		}
	} else {
		a.emit(60, "installer.progress.writeDefaults", "done", "")
	}

	a.emit(60, "installer.progress.copyFiles", "done", "")

	// Step 5: Set data directory ACL
	a.emit(60, "installer.progress.secureDataDirectory", "running", "")
	if err = internal.SetDirectoryACL(installDir); err != nil {
		a.emit(60, "installer.progress.secureDataDirectory", "warn", err.Error())
	} else {
		a.emit(67, "installer.progress.secureDataDirectory", "done", "")
	}

	// Step 6: Register COM DLL
	a.emit(67, "installer.progress.registerProvider", "running", "")
	dllPath := filepath.Join(installDir, "FaceLoginCredentialProvider.dll")
	if err = internal.RegisterCOMDLL(dllPath); err != nil {
		a.emit(67, "installer.progress.registerProvider", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(75, "installer.progress.registerProvider", "done", "")

	// Step 7: Install service (stops old service first)
	a.emit(75, "installer.progress.installService", "running", "")
	svcPath := filepath.Join(installDir, "FaceLoginService.exe")
	if err = internal.InstallService(svcPath); err != nil {
		a.emit(75, "installer.progress.installService", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(90, "installer.progress.installService", "done", "")

	// Step 8: Finalize
	enrollDest := filepath.Join(installDir, "FaceLoginConsole.exe")
	_ = internal.ExtractResource("resources/FaceLoginConsole.exe", enrollDest)

	a.emit(100, "installer.progress.complete", "done", "")
	return result(true, "installer.result.installed")
}

// Uninstall runs the full uninstallation.
func (a *App) Uninstall() map[string]interface{} {
	var err error

	a.emit(0, "installer.progress.startUninstall", "running", "")

	// Step 1: Stop and delete service
	a.emit(0, "installer.progress.stopService", "running", "")
	if err = internal.StopAndDeleteService(); err != nil {
		a.emit(0, "installer.progress.stopService", "fail", err.Error())
		return result(false, err.Error())
	}
	a.emit(30, "installer.progress.stopService", "done", "")

	// Step 2: Unregister COM DLL
	a.emit(30, "installer.progress.unregisterProvider", "running", "")
	installDir := internal.ReadRegString(REGVAL_INSTALL_PATH, "")
	if installDir != "" {
		dllPath := filepath.Join(installDir, "FaceLoginCredentialProvider.dll")
		if err = internal.UnregisterCOMDLL(dllPath); err != nil {
			a.emit(30, "installer.progress.unregisterProvider", "warn", err.Error())
		} else {
			a.emit(50, "installer.progress.unregisterProvider", "done", "")
		}
	} else {
		internal.UnregisterCOMDLL("")
		a.emit(50, "installer.progress.unregisterProvider", "done", "")
	}

	// Step 3: Delete installed program files AND user data (data/, log/) and
	// remove the install directory if it becomes empty. This is a FULL purge —
	// enrolled faces, config, and logs are gone (the frontend warns about this
	// before uninstall). The directory itself is only removed after every file
	// we own has been deleted and the folder is confirmed empty, so an
	// unexpected/unknown file (not deployed by us) leaves the dir in place and
	// never gets silently wiped.
	a.emit(50, "installer.progress.deleteFiles", "running", "")
	if installDir != "" && internal.DirExists(installDir) {
		removed, rmErr := internal.RemoveInstalledFiles(installDir, true)
		if rmErr != nil {
			a.emit(50, "installer.progress.deleteFiles", "warn",
				i18nMessage("installer.uninstall.filesPartiallyRemoved", map[string]interface{}{"count": removed, "error": rmErr.Error()}))
		} else {
			a.emit(70, "installer.progress.deleteFiles", "done",
				i18nMessage("installer.uninstall.filesRemoved", map[string]interface{}{"count": removed}))
		}

		// Remove the (now empty) install directory — guarded so it only
		// happens when nothing remains.
		if internal.DirExists(installDir) {
			if _, err := internal.RemoveInstalledDir(installDir); err != nil {
				a.emit(70, "installer.progress.deleteFiles", "warn",
					i18nMessage("installer.uninstall.directoryKept", map[string]interface{}{"error": err.Error()}))
			}
		}
	} else {
		a.emit(70, "installer.progress.deleteFiles", "done", "")
	}

	// Step 4: Clean registry — remove the whole HKLM\SOFTWARE\FaceLogin key
	// (InstallPath/DataPath plus the runtime values UserLoggedIn,
	// ServiceStartUptime, AboutSeenVersion written by the service and console).
	// DeleteRegValue() alone would only remove two values and leave the key and
	// the orphaned runtime values behind.
	a.emit(70, "installer.progress.cleanRegistry", "running", "")
	_ = internal.DeleteRegKey()
	a.emit(85, "installer.progress.cleanRegistry", "done", "")

	// Step 5: Notify complete
	a.emit(85, "installer.progress.complete", "running", "")
	a.emit(100, "installer.progress.complete", "done", "installer.result.uninstalled")

	return result(true, "installer.result.uninstalled")
}

func result(success bool, message string) map[string]interface{} {
	return map[string]interface{}{
		"success": success,
		"message": message,
	}
}
