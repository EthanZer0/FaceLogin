package internal

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	shell32        = windows.NewLazySystemDLL("shell32.dll")
	procShellExecW = shell32.NewProc("ShellExecuteW")
)

// IsAdmin checks if the current process is running elevated.
func IsAdmin() bool {
	var token windows.Token
	err := windows.OpenProcessToken(
		windows.CurrentProcess(),
		windows.TOKEN_QUERY,
		&token,
	)
	if err != nil {
		return false
	}
	defer token.Close()

	var elevation struct {
		TokenIsElevated uint32
	}
	var size uint32
	err = windows.GetTokenInformation(
		token,
		windows.TokenElevation,
		(*byte)(unsafe.Pointer(&elevation)),
		uint32(unsafe.Sizeof(elevation)),
		&size,
	)
	if err != nil {
		return false
	}
	return elevation.TokenIsElevated != 0
}

// Elevate relaunches the current executable as administrator.
func Elevate() error {
	exe, err := os.Executable()
	if err != nil {
		return fmt.Errorf("get executable path: %w", err)
	}

	verb, err := syscall.UTF16PtrFromString("runas")
	if err != nil {
		return err
	}
	file, err := syscall.UTF16PtrFromString(exe)
	if err != nil {
		return err
	}
	cwd, err := syscall.UTF16PtrFromString("")
	if err != nil {
		return err
	}

	ret, _, _ := procShellExecW.Call(
		0,
		uintptr(unsafe.Pointer(verb)),
		uintptr(unsafe.Pointer(file)),
		0,
		uintptr(unsafe.Pointer(cwd)),
		1,
	)
	if ret <= 32 {
		return fmt.Errorf("ShellExecuteW failed with code %d", ret)
	}
	return nil
}
