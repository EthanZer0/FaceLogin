package main

import "embed"

// The frontend is shared by the full installer and the lightweight
// standalone uninstaller. Only the full installer embeds resources/.
//
//go:embed all:frontend/dist
var assets embed.FS
