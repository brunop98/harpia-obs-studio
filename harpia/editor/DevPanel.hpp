#pragma once

#include "EditorColors.hpp"

#include <QColor>
#include <QDialog>
#include <QString>
#include <QVector>

#include "timeline/KeyframeEditor.hpp" // KeyframeLayoutParams
#include "../ui/UiText.hpp"

#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace harpia {

// Window-level "chrome" tweaks for the editor toolbar/transport — not part of
// any timeline layout struct. The editor reads these at startup and follows
// chromeChanged() live from the Developer Panel.
struct EditorChromeParams {
	int buttonH = 28;         // toolbar/transport button height
	// Derived from the app's base font rather than fixed, so these follow the
	// global text scale; the Dev panel still overrides them per-install.
	int timecodeFontPx = uiTimecodePx(); // the big monospace playhead timecode
	int inspectorFontPx = uiReadoutPx(); // the inspector's value readouts
	int speedSliderMinW = 96; // speed slider minimum width (drives the toolbar's
				  // own minimum, so it caps how narrow the window can get)
	int speedSpinW = 72;       // editable speed-value box width
	// How much Up / Down (Multi-Cut, with cuts selected) changes a cut's speed
	// per press. Shift+Up / Shift+Down step by five of these.
	double speedStep = 0.2;
	// Pause preview playback while the app is in the background. Playback is the
	// only thing in the editor that runs continuously on its own, so this is
	// what "idle in the background" costs.
	bool powerSaveOnBlur = true;
};

// Sizing for the right-hand Inspector panel. Like the chrome params, the editor
// reads these at startup and follows inspectorChanged() live.
struct EditorInspectorParams {
	int minWidth = 250;    // narrowest the splitter will let it get
	int openWidth = 320;   // width it opens at from the toolbar button
	int margin = 10;       // content margin, left/right
	int spacing = 6;       // gap between sections
	int labelSpacing = 10; // form layouts: label -> control
	int rowSpacing = 4;    // form layouts: row -> row
	int scriptListH = 112; // height of the transform-script stack list
};

class PreviewCanvas;
class Timeline;
class TrackEditor;
class TimelineView;
class VoiceoverTrack;
struct PreviewLayoutParams;
struct TimelineLayoutParams;
struct TimelineViewParams;
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
		 PreviewCanvas *preview, TimelineView *fullTimeline = nullptr,
		 QWidget *parent = nullptr);

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
	static EditorInspectorParams loadInspector();
	static void saveInspector(const EditorInspectorParams &p);

	// Full-editing timeline layout. Kept as its own pair (like the chrome above)
	// so the older loadInto/saveFrom signature stays untouched.
	static TimelineViewParams loadFullTimeline();
	static void saveFullTimeline(const TimelineViewParams &p);

	// Keyframe-lane geometry. Signal-based rather than widget-based like the
	// group above, because the keyframe editor is a dialog that only exists
	// while it is open -- the window keeps the values and applies them to the
	// editor whenever it is opened.
	static KeyframeLayoutParams loadKeyframe();
	static void saveKeyframe(const KeyframeLayoutParams &p);

	// The editor palette, shared by all three track widgets. Same deal: the
	// editor loads it at startup and follows colorsChanged() live.
	static EditorColors loadColors();
	static void saveColors(const EditorColors &c);

signals:
	void chromeChanged(const EditorChromeParams &p);
	void inspectorChanged(const EditorInspectorParams &p);
	void keyframeChanged(const KeyframeLayoutParams &p);

private:
	void applyTimeline();
	void applyTracks();
	void applyVoice();
	void applyPreview();
	void applyChrome();
	void applyFullTimeline();
	void applyKeyframe();
	void applyColors();

public:
	// The live half of a colour pick, public so the behaviour is testable
	// without driving a modal QColorDialog:
	//   * previewColor pushes a candidate to every track widget IMMEDIATELY,
	//     without saving -- this runs on every move inside the picker, and a
	//     QSettings write per mouse-move would be disk churn for nothing;
	//   * finishColorPick settles the row: the picked colour when accepted,
	//     the ORIGINAL when cancelled -- cancelling a live preview must mean
	//     "as if I never opened the dialog" -- then saves once.
	void previewColor(int rowIdx, const QColor &candidate);
	void finishColorPick(int rowIdx, const QColor &original, bool accepted, const QColor &picked);

