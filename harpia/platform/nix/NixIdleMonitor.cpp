#include "platform/IdleMonitor.hpp"

#include <dlfcn.h>

namespace harpia {

namespace {

// System-wide idle time on X11 via the XScreenSaver extension. We load X11 and
// Xss at runtime with dlopen so the app needs no X development packages at build
// time and degrades gracefully (returns 0 → idle auto-pause disabled) on
// headless or pure-Wayland sessions.
//
// XScreenSaverInfo layout (from libXScrnSaver): we only need the `idle` field,
// which is the 4th member (Window window; int state; int kind; unsigned long
// til_or_since; unsigned long idle; ...). We mirror the leading fields so the
// offset of `idle` is correct.
struct XScreenSaverInfoLite {
	unsigned long window;      // Window (XID)
	int state;
	int kind;
	unsigned long til_or_since;
	unsigned long idle;        // milliseconds since last input
	unsigned long event_mask;
};

class NixIdleMonitor : public IdleMonitor {
public:
	NixIdleMonitor()
	{
		x11_ = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
		xss_ = dlopen("libXss.so.1", RTLD_NOW | RTLD_GLOBAL);
		if (!x11_ || !xss_)
			return;

		openDisplay_ = (OpenDisplayFn)dlsym(x11_, "XOpenDisplay");
		defaultRootWindow_ = (DefaultRootWindowFn)dlsym(x11_, "XDefaultRootWindow");
		allocInfo_ = (AllocInfoFn)dlsym(xss_, "XScreenSaverAllocInfo");
		queryInfo_ = (QueryInfoFn)dlsym(xss_, "XScreenSaverQueryInfo");

		if (!openDisplay_ || !defaultRootWindow_ || !allocInfo_ || !queryInfo_)
			return;

		display_ = openDisplay_(nullptr);
		if (display_) {
			root_ = defaultRootWindow_(display_);
			info_ = allocInfo_();
		}
	}

	~NixIdleMonitor() override
	{
		// Intentionally leak the display/info at shutdown rather than risk
		// unloading X while it's in use; the process is exiting anyway.
	}

	double currentIdleSeconds() const override
	{
		if (!display_ || !info_ || !queryInfo_)
			return 0.0;
		if (queryInfo_(display_, root_, info_) == 0)
			return 0.0;
		return (double)info_->idle / 1000.0;
	}

private:
	using OpenDisplayFn = void *(*)(const char *);
	using DefaultRootWindowFn = unsigned long (*)(void *);
	using AllocInfoFn = XScreenSaverInfoLite *(*)();
	using QueryInfoFn = int (*)(void *, unsigned long, XScreenSaverInfoLite *);

	void *x11_ = nullptr;
	void *xss_ = nullptr;
	OpenDisplayFn openDisplay_ = nullptr;
	DefaultRootWindowFn defaultRootWindow_ = nullptr;
	AllocInfoFn allocInfo_ = nullptr;
	QueryInfoFn queryInfo_ = nullptr;

	void *display_ = nullptr;
	unsigned long root_ = 0;
	XScreenSaverInfoLite *info_ = nullptr;
};

} // namespace

std::unique_ptr<IdleMonitor> IdleMonitor::create()
{
	return std::make_unique<NixIdleMonitor>();
}

} // namespace harpia
