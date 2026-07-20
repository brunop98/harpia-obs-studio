#pragma once

#include <QImage>
#include <QRect>
#include <QWidget>

namespace harpia {

// Shows the current video frame scaled-to-fit, and (when crop is enabled) an
// interactive crop rectangle with 8 resize handles that maps to source-video
// pixels. Emits cropChanged() in source pixels.
class PreviewCanvas : public QWidget {
	Q_OBJECT
public:
	explicit PreviewCanvas(QWidget *parent = nullptr);

	void setVideoSize(int w, int h); // source dimensions (px)
	void setFrame(const QImage &img);

	void setCropEnabled(bool on);
	bool cropEnabled() const { return cropEnabled_; }
	QRect cropRectVideo() const { return cropVideo_; }
	void resetCrop(); // full frame

signals:
	void cropChanged(const QRect &videoRect);

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

private:
	enum class Zone { None, Move, L, R, T, B, TL, TR, BL, BR };
	QRect displayRect() const;                  // where the frame is painted
	QRect videoToWidget(const QRect &r) const;  // source px -> widget px
	QRect widgetCropRect() const;               // crop in widget px
	Zone zoneAt(const QPoint &p) const;
	void applyWidgetCrop(const QRect &widgetRect); // clamp + map back to video px

	QImage frame_;
	int vw_ = 0, vh_ = 0;
	bool cropEnabled_ = false;
	QRect cropVideo_; // source pixels

	Zone drag_ = Zone::None;
	QPoint dragStart_;
	QRect dragStartCrop_; // widget px at press
};

// A trim timeline with start/end handles and a playhead. Dragging a handle
// emits scrub() with the ms under it (for live preview) plus startChanged()/
// endChanged(); clicking the bar moves the playhead and scrubs.
class Timeline : public QWidget {
	Q_OBJECT
public:
	explicit Timeline(QWidget *parent = nullptr);

	void setDuration(qint64 ms);
	void setStart(qint64 ms);
	void setEnd(qint64 ms);
	qint64 start() const { return start_; }
	qint64 end() const { return end_; }

signals:
	void startChanged(qint64 ms);
	void endChanged(qint64 ms);
	void scrub(qint64 ms); // frame to preview while interacting

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

private:
	int msToX(qint64 ms) const;
	qint64 xToMs(int x) const;

	enum class Grab { None, Start, End, Playhead };
	Grab grab_ = Grab::None;

	qint64 duration_ = 0;
	qint64 start_ = 0;
	qint64 end_ = 0;
	qint64 playhead_ = 0;
};

} // namespace harpia
