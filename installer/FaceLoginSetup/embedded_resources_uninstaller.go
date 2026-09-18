//go:build uninstaller

package main

import "FaceLoginSetup/internal"

func installEmbeddedResources() {
	// The standalone uninstaller has no installation payload. The uninstall
	// implementation uses its compact known-file manifest instead.
	internal.EmbeddedFS = nil
}
