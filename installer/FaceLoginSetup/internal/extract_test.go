package internal

import (
	"os"
	"path/filepath"
	"testing"
	"testing/fstest"
)

func TestExtractAllDeploysRuntimeLocales(t *testing.T) {
	previousFS := EmbeddedFS
	t.Cleanup(func() { EmbeddedFS = previousFS })

	EmbeddedFS = fstest.MapFS{
		"resources/FaceLoginConsole.exe": &fstest.MapFile{Data: []byte("console")},
		"resources/models/model.onnx":    &fstest.MapFile{Data: []byte("model")},
		"resources/locales/ko-KR.json":   &fstest.MapFile{Data: []byte(`{"meta.locale":"ko-KR"}`)},
		"resources/locales/zh-CN.json":   &fstest.MapFile{Data: []byte(`{"meta.locale":"zh-CN"}`)},
	}

	destination := t.TempDir()
	var progressSteps int
	if err := ExtractAll(destination, func(step, total int, name string) {
		progressSteps = step
		if total != 4 {
			t.Fatalf("unexpected progress total: got %d, want 4", total)
		}
	}); err != nil {
		t.Fatalf("ExtractAll: %v", err)
	}
	if progressSteps != 4 {
		t.Fatalf("unexpected final progress step: got %d, want 4", progressSteps)
	}

	for _, relativePath := range []string{
		"FaceLoginConsole.exe",
		filepath.Join("models", "model.onnx"),
		filepath.Join("locales", "ko-KR.json"),
		filepath.Join("locales", "zh-CN.json"),
	} {
		if _, err := os.Stat(filepath.Join(destination, relativePath)); err != nil {
			t.Errorf("expected extracted file %s: %v", relativePath, err)
		}
	}
}

func TestRemoveInstalledFilesRemovesRuntimeLocales(t *testing.T) {
	previousFS := EmbeddedFS
	t.Cleanup(func() { EmbeddedFS = previousFS })

	EmbeddedFS = fstest.MapFS{
		"resources/FaceLoginConsole.exe": &fstest.MapFile{Data: []byte("console")},
		"resources/locales/ko-KR.json":   &fstest.MapFile{Data: []byte("ko")},
	}

	destination := t.TempDir()
	if err := ExtractAll(destination, nil); err != nil {
		t.Fatalf("ExtractAll: %v", err)
	}
	if _, err := RemoveInstalledFiles(destination, false); err != nil {
		t.Fatalf("RemoveInstalledFiles: %v", err)
	}
	if _, err := os.Stat(filepath.Join(destination, "locales", "ko-KR.json")); !os.IsNotExist(err) {
		t.Fatalf("locale pack still exists after removal: %v", err)
	}
}
