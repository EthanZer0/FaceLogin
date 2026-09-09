package internal

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

const desktopShortcutName = "FaceLogin Console.lnk"

var (
	ole32            = windows.NewLazySystemDLL("ole32.dll")
	coCreateInstance = ole32.NewProc("CoCreateInstance")

	clsidShellLink = windows.GUID{Data1: 0x00021401, Data4: [8]byte{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	iidShellLinkW  = windows.GUID{Data1: 0x000214f9, Data4: [8]byte{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
	iidPersistFile = windows.GUID{Data1: 0x0000010b, Data4: [8]byte{0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}}
)

const (
	clsctxInprocServer    = 0x1
	coinitApartmentThread = 0x2
)

// CreateDesktopShortcut creates a native Windows Shell Link. It calls
// IShellLinkW/IPersistFile directly, without WScript.Shell, PowerShell, or a
// helper process.
func CreateDesktopShortcut(installDir string) (err error) {
	installDir = filepath.Clean(installDir)
	target := filepath.Join(installDir, "FaceLoginConsole.exe")
	if !FileExists(target) {
		return fmt.Errorf("installer.error.desktopShortcutTargetMissing")
	}

	shortcutPath, err := desktopShortcutPath()
	if err != nil {
		return fmt.Errorf("installer.error.desktopShortcutUnavailable")
	}
	if FileExists(shortcutPath) {
		existingTarget, targetErr := readShortcutTarget(shortcutPath)
		if targetErr != nil || !samePath(existingTarget, target) {
			return fmt.Errorf("installer.error.desktopShortcutConflict")
		}
	}

	return withCOM(func() error {
		link, err := createShellLink()
		if err != nil {
			return err
		}
		defer comRelease(link)

		if err := shellLinkSetString(link, 20, target); err != nil { // SetPath
			return fmt.Errorf("set shortcut target: %w", err)
		}
		if err := shellLinkSetString(link, 9, installDir); err != nil { // SetWorkingDirectory
			return fmt.Errorf("set shortcut working directory: %w", err)
		}
		if err := shellLinkSetString(link, 7, "FaceLogin 人脸识别登录控制台"); err != nil { // SetDescription
			return fmt.Errorf("set shortcut description: %w", err)
		}
		iconPath, err := windows.UTF16PtrFromString(target)
		if err != nil {
			return err
		}
		if err := comCall(link, 17, uintptr(unsafe.Pointer(iconPath)), 0); err != nil { // SetIconLocation
			return fmt.Errorf("set shortcut icon: %w", err)
		}

		persist, err := queryInterface(link, &iidPersistFile)
		if err != nil {
			return fmt.Errorf("query IPersistFile: %w", err)
		}
		defer comRelease(persist)
		shortcutPathPtr, err := windows.UTF16PtrFromString(shortcutPath)
		if err != nil {
			return err
		}
		if err := comCall(persist, 6, uintptr(unsafe.Pointer(shortcutPathPtr)), 1); err != nil { // IPersistFile::Save
			return fmt.Errorf("save desktop shortcut: %w", err)
		}
		return nil
	})
}

// RemoveDesktopShortcut removes only our shortcut when it points to the
// current installation. A same-named shortcut targeting another application
// is left untouched.
func RemoveDesktopShortcut(installDir string) error {
	shortcutPath, err := desktopShortcutPath()
	if err != nil || !FileExists(shortcutPath) {
		return nil
	}
	target, err := readShortcutTarget(shortcutPath)
	if err != nil || !samePath(target, filepath.Join(filepath.Clean(installDir), "FaceLoginConsole.exe")) {
		return nil
	}
	if err := os.Remove(shortcutPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func desktopShortcutPath() (string, error) {
	desktop, err := windows.KnownFolderPath(windows.FOLDERID_Desktop, windows.KF_FLAG_DEFAULT)
	if err != nil {
		return "", err
	}
	return filepath.Join(desktop, desktopShortcutName), nil
}

func readShortcutTarget(path string) (target string, err error) {
	var result string
	err = withCOM(func() error {
		link, createErr := createShellLink()
		if createErr != nil {
			return createErr
		}
		defer comRelease(link)
		persist, queryErr := queryInterface(link, &iidPersistFile)
		if queryErr != nil {
			return queryErr
		}
		defer comRelease(persist)
		shortcutPathPtr, pathErr := windows.UTF16PtrFromString(path)
		if pathErr != nil {
			return pathErr
		}
		if loadErr := comCall(persist, 5, uintptr(unsafe.Pointer(shortcutPathPtr)), 0); loadErr != nil { // IPersistFile::Load
			return loadErr
		}

		buffer := make([]uint16, 32768)
		if callErr := comCall(link, 3, uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer)), 0, 0); callErr != nil { // GetPath
			return callErr
		}
		result = windows.UTF16ToString(buffer)
		return nil
	})
	return result, err
}

func withCOM(fn func() error) (err error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer func() {
		if recovered := recover(); recovered != nil {
			err = fmt.Errorf("installer.error.desktopShortcutFailed")
		}
	}()
	if err := windows.CoInitializeEx(0, coinitApartmentThread); err != nil {
		return err
	}
	defer windows.CoUninitialize()
	return fn()
}

func createShellLink() (unsafe.Pointer, error) {
	var link unsafe.Pointer
	ret, _, _ := coCreateInstance.Call(
		uintptr(unsafe.Pointer(&clsidShellLink)),
		0,
		clsctxInprocServer,
		uintptr(unsafe.Pointer(&iidShellLinkW)),
		uintptr(unsafe.Pointer(&link)),
	)
	if err := hresultError(ret); err != nil {
		return nil, fmt.Errorf("CoCreateInstance IShellLinkW: %w", err)
	}
	return link, nil
}

func queryInterface(object unsafe.Pointer, iid *windows.GUID) (unsafe.Pointer, error) {
	var result unsafe.Pointer
	if err := comCall(object, 0, uintptr(unsafe.Pointer(iid)), uintptr(unsafe.Pointer(&result))); err != nil {
		return nil, err
	}
	return result, nil
}

func shellLinkSetString(object unsafe.Pointer, methodIndex uintptr, value string) error {
	ptr, err := windows.UTF16PtrFromString(value)
	if err != nil {
		return err
	}
	return comCall(object, methodIndex, uintptr(unsafe.Pointer(ptr)))
}

func comCall(object unsafe.Pointer, methodIndex uintptr, args ...uintptr) error {
	vtable := *(*uintptr)(object)
	method := *(*uintptr)(unsafe.Pointer(vtable + methodIndex*unsafe.Sizeof(uintptr(0))))
	callArgs := make([]uintptr, 0, len(args)+1)
	callArgs = append(callArgs, uintptr(object))
	callArgs = append(callArgs, args...)
	ret, _, _ := syscall.SyscallN(method, callArgs...)
	return hresultError(ret)
}

func comRelease(object unsafe.Pointer) {
	if object == nil {
		return
	}
	vtable := *(*uintptr)(object)
	method := *(*uintptr)(unsafe.Pointer(vtable + 2*unsafe.Sizeof(uintptr(0))))
	_, _, _ = syscall.SyscallN(method, uintptr(object))
}

func hresultError(value uintptr) error {
	if int32(value) < 0 {
		return fmt.Errorf("HRESULT 0x%08X", uint32(value))
	}
	return nil
}

func samePath(left, right string) bool {
	return strings.EqualFold(filepath.Clean(left), filepath.Clean(right))
}
