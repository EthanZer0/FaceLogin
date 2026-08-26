package internal

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

// EmbeddedFS is set by the caller (main.go's //go:embed resources/*).
// The caller must assign it before calling ExtractAll.
var EmbeddedFS fs.FS

// ExtractResource extracts a single embedded resource to a destination path.
func ExtractResource(embeddedPath, destPath string) error {
	data, err := fs.ReadFile(EmbeddedFS, embeddedPath)
	if err != nil {
		return fmt.Errorf("read embedded %s: %w", embeddedPath, err)
	}
	return os.WriteFile(destPath, data, 0644)
}

// ExtractAll extracts all embedded resources to destDir, organized:
//
//	destDir/
//	  FaceLoginService.exe
//	  FaceLoginCredentialProvider.dll
//	  FaceLoginConsole.exe
//	  openblas.dll  (etc.)
//	  models/
//	    *.dat, *.onnx  (model files)
//	  locales/
//	    *.json          (runtime language packs)
func ExtractAll(destDir string, progressFn func(step, total int, name string)) error {
	// Create target directories
	modelsDir := filepath.Join(destDir, "models")
	if err := os.MkdirAll(modelsDir, 0755); err != nil {
		return fmt.Errorf("create models dir: %w", err)
	}
	localesDir := filepath.Join(destDir, "locales")
	if err := os.MkdirAll(localesDir, 0755); err != nil {
		return fmt.Errorf("create locales dir: %w", err)
	}

	// List embedded files (recursively from "resources" using all: embed)
	entries, err := fs.ReadDir(EmbeddedFS, "resources")
	if err != nil {
		return fmt.Errorf("read embedded resources: %w", err)
	}
	modelEntries, _ := fs.ReadDir(EmbeddedFS, "resources/models")
	localeEntries, _ := fs.ReadDir(EmbeddedFS, "resources/locales")

	total := 0
	for _, group := range [][]fs.DirEntry{entries, modelEntries, localeEntries} {
		for _, entry := range group {
			if !entry.IsDir() {
				total++
			}
		}
	}
	step := 0

	processEntry := func(name, srcPath, dstPath string) error {
		step++
		if progressFn != nil {
			progressFn(step, total, name)
		}
		return ExtractResource(srcPath, dstPath)
	}

	for _, entry := range entries {
		// Skip directories (handled separately)
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		srcPath := "resources/" + name

		// .dat files go to models/ subdirectory (historical convention)
		var dstPath string
		if filepath.Ext(name) == ".dat" {
			dstPath = filepath.Join(modelsDir, name)
		} else {
			dstPath = filepath.Join(destDir, name)
		}

		if err := processEntry(name, srcPath, dstPath); err != nil {
			return fmt.Errorf("extract %s: %w", name, err)
		}
	}

	// Extract model files from resources/models/ to dest/models/
	for _, entry := range modelEntries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		srcPath := "resources/models/" + name
		dstPath := filepath.Join(modelsDir, name)
		if err := processEntry(name, srcPath, dstPath); err != nil {
			return fmt.Errorf("extract %s: %w", name, err)
		}
	}

	// Language packs are deliberately external to FaceLoginConsole.exe so they
	// can be maintained and updated independently. They must therefore be
	// deployed beside the application rather than merely embedded in Setup.
	for _, entry := range localeEntries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		srcPath := "resources/locales/" + name
		dstPath := filepath.Join(localesDir, name)
		if err := processEntry(filepath.Join("locales", name), srcPath, dstPath); err != nil {
			return fmt.Errorf("extract locale %s: %w", name, err)
		}
	}
	return nil
}

