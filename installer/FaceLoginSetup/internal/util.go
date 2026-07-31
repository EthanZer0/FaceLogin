package internal

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows/registry"
)

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
	cmd := exec.Command(name, args...)
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
