package internal

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/windows/registry"
)

// hiddenCommand starts a native Windows helper without creating a transient
// console window. The installer is a GUI application, so every command-line
// helper must use this wrapper; otherwise Windows may briefly attach a new
// console while the helper runs.
func hiddenCommand(name string, args ...string) *exec.Cmd {
	cmd := exec.Command(name, args...)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	return cmd
}

// ReadRegString reads a REG_SZ from HKLM\SOFTWARE\FaceLogin.
// Returns defaultValue if missing.
func ReadRegString(valueName, defaultValue string) string {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.QUERY_VALUE)
	if err != nil {
		return defaultValue
	}
	defer k.Close()

	val, _, err := k.GetStringValue(valueName)
	if err != nil {
		return defaultValue
	}
	return val
}

// WriteRegString writes a REG_SZ to HKLM\SOFTWARE\FaceLogin (creates key if needed).
func WriteRegString(valueName, value string) error {
	k, _, err := registry.CreateKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.SET_VALUE)
	if err != nil {
		return fmt.Errorf("create/open registry key: %w", err)
	}
	defer k.Close()
	return k.SetStringValue(valueName, value)
}

// DeleteRegValue deletes a value from HKLM\SOFTWARE\FaceLogin.
func DeleteRegValue(valueName string) error {
	k, err := registry.OpenKey(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`, registry.SET_VALUE)
	if err != nil {
		return nil // key already gone
	}
	defer k.Close()
	return k.DeleteValue(valueName)
}

// deleteRegKeyTree recursively deletes a registry key and everything beneath
// it (all subkeys, recursively, and all values). The standard
// registry.DeleteKey only deletes EMPTY keys (it wraps RegDeleteKeyW), so it
// fails on HKLM\SOFTWARE\FaceLogin which holds values and subkeys. We enumerate
// and delete depth-first instead.
func deleteRegKeyTree(parent registry.Key, subpath string) error {
	k, err := registry.OpenKey(parent, subpath, registry.ENUMERATE_SUB_KEYS|registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		if err == registry.ErrNotExist {
			return nil // already gone — idempotent
		}
		return err
	}
	defer k.Close()

	// Delete subkeys first (they can themselves hold deeper subkeys/values).
	subs, _ := k.ReadSubKeyNames(-1)
	for _, s := range subs {
		if err := deleteRegKeyTree(k, s); err != nil {
			return err
		}
	}

	// Delete all values.
	vals, _ := k.ReadValueNames(-1)
	for _, v := range vals {
		if err := k.DeleteValue(v); err != nil {
			// A value that vanished concurrently is fine; anything else is real.
			if err != registry.ErrNotExist {
				return err
			}
		}
	}

	// Now the key itself is empty → remove it.
	return registry.DeleteKey(parent, subpath)
}

// DeleteRegKey removes the complete HKLM\SOFTWARE\FaceLogin key tree,
// including installation settings and any service or console subkeys.
// It is used by uninstall so no FaceLogin registry state survives a full purge.
// Returns nil when the key does not exist (idempotent).
func DeleteRegKey() error {
	return deleteRegKeyTree(registry.LOCAL_MACHINE, `SOFTWARE\FaceLogin`)
}

// Path helpers

// GetDefaultInstallDir returns the default install path.
func GetDefaultInstallDir() string {
	return filepath.Join(os.Getenv("ProgramFiles"), "FaceLogin")
}

// FileExists checks if a file exists and is not a directory.
func FileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

// DirExists checks if a directory exists.
func DirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}

// IsSafeInstallDir returns true only when path looks like a real FaceLogin
// install directory, i.e. the directory name contains "FaceLogin" (case-
// insensitive) AND the directory holds FaceLoginService.exe. Used before an
// uninstall deletes the tree: without this guard, a corrupted/malicious
// InstallPath registry value (e.g. "C:\" or "C:\Users\<user>") would make
// os.RemoveAll recursively delete an arbitrary directory — catastrophic data
// loss. When this returns false, the uninstall must NOT delete the directory.
func IsSafeInstallDir(path string) bool {
	if !DirExists(path) {
		return false
	}

	// 1) Directory name must contain "FaceLogin" (case-insensitive).
	base := strings.ToLower(filepath.Base(filepath.Clean(path)))
	if !strings.Contains(base, "facelogin") {
		return false
	}

	// 2) The directory must contain the service executable — the strongest
	//    signal that this is really an install dir, not just a folder whose
	//    name happens to contain "FaceLogin".
	svcPath := filepath.Join(path, "FaceLoginService.exe")
	if !FileExists(svcPath) {
		return false
	}

	return true
}

// ValidateInstallDir validates a user-selected installation target before any
// service, registry, or filesystem mutation occurs. The installer accepts a
// custom parent directory, but the final component must remain the product
// directory so the registered service and uninstaller have a stable layout.
func ValidateInstallDir(path string) error {
	path = strings.TrimSpace(path)
	if path == "" {
		return fmt.Errorf("installer.error.installPathRequired")
	}
	if !filepath.IsAbs(path) || filepath.VolumeName(path) == "" {
		return fmt.Errorf("installer.error.installPathAbsolute")
	}

	clean := filepath.Clean(path)
	if !strings.EqualFold(filepath.Base(clean), "FaceLogin") {
		return fmt.Errorf("installer.error.installPathName")
	}

	// Allow normal subdirectories such as C:\Program Files\FaceLogin, but
	// reject the protected directory itself as the product target.
	protected := []string{
		os.Getenv("windir"),
		os.Getenv("SystemRoot"),
		os.Getenv("ProgramData"),
		os.Getenv("Public"),
		os.Getenv("UserProfile"),
		os.Getenv("ProgramFiles"),
		os.Getenv("ProgramFiles(x86)"),
	}
	for _, reserved := range protected {
		if reserved != "" && strings.EqualFold(clean, filepath.Clean(reserved)) {
			return fmt.Errorf("installer.error.installPathProtected")
		}
	}
	if filepath.Dir(clean) == clean {
		return fmt.Errorf("installer.error.installPathRoot")
	}

	info, err := os.Stat(clean)
	if err == nil {
		if !info.IsDir() {
			return fmt.Errorf("installer.error.installPathNotDirectory")
		}
		entries, readErr := os.ReadDir(clean)
		if readErr != nil {
			return fmt.Errorf("installer.error.installPathUnreadable")
		}
		if len(entries) > 0 && !IsSafeInstallDir(clean) &&
			!strings.EqualFold(ReadRegString("InstallPath", ""), clean) {
			return fmt.Errorf("installer.error.installPathNotFaceLogin")
		}
	} else if !os.IsNotExist(err) {
		return fmt.Errorf("installer.error.installPathUnavailable")
	}

	parent := filepath.Dir(clean)
	if parentInfo, parentErr := os.Stat(parent); parentErr == nil {
		if !parentInfo.IsDir() {
			return fmt.Errorf("installer.error.installParentNotDirectory")
		}
	} else if !os.IsNotExist(parentErr) {
		return fmt.Errorf("installer.error.installParentUnavailable")
	}

	return nil
}

// CopyFile copies a file from src to dst. Parent directories of dst must exist.
func CopyFile(src, dst string) error {
	data, err := os.ReadFile(src)
	if err != nil {
		return fmt.Errorf("read %s: %w", src, err)
	}
	return os.WriteFile(dst, data, 0644)
}

// RunCommand runs a command and returns stdout+stderr combined.
func RunCommand(name string, args ...string) (string, error) {
	cmd := hiddenCommand(name, args...)
	out, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(out)), err
}

// DefaultDLLs lists runtime DLLs that must be bundled alongside the service.
var DefaultDLLs = []string{
	"openblas.dll",
	"liblapack.dll",
	"libgfortran-5.dll",
	"libquadmath-0.dll",
	"libgcc_s_seh-1.dll",
	"libwinpthread-1.dll",
}