private:
	// "Reset to defaults" used to mean ALL of them, from any tab -- so touching
	// one number on the Colors page and wanting it back cost you every other
	// tweak in the panel. Each section resets on its own now; resetDefaults()
	// is what the explicit "all tabs" button calls.
	void resetDefaults();
	void resetCurrentTab();
	void resetWindowTab();
	void resetTrimTab();
	void resetPreviewTab();
	void resetInspectorTab();
	void resetMultiCutTab();
	void resetKeyframeTab();
	void resetFullEditTab();
	void resetVoiceoverTab();
	void resetColorsTab();

	// One swatch button per palette entry, keyed by the same name used in
	// QSettings, so adding a colour means touching one table and nothing else.
	struct ColorRow {
		QString key;
		QPushButton *btn = nullptr;
		QColor EditorColors::*field = nullptr;
	};
	QVector<ColorRow> colorRows_;
	EditorColors colors_;
	void paintSwatch(const ColorRow &r) const; // button face = the colour itself

	Timeline *timeline_ = nullptr;
	TrackEditor *tracks_ = nullptr;
	VoiceoverTrack *voice_ = nullptr;
	PreviewCanvas *preview_ = nullptr;
	TimelineView *fullTimeline_ = nullptr;

	// Editor window (toolbar/transport chrome).
	QSpinBox *winBtnH_ = nullptr;
	QSpinBox *winTcFont_ = nullptr;
	QSpinBox *winInsFont_ = nullptr;
	QSpinBox *winSpeedW_ = nullptr;
	QSpinBox *winSpinW_ = nullptr;
	QDoubleSpinBox *winSpeedStep_ = nullptr;
	QCheckBox *winPowerSave_ = nullptr;
	QSpinBox *insMinW_ = nullptr;
	QSpinBox *insOpenW_ = nullptr;
	QSpinBox *insMargin_ = nullptr;
	QSpinBox *insSpacing_ = nullptr;
	QSpinBox *insLabelSp_ = nullptr;
	QSpinBox *insRowSp_ = nullptr;
	QSpinBox *insScriptH_ = nullptr;
	void applyInspector();

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

	// Full-editing multi-track timeline.
	QSpinBox *ftGutterW_ = nullptr;
	QSpinBox *ftRulerH_ = nullptr;
	QSpinBox *ftVideoLaneH_ = nullptr;
	QSpinBox *ftAudioLaneH_ = nullptr;
	QSpinBox *ftLaneGap_ = nullptr;
	QSpinBox *ftMargin_ = nullptr;
	QSpinBox *ftMinClipW_ = nullptr;
	QSpinBox *ftClipGap_ = nullptr;
	QSpinBox *ftClipRadius_ = nullptr;
	QSpinBox *ftSplitSeamW_ = nullptr;
	QSpinBox *ftSnapPx_ = nullptr;
	QSpinBox *ftDropBandPx_ = nullptr;
	QSpinBox *ftSegFontPx_ = nullptr;
	QDoubleSpinBox *ftMaxZoom_ = nullptr;

	// Keyframe lanes.
	QSpinBox *kfMargin_ = nullptr;
	QSpinBox *kfRulerH_ = nullptr;
	QSpinBox *kfGrab_ = nullptr;
	QSpinBox *kfDiamond_ = nullptr;
	QSpinBox *kfLaneMinH_ = nullptr;
	QDoubleSpinBox *kfMaxZoom_ = nullptr;

	// Voiceover track.
	QSpinBox *voMargin_ = nullptr;
	QSpinBox *voCaptionH_ = nullptr;
	QSpinBox *voTrackH_ = nullptr;
	QSpinBox *voMinClipW_ = nullptr;
	QSpinBox *voEdgeZone_ = nullptr;

	bool loading_ = false;
	// The tab bar, and one id per tab in tab order. Ids rather than indices:
	// two of the pages are only added when their widget exists, so the index of
	// "Colors" is not a constant.
	QTabWidget *tabs_ = nullptr;
	QVector<QString> tabIds_;
	QPushButton *resetTabBtn_ = nullptr; // guard: setting spin values must not re-apply
};

} // namespace harpia
