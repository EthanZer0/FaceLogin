package internal

import (
	"encoding/json"
	"fmt"
	"os"
)

// ConfigUpgradeEnabled is the version-scoped switch for running config
// "upgrade actions" during install.
//
// A config upgrade action force-syncs specific keys to new defaults. It is
// intended for ONE release: when the app's default parameters change, the
// release that ships that change sets ConfigUpgradeEnabled = true and lists
// the affected keys in ConfigUpgradeForcedDefaults. Later releases leave the
// switch OFF so their installs never re-apply (and never clobber a value the
// user adjusted back after the fact).
//
// The switch defaults to OFF. Each release that needs an upgrade action must
// explicitly turn it on — this is the "custom action area" that can be
// enabled/disabled per version.
var ConfigUpgradeEnabled = false

// ConfigUpgradeForcedDefaults lists the keys force-synced to these values
// when ConfigUpgradeEnabled is true. Keys NOT listed here are preserved
// verbatim on upgrade.
//
// v1.8.0: match_threshold 0.65 → 0.75 (the 512-D model's same-person range
// measures 0.14–0.80; 0.65 sat too close to the high end of real-user matches
// under illumination changes, causing intermittent unlock failures — 0.75
// keeps strangers >0.94 comfortably rejected). Existing installs must be
// force-synced; new installs get it from the fresh-default below.
var ConfigUpgradeEnabled = true
var ConfigUpgradeForcedDefaults = map[string]any{
	"match_threshold": 0.75,
}

// EnsureConfigDefaults ensures config.json exists and — when the per-release
// upgrade switch is enabled — force-syncs the declared keys to their new
// defaults while preserving every other key.
//
// If the file is missing, a fresh default config is written so the app has a
// complete, valid file to load. If the upgrade switch is off, an existing
// config is left completely untouched.
func EnsureConfigDefaults(configPath string) error {
	var cfg map[string]any

	data, err := os.ReadFile(configPath)
	if err == nil {
		if jerr := json.Unmarshal(data, &cfg); jerr != nil || cfg == nil {
			cfg = map[string]any{}
		}
	} else {
		// No config yet — start from a full default so the app loads sane
		// values on first run (the app's own defaults mirror these).
		cfg = map[string]any{
			"recognition_model":       "onnx",
			"detector":                "scrfd",
			"liveness_method":         "none",
			"match_threshold":         0.75,
			"anti_spoof_threshold":    0.30,
			"blink_glasses_mode":      false,
			"low_light_enhance":       false,
			"unload_models_after_auth": false,
			"camera_rotation":         0,
			"capture_unknown_faces":   false,
			"cold_boot_key_trigger":   false,
		}
	}

	// Apply per-release forced defaults ONLY when this release opted in.
	if ConfigUpgradeEnabled {
		for k, v := range ConfigUpgradeForcedDefaults {
			cfg[k] = v
		}
	}

	// Ensure the JSON is written even if we didn't touch anything.
	out, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal config: %w", err)
	}
	if err := os.WriteFile(configPath, out, 0644); err != nil {
		return fmt.Errorf("write config %s: %w", configPath, err)
	}
	return nil
}

