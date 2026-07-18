#pragma once

#include "Preset.hpp"

#include <string>
#include <vector>

namespace harpia {

// Owns the collection of recording presets and persists them as JSON in the
// application's config directory (%APPDATA%/harpia-recorder on Windows,
// ~/.config/harpia-recorder on Linux). Persistence uses libobs' obs_data so we
// pull in no extra JSON dependency.
class PresetStore {
public:
	// `configDir` is the directory where presets.json lives. If empty, the
	// platform config path is resolved automatically.
	explicit PresetStore(std::string configDir = {});

	// Load presets from disk. If none exist, seeds a single default preset
	// (writing recordings to `defaultOutputFolder`) and persists it.
	void load(const std::string &defaultOutputFolder);

	bool save() const;

	const std::vector<Preset> &presets() const { return presets_; }

	// Returns a pointer to the preset with `id`, or nullptr.
	const Preset *find(const std::string &id) const;

	// Insert or replace by id, then persist.
	void upsert(const Preset &preset);

	// Remove by id, then persist. Never removes the last remaining preset.
	void remove(const std::string &id);

	const std::string &configDir() const { return configDir_; }

private:
	std::string filePath() const;

	std::string configDir_;
	std::vector<Preset> presets_;
};

} // namespace harpia
