#pragma once

#include <QWidget>

#include <string>

struct obs_display;
typedef struct obs_display obs_display_t;
struct obs_source;
typedef struct obs_source obs_source_t;

namespace harpia {

// A small live camera preview rendered with a libobs display bound to this
// widget's native window — the same mechanism OBS uses for its previews. It owns
// the webcam obs_source so the toolbar can show the feed before/while recording;
// the source can be shared with WebcamRecorder so the device is opened once.
//
// Rendering happens on the libobs graphics thread via a draw callback; the
// widget itself never paints (paintEngine() returns null, paintEvent is empty).
class WebcamPreview : public QWidget {
	Q_OBJECT
public:
	explicit WebcamPreview(QWidget *parent = nullptr);
	~WebcamPreview() override;

	// (Re)create the camera source for the given device/size/fps. An empty
	// deviceId uses the first available camera.
	void setDevice(const std::string &deviceId, int width, int height, int fps);

	// Destroy the camera source, releasing the physical device.
	void clearDevice();

	// The live camera source (may be null), so a recorder can reuse it.
	obs_source_t *source() const { return source_; }
	bool hasSource() const { return source_ != nullptr; }

	// Native-render surface: Qt must not paint over the libobs display.
	QPaintEngine *paintEngine() const override { return nullptr; }

protected:
	void showEvent(QShowEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override; // no-op

private:
	void ensureDisplay();
	void destroyDisplay();
	static void drawPreview(void *data, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
	obs_source_t *source_ = nullptr;

	// Remembered device config so the source survives show/hide cycles.
	std::string deviceId_;
	int width_ = 1280;
	int height_ = 720;
	int fps_ = 30;
};

} // namespace harpia
