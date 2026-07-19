#include "WebcamPreview.hpp"

#include "core/WebcamRecorder.hpp"

#include <obs.h>

#include <algorithm>
#include <string>

namespace harpia {

namespace {
// Look up the device-id property key the platform camera source exposes.
const char *cameraDeviceKey(obs_properties_t *props)
{
	for (const char *k : {"video_device_id", "device_id", "device"}) {
		if (obs_properties_get(props, k))
			return k;
	}
	return nullptr;
}
} // namespace

WebcamPreview::WebcamPreview(QWidget *parent) : QWidget(parent)
{
	// Turn this widget into a native surface libobs can render into directly.
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setMinimumSize(120, 68);
}

WebcamPreview::~WebcamPreview()
{
	destroyDisplay();
	if (source_) {
		obs_source_release(source_);
		source_ = nullptr;
	}
}

void WebcamPreview::paintEvent(QPaintEvent *)
{
	// Intentionally empty — the libobs display owns this surface.
}

void WebcamPreview::setDevice(const std::string &deviceId, int width, int height, int fps)
{
	deviceId_ = deviceId;
	width_ = width >= 16 ? width : 1280;
	height_ = height >= 16 ? height : 720;
	fps_ = fps >= 1 ? fps : 30;

	// Resolve an empty selection to the first available camera.
	std::string dev = deviceId_;
	if (dev.empty()) {
		const auto cams = WebcamRecorder::cameras();
		if (!cams.empty())
			dev = cams.front().id;
	}

	const char *camId = WebcamRecorder::platformCameraId();
	obs_data_t *cs = obs_data_create();
	{
		obs_properties_t *props = obs_get_source_properties(camId);
		const char *key = props ? cameraDeviceKey(props) : nullptr;
		if (key && !dev.empty())
			obs_data_set_string(cs, key, dev.c_str());
		if (props)
			obs_properties_destroy(props);
	}
	obs_data_set_int(cs, "res_type", 1);
	obs_data_set_string(cs, "resolution",
			    (std::to_string(width_) + "x" + std::to_string(height_)).c_str());
	obs_data_set_int(cs, "frame_interval", 10000000LL / fps_);

	obs_source_t *fresh = obs_source_create(camId, "harpia_webcam_preview", cs, nullptr);
	obs_data_release(cs);

	// Swap the source under the graphics lock so the draw callback never reads a
	// source mid-destruction, then release the old one outside the lock.
	obs_source_t *old = nullptr;
	obs_enter_graphics();
	old = source_;
	source_ = fresh;
	obs_leave_graphics();
	if (old)
		obs_source_release(old);

	ensureDisplay();
}

void WebcamPreview::clearDevice()
{
	obs_source_t *old = nullptr;
	obs_enter_graphics();
	old = source_;
	source_ = nullptr;
	obs_leave_graphics();
	if (old)
		obs_source_release(old);
	update();
}

void WebcamPreview::showEvent(QShowEvent *)
{
	ensureDisplay();
}

void WebcamPreview::resizeEvent(QResizeEvent *)
{
	if (display_)
		obs_display_resize(display_, (uint32_t)(width() * devicePixelRatioF()),
				   (uint32_t)(height() * devicePixelRatioF()));
}

void WebcamPreview::ensureDisplay()
{
	if (display_ || !isVisible() || width() <= 0 || height() <= 0)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)(width() * devicePixelRatioF());
	info.cy = (uint32_t)(height() * devicePixelRatioF());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;

#if defined(_WIN32)
	info.window.hwnd = (void *)winId();
#elif defined(__APPLE__)
	info.window.view = (id)(uintptr_t)winId();
#else
	// X11/Wayland preview would need the native display handle; not wired here.
	return;
#endif

	display_ = obs_display_create(&info, 0x000000);
	if (display_)
		obs_display_add_draw_callback(display_, &WebcamPreview::drawPreview, this);
}

void WebcamPreview::destroyDisplay()
{
	if (display_) {
		obs_display_remove_draw_callback(display_, &WebcamPreview::drawPreview, this);
		obs_display_destroy(display_);
		display_ = nullptr;
	}
}

void WebcamPreview::drawPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<WebcamPreview *>(data);
	obs_source_t *src = self->source_;
	if (!src)
		return;

	const int sw = (int)obs_source_get_width(src);
	const int sh = (int)obs_source_get_height(src);
	if (sw <= 0 || sh <= 0)
		return;

	// Letterbox-fit the source into the display, preserving aspect ratio.
	const float scale = std::min((float)cx / (float)sw, (float)cy / (float)sh);
	const int dw = (int)((float)sw * scale);
	const int dh = (int)((float)sh * scale);
	const int dx = ((int)cx - dw) / 2;
	const int dy = ((int)cy - dh) / 2;

	gs_viewport_push();
	gs_projection_push();
	gs_set_viewport(dx, dy, dw, dh);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);

	obs_source_video_render(src);

	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace harpia