// RemoveInstalledFiles deletes exactly the files this installer deployed, in
// the same layout ExtractAll wrote them. It NEVER removes the install directory
// or any user data (data/, log/). Uninstall uses this instead of os.RemoveAll
// so a corrupted/malicious InstallPath registry value can never wipe an
// arbitrary directory. Returns the number of files removed and the first error
// (if any) — callers can continue and report a summary.
//
// removeUserData: when true, additionally deletes the data/ (config.json,
// enrolled face database) and log/ (logs) subdirectories — i.e. a full purge.
// When false, only program files are removed and user data is preserved.
func RemoveInstalledFiles(destDir string, removeUserData bool) (int, error) {
	removed := 0
	var firstErr error
	recordErr := func(err error) {
		if err != nil && firstErr == nil {
			firstErr = err
		}
	}

	modelsDir := filepath.Join(destDir, "models")

	entries, err := fs.ReadDir(EmbeddedFS, "resources")
	if err != nil {
		// If embedded resources can't be enumerated, fall back to the known
		// top-level binary names so uninstall still removes the executables.
		for _, name := range []string{
			"FaceLoginService.exe",
			"FaceLoginCredentialProvider.dll",
			"FaceLoginConsole.exe",
		} {
			p := filepath.Join(destDir, name)
			if FileExists(p) {
				if err := os.Remove(p); err != nil {
					recordErr(err)
				} else {
					removed++
				}
			}
		}
		return removed, firstErr
	}
	modelEntries, _ := fs.ReadDir(EmbeddedFS, "resources/models")
	localeEntries, _ := fs.ReadDir(EmbeddedFS, "resources/locales")

	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		name := entry.Name()
		var dstPath string
		if filepath.Ext(name) == ".dat" {
			dstPath = filepath.Join(modelsDir, name)
		} else {
			dstPath = filepath.Join(destDir, name)
		}
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}
	for _, entry := range modelEntries {
		if entry.IsDir() {
			continue
		}
		dstPath := filepath.Join(modelsDir, entry.Name())
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}
	localesDir := filepath.Join(destDir, "locales")
	for _, entry := range localeEntries {
		if entry.IsDir() {
			continue
		}
		dstPath := filepath.Join(localesDir, entry.Name())
		if FileExists(dstPath) {
			if err := os.Remove(dstPath); err != nil {
				recordErr(err)
			} else {
				removed++
			}
		}
	}
	// Remove only the now-empty directories owned by the installer. Unknown
	// files keep the directory in place and are never deleted implicitly.
	for _, dir := range []string{modelsDir, localesDir} {
		if entries, err := os.ReadDir(dir); err == nil && len(entries) == 0 {
			recordErr(os.Remove(dir))
		}
	}

	// Full purge: also remove user data and logs (config.json, the enrolled
	// face database in data/, and log/). Only invoked when the caller opted in
	// (removeUserData); the default uninstall preserves these.
	if removeUserData {
		for _, sub := range []string{"data", "log"} {
			dir := filepath.Join(destDir, sub)
			if DirExists(dir) {
				if err := os.RemoveAll(dir); err != nil {
					recordErr(err)
				} else {
					removed++
				}
			}
		}
	}

	return removed, firstErr
}

// RemoveInstalledDir removes the install directory itself, but ONLY if it is
// empty after the files above were deleted. This is the anti-misdeletion guard:
// a real FaceLogin install dir that held unexpected/unknown files (not deployed
// by us) will still contain them here, so the directory is left in place and
// false is returned — never silently wiping an arbitrary directory. Returns
// true when the directory was removed.
func RemoveInstalledDir(destDir string) (bool, error) {
	if !DirExists(destDir) {
		return true, nil // already gone
	}
	entries, err := os.ReadDir(destDir)
	if err != nil {
		return false, err
	}
	// Also tolerate installer-owned subdirectories being left empty — but only
	// if they contain nothing. Anything else means the dir is NOT empty.
	if len(entries) == 0 {
		if err := os.Remove(destDir); err != nil {
			return false, err
		}
		return true, nil
	}
	// Remove any empty installer-owned directories, then retry once. Unknown
	// directories or files are intentionally preserved.
	for _, entry := range entries {
		if !entry.IsDir() || (!strings.EqualFold(entry.Name(), "models") && !strings.EqualFold(entry.Name(), "locales")) {
			continue
		}
		dir := filepath.Join(destDir, entry.Name())
		subEntries, readErr := os.ReadDir(dir)
		if readErr == nil && len(subEntries) == 0 {
			if err := os.Remove(dir); err != nil {
				return false, err
			}
		}
	}
	remaining, err := os.ReadDir(destDir)
	if err != nil {
		return false, err
	}
	if len(remaining) == 0 {
		if err := os.Remove(destDir); err != nil {
			return false, err
		}
		return true, nil
	}
	return false, nil // not empty — do NOT delete
}
