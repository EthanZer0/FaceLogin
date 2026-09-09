package internal

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"golang.org/x/sys/windows"
)

const standaloneUninstallerName = "Uninstall.exe"

// LaunchUninstallCleanup copies the running uninstaller to a temporary file
// and starts it in cleanup mode. The worker owns no product resources, so it
// can safely wait for the UI process and delete the executable that launched
// it. This avoids a reboot-only delete and leaves no program file behind.
func LaunchUninstallCleanup(installDir string) error {
	exe, err := os.Executable()
	if err != nil {
		return fmt.Errorf("get uninstaller path: %w", err)
	}
	if !strings.EqualFold(filepath.Base(exe), standaloneUninstallerName) {
		return fmt.Errorf("not running from %s", standaloneUninstallerName)
	}

	cleanupPath := filepath.Join(os.TempDir(), fmt.Sprintf("UninstallCleanup-%d.exe", os.Getpid()))
	if err := CopyFile(exe, cleanupPath); err != nil {
		return fmt.Errorf("copy cleanup helper: %w", err)
	}

	cmd := hiddenCommand(cleanupPath, "--cleanup", strconv.Itoa(os.Getpid()), installDir, exe)
	// Never inherit the install directory as the cleanup worker's current
	// directory. Windows refuses to remove a process's current directory even
	// when it is empty, which would leave an otherwise fully uninstalled folder
	// behind.
	cmd.Dir = os.TempDir()
	if err := cmd.Start(); err != nil {
		_ = os.Remove(cleanupPath)
		return fmt.Errorf("start cleanup helper: %w", err)
	}
	return nil
}

// RunUninstallCleanup recognizes the private helper invocation before Wails is
// initialized. It returns true only when valid cleanup arguments were handled.
func RunUninstallCleanup(args []string) bool {
	if len(args) != 4 || args[0] != "--cleanup" {
		return false
	}
	pid, err := strconv.Atoi(args[1])
	if err != nil || pid <= 0 {
		return true
	}
	installDir := filepath.Clean(args[2])
	uninstallerPath := filepath.Clean(args[3])
	if !strings.EqualFold(filepath.Base(uninstallerPath), standaloneUninstallerName) ||
		!strings.EqualFold(filepath.Dir(uninstallerPath), installDir) {
		return true
	}

	waitForProcessExit(uint32(pid))
	// Wails shutdown can briefly retain the executable after its process handle
	// signals, so retry for a bounded interval before preserving the directory.
	for attempt := 0; attempt < 30; attempt++ {
		if err := os.Remove(uninstallerPath); err == nil || os.IsNotExist(err) {
			break
		}
		time.Sleep(time.Second)
	}
	_, _ = RemoveInstalledDir(installDir)
	scheduleOwnTempDeletion()
	return true
}

func waitForProcessExit(pid uint32) {
	process, err := windows.OpenProcess(windows.SYNCHRONIZE, false, pid)
	if err != nil {
		return
	}
	defer windows.CloseHandle(process)
	_, _ = windows.WaitForSingleObject(process, windows.INFINITE)
}

func scheduleOwnTempDeletion() {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	// This command only removes our generated file under %TEMP%; it never
	// receives a user-controlled install path and runs hidden to avoid a console
	// flash during uninstall completion.
	command := fmt.Sprintf(`ping 127.0.0.1 -n 2 >nul & del /f /q "%s"`, exe)
	cmd := exec.Command(os.Getenv("ComSpec"), "/d", "/s", "/c", command)
	cmd.SysProcAttr = hiddenCommand(os.Getenv("ComSpec")).SysProcAttr
	cmd.Dir = os.TempDir()
	_ = cmd.Start()
}
