#pragma once

#include <functional>
#include <memory>

namespace harpia {

struct MacHotkey;

// Carbon registers a shortcut without intercepting arbitrary keyboard events
// or requiring Accessibility access. The caller keeps its Qt fallback when
// registration fails, including keys already reserved by the system or an app.
class MacGlobalHotkeys {
public:
	MacGlobalHotkeys(std::function<void(int)> triggered, std::function<void()> layoutChanged);
	~MacGlobalHotkeys();
	bool bind(int id, const MacHotkey &hotkey);
	void unbindAll();

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

} // namespace harpia
