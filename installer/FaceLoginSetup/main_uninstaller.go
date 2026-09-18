//go:build uninstaller

package main

import (
	"fmt"
	"os"

	"FaceLoginSetup/internal"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
)

// This entry point is built separately with -tags uninstaller. It keeps the
// same Wails UI and uninstall bindings, but does not embed resources/ (models,
// services, runtime DLLs, or the Console executable).
func main() {
	if internal.RunUninstallCleanup(os.Args[1:]) {
		return
	}

	if !internal.IsAdmin() {
		if err := internal.Elevate(); err != nil {
			fmt.Fprintf(os.Stderr, "Elevation failed: %v\n", err)
		}
		os.Exit(0)
	}

	installEmbeddedResources()
	app := NewApp(true)

	err := wails.Run(&options.App{
		Title:            "FaceLogin Setup",
		Width:            640,
		Height:           520,
		DisableResize:    true,
		WindowStartState: options.Normal,
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		BackgroundColour: &options.RGBA{R: 255, G: 255, B: 255, A: 1},
		OnStartup:        app.startup,
		Bind: []interface{}{
			app,
		},
	})
	if err != nil {
		println("Error:", err.Error())
	}
}
