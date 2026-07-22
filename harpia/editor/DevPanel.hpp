#pragma once

#include <QDialog>

#include <functional>

class QDoubleSpinBox;
class QSpinBox;

namespace harpia {

// Window-level "chrome" tweaks for the editor toolbar/transport — not part of
// any timeline layout struct. The editor reads these at startup and follows
// chromeChanged() live from the Developer Panel.
struct EditorChromeParams {
	int buttonH = 28;         // toolbar/transport button height
	int timecodeFontPx = 18;  // the big monospace playhead timecode
	int inspectorFontPx = 13; // the inspector's value readouts
	int speedSliderMinW = 140; // speed slider minimum width
	int speedSpinW = 72;       // editable speed-value box width
};

class PreviewCanvas;
class Timeline;
class TrackEditor;
class VoiceoverTrack;
struct PreviewLayoutParams;
struct TimelineLayoutParams;
struct TrackLayoutParams;
struct VoiceoverLayoutParams;

// Developer Panel for the video editor: exposes every timeline layout variable
// (spacing, thumbnail/track sizes, padding, zoom limits…) as live spin boxes,
// so the best UI configuration can be found by tweaking the running app. Every
// change is auto-saved (QSettings) and restored on the next launch.
class DevPanel : public QDialog {
	Q_OBJECT
public:
	DevPanel(Timeline *timeline, TrackEditor *tracks, VoiceoverTrack *voice,
		 PreviewCanvas *preview, QWidget *parent = nullptr);

	// Persisted tweaks: the editor calls loadInto() at startup to restore the
	// saved layout; the panel calls saveFrom() after every change.
	static void loadInto(TimelineLayoutParams &tl, TrackLayoutParams &tr, VoiceoverLayoutParams &vo,
			     PreviewLayoutParams &pv);
	static void saveFrom(const TimelineLayoutParams &tl, const TrackLayoutParams &tr,
			     const VoiceoverLayoutParams &vo, const PreviewLayoutParams &pv);

	// Window-chrome tweaks (button height, text sizes, speed-slider widths):
	// the editor reads these at startup and follows chromeChanged() live.
	static EditorChromeParams loadChrome();
	static void saveChrome(const EditorChromeParams &p);

signals:
	void chromeChanged(const EditorChromeParams &p);

private:
	void applyTimeline();
	void applyTracks();
	void applyVoice();
	void applyPreview();
	void applyChrome();
	void resetDefaults();

	Timeline *timeline_ = nullptr;
	TrackEditor *tracks_ = nullptr;
	VoiceoverTrack *voice_ = nullptr;
	PreviewCanvas *preview_ = nullptr;

	// Editor window (toolbar/transport chrome).
	QSpinBox *winBtnH_ = nullptr;
	QSpinBox *winTcFont_ = nullptr;
	QSpinBox *winInsFont_ = nullptr;
	QSpinBox *winSpeedW_ = nullptr;
	QSpinBox *winSpinW_ = nullptr;

	// Simple Trim timeline.
	QSpinBox *tlPad_ = nullptr;
	QSpinBox *tlBarTop_ = nullptr;
	QSpinBox *tlBarH_ = nullptr;
	QSpinBox *tlHandleW_ = nullptr;
	QSpinBox *tlTileGap_ = nullptr;
	QSpinBox *tlFontPx_ = nullptr;
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
	QSpinBox *trCaptionFontPx_ = nullptr;
	QSpinBox *trSegFontPx_ = nullptr;
	QDoubleSpinBox *trMaxZoom_ = nullptr;

	// Preview canvas (shared by both modes).
	QSpinBox *pvW_ = nullptr;
	QSpinBox *pvH_ = nullptr;

	// Voiceover track.
	QSpinBox *voMargin_ = nullptr;
	QSpinBox *voCaptionH_ = nullptr;
	QSpinBox *voTrackH_ = nullptr;
	QSpinBox *voMinClipW_ = nullptr;
	QSpinBox *voEdgeZone_ = nullptr;

	bool loading_ = false; // guard: setting spin values must not re-apply
};

} // namespace harpia
