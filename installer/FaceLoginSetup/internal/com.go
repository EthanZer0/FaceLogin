package internal

import (
	"fmt"
	"os"
	"os/exec"
)

// RegisterCOMDLL registers a COM DLL via regsvr32.
func RegisterCOMDLL(dllPath string) error {
	if !FileExists(dllPath) {
		return fmt.Errorf("DLL not found: %s", dllPath)
	}
	cmd := exec.Command("regsvr32", "/s", dllPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("regsvr32 failed: %w\n%s", err, string(out))
	}
	return nil
}

// UnregisterCOMDLL unregisters a COM DLL via regsvr32.
// If the DLL file is already gone, cleans registry directly.
func UnregisterCOMDLL(dllPath string) error {
	if FileExists(dllPath) {
		cmd := exec.Command("regsvr32", "/s", "/u", dllPath)
		out, err := cmd.CombinedOutput()
		if err != nil {
			return fmt.Errorf("regsvr32 /u failed: %w\n%s", err, string(out))
		}
		return nil
	}

	// DLL gone — clean registry keys directly
	clsid := "{B8F4C7A1-3D5E-4F2B-A9C6-1D8E7F3A5B2C}"
	cpKey := fmt.Sprintf(
		`SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\%s`,
		clsid,
	)
	exec.Command("reg", "delete", fmt.Sprintf(`HKLM\%s`, cpKey), "/f").Run()
	exec.Command("reg", "delete", fmt.Sprintf(`HKCR\CLSID\%s`, clsid), "/f").Run()
	return nil
}

// SetDirectoryACL sets the ACL on a directory to SYSTEM + Administrators only.
func SetDirectoryACL(dirPath string) error {
	if !DirExists(dirPath) {
		return os.MkdirAll(dirPath, 0755)
	}
	cmd := exec.Command("icacls", dirPath,
		"/inheritance:r",
		"/grant", "SYSTEM:(OI)(CI)F",
		"/grant", "BUILTIN\\Administrators:(OI)(CI)F",
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("icacls failed: %w\n%s", err, string(out))
	}
	return nil
}
