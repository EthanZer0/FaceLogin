package internal

import (
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
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
func ExtractAll(destDir string, progressFn func(step, total int, name string)) error {
	// Create target directories
	modelsDir := filepath.Join(destDir, "models")
	if err := os.MkdirAll(modelsDir, 0755); err != nil {
		return fmt.Errorf("create models dir: %w", err)
	}

	// List embedded files (recursively from "resources" using all: embed)
	entries, err := fs.ReadDir(EmbeddedFS, "resources")
	if err != nil {
		return fmt.Errorf("read embedded resources: %w", err)
	}
	modelEntries, _ := fs.ReadDir(EmbeddedFS, "resources/models")

	total := len(entries) + len(modelEntries)
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
	return nil
}

// RemoveInstalledFiles deletes exactly the files this installer deployed, in
// the same layout ExtractAll wrote them. It NEVER removes the install directory
// or any user data (data/, log/). Uninstall uses this instead of os.RemoveAll
// so a corrupted/malicious InstallPath registry value can never wipe an
// arbitrary directory. Returns the number of files removed and the first error
// (if any) — callers can continue and report a summary.
func RemoveInstalledFiles(destDir string) (int, error) {
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
	return removed, firstErr
}
