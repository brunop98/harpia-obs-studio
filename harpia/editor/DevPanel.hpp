#pragma once

#include <QDialog>

#include <functional>

class QDoubleSpinBox;
class QSpinBox;

namespace harpia {

class Timeline;
class TrackEditor;
struct TimelineLayoutParams;
struct TrackLayoutParams;

// Developer Panel for the video editor: exposes every timeline layout variable
// (spacing, thumbnail/track sizes, padding, zoom limits…) as live spin boxes,
// so the best UI configuration can be found by tweaking the running app. Every
// change is auto-saved (QSettings) and restored on the next launch.
class DevPanel : public QDialog {
	Q_OBJECT
public:
	DevPanel(Timeline *timeline, TrackEditor *tracks, QWidget *parent = nullptr);

	// Persisted tweaks: the editor calls loadInto() at startup to restore the
	// saved layout; the panel calls saveFrom() after every change.
	static void loadInto(TimelineLayoutParams &tl, TrackLayoutParams &tr);
	static void saveFrom(const TimelineLayoutParams &tl, const TrackLayoutParams &tr);

private:
	void applyTimeline();
	void applyTracks();
	void resetDefaults();

	Timeline *timeline_ = nullptr;
	TrackEditor *tracks_ = nullptr;

	// Simple Trim timeline.
	QSpinBox *tlPad_ = nullptr;
	QSpinBox *tlBarTop_ = nullptr;
	QSpinBox *tlBarH_ = nullptr;
	QSpinBox *tlHandleW_ = nullptr;
	QSpinBox *tlTileGap_ = nullptr;
	QDoubleSpinBox *tlMaxZoom_ = nullptr;

	// Multi-Cut track editor.
	QSpinBox *trMargin_ = nullptr;
	QSpinBox *trCaptionH_ = nullptr;
	QSpinBox *trSrcH_ = nullptr;
	QSpinBox *trTrackGap_ = nullptr;
	QSpinBox *trOutH_ = nullptr;
	QSpinBox *trSegGap_ = nullptr;
	QSpinBox *trMinSegW_ = nullptr;
	QSpinBox *trHardMinSegW_ = nullptr;
	QSpinBox *trTileGap_ = nullptr;
	QDoubleSpinBox *trMaxZoom_ = nullptr;

	bool loading_ = false; // guard: setting spin values must not re-apply
};

} // namespace harpia
