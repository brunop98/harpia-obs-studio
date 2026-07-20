#pragma once

#include <string>
#include <vector>

namespace harpia {

// A named, reusable capture region (screen device pixels). Position, size and
// the display it was drawn on are stored so a saved region can be restored
// instantly — including switching to its monitor — from the capture dropdown.
struct SavedRegion {
	std::string id;
	std::string name;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	int monitorIndex = 0;
};

// Persists the user's saved capture regions as JSON in the app config dir.
// Global (not per-preset) — the capture region is a global tool in Harpia.
class RegionStore {
public:
	explicit RegionStore(std::string configDir);

	void load();
	bool save() const;

	const std::vector<SavedRegion> &regions() const { return regions_; }

	// Add a new region or replace the existing one with the same id.
	void upsert(const SavedRegion &r);
	void remove(const std::string &id);
	const SavedRegion *find(const std::string &id) const;

private:
	std::string filePath() const;

	std::string configDir_;
	std::vector<SavedRegion> regions_;
};

} // namespace harpia
