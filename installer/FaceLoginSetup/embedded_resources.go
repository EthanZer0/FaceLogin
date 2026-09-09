//go:build !uninstaller

package main

import (
	"embed"

	"FaceLoginSetup/internal"
)

//go:embed all:resources
var resources embed.FS

func installEmbeddedResources() {
	internal.EmbeddedFS = resources
}
