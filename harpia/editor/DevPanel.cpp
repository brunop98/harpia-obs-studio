#include "DevPanel.hpp"

#include "EditorWidgets.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverTrack.hpp"
#include "timeline/TimelineView.hpp"
// PreviewCanvas + PreviewLayoutParams come from EditorWidgets.hpp.

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

namespace harpia {

namespace {
QSettings devSettings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
// The editor's timecode and inspector font sizes were fixed at 18 and 13; they
// now follow the app-wide text scale. Every existing install has those old
// numbers written out from a previous shutdown, so without this they would
// keep them forever and the one part of the UI the user looks at most would
// never shrink.
//
// A one-shot flag rather than "ignore the value if it equals the old default":
// that comparison would also silently revert anyone who later dialled the size
// back to exactly 18, which is a much worse bug than the one it fixes. Must be
// called inside the settings group the keys live in.
void migrateFontSizes(QSettings &s)
{
	if (s.value(QStringLiteral("win/fontsFollowScale"), false).toBool())
		return;
	s.remove(QStringLiteral("win/tcFont"));
	s.remove(QStringLiteral("win/insFont"));
	s.setValue(QStringLiteral("win/fontsFollowScale"), true);
}

} // namespace

EditorChromeParams DevPanel::loadChrome()
{
	const EditorChromeParams d; // struct defaults ARE the shipped defaults
	EditorChromeParams p;
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	p.buttonH = s.value(QStringLiteral("win/btnH"), d.buttonH).toInt();
	migrateFontSizes(s); // once: drop pre-text-scale sizes so the scale applies
	p.timecodeFontPx = s.value(QStringLiteral("win/tcFont"), d.timecodeFontPx).toInt();
	p.inspectorFontPx = s.value(QStringLiteral("win/insFont"), d.inspectorFontPx).toInt();
	p.speedSliderMinW = s.value(QStringLiteral("win/speedW"), d.speedSliderMinW).toInt();
	p.speedSpinW = s.value(QStringLiteral("win/spinW"), d.speedSpinW).toInt();
	p.powerSaveOnBlur =
		s.value(QStringLiteral("win/powerSave"), d.powerSaveOnBlur).toBool();
	s.endGroup();
	return p;
}

EditorInspectorParams DevPanel::loadInspector()
{
	const EditorInspectorParams d; // struct defaults ARE the shipped defaults
	EditorInspectorParams p;
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	p.minWidth = s.value(QStringLiteral("ins/minW"), d.minWidth).toInt();
	p.openWidth = s.value(QStringLiteral("ins/openW"), d.openWidth).toInt();
	p.margin = s.value(QStringLiteral("ins/margin"), d.margin).toInt();
	p.spacing = s.value(QStringLiteral("ins/spacing"), d.spacing).toInt();
	p.labelSpacing = s.value(QStringLiteral("ins/labelSp"), d.labelSpacing).toInt();
	p.rowSpacing = s.value(QStringLiteral("ins/rowSp"), d.rowSpacing).toInt();
	p.scriptListH = s.value(QStringLiteral("ins/scriptH"), d.scriptListH).toInt();
	s.endGroup();
	return p;
}

void DevPanel::saveInspector(const EditorInspectorParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("ins/minW"), p.minWidth);
	s.setValue(QStringLiteral("ins/openW"), p.openWidth);
	s.setValue(QStringLiteral("ins/margin"), p.margin);
	s.setValue(QStringLiteral("ins/spacing"), p.spacing);
	s.setValue(QStringLiteral("ins/labelSp"), p.labelSpacing);
	s.setValue(QStringLiteral("ins/rowSp"), p.rowSpacing);
	s.setValue(QStringLiteral("ins/scriptH"), p.scriptListH);
	s.endGroup();
}

void DevPanel::applyInspector()
{
	EditorInspectorParams p;
	p.minWidth = insMinW_->value();
	p.openWidth = insOpenW_->value();
	p.margin = insMargin_->value();
	p.spacing = insSpacing_->value();
	p.labelSpacing = insLabelSp_->value();
	p.rowSpacing = insRowSp_->value();
	p.scriptListH = insScriptH_->value();
	saveInspector(p);
	emit inspectorChanged(p);
}

void DevPanel::saveChrome(const EditorChromeParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("win/btnH"), p.buttonH);
	s.setValue(QStringLiteral("win/tcFont"), p.timecodeFontPx);
	s.setValue(QStringLiteral("win/insFont"), p.inspectorFontPx);
	s.setValue(QStringLiteral("win/speedW"), p.speedSliderMinW);
	s.setValue(QStringLiteral("win/spinW"), p.speedSpinW);
	s.setValue(QStringLiteral("win/powerSave"), p.powerSaveOnBlur);
	s.endGroup();
}

// The palette table: one row per colour, used for the settings keys, the panel
// rows and the reset, so the three can never disagree about what exists.
namespace {
struct ColorDef {
	const char *key;
	const char *label;
	QColor EditorColors::*field;
	const char *hint;
};
const ColorDef kColorDefs[] = {
	{"timelineBg", "Timeline background", &EditorColors::timelineBg,
	 "Behind the multi-track timeline"},
	{"panelBg", "Bar background", &EditorColors::panelBg,
	 "The Trim / Multi-Cut / voiceover bars"},
	{"gutter", "Track header column", &EditorColors::gutter, ""},
	{"lane", "Track lane", &EditorColors::lane, ""},
	{"laneAlt", "Track lane (alternate)", &EditorColors::laneAlt,
	 "Every other lane, so the rows read apart"},
	{"border", "Border", &EditorColors::border, ""},
	{"caption", "Secondary text", &EditorColors::caption, ""},
	{"accent", "Accent", &EditorColors::accent, "Selection and drop targets"},
	{"videoClip", "Video clip", &EditorColors::videoClip,
	 "Full editing: only for tracks with no colour of their own — a track's own "
	 "colour wins. Also the Multi-Cut segment fill."},
	{"videoClipSel", "Video clip (selected)", &EditorColors::videoClipSel, ""},
	{"audioClip", "Audio clip", &EditorColors::audioClip,
	 "Same: the fallback for a track with no colour, and the voiceover clip fill."},
	{"audioClipSel", "Audio clip (selected)", &EditorColors::audioClipSel, ""},
	{"textClip", "Text clip", &EditorColors::textClip, ""},
	{"textClipSel", "Text clip (selected)", &EditorColors::textClipSel, ""},
	{"waveform", "Waveform", &EditorColors::waveform, ""},
	{"effectClip", "Effect clip", &EditorColors::effectClip,
	 "Effect clips always use this, never the track's colour"},
	{"playhead", "Playhead", &EditorColors::playhead, "Where an edit will land"},
	{"hover", "Hover marker", &EditorColors::hover, "Where the preview is looking"},
	{"marker", "Project marker", &EditorColors::marker, ""},
	{"snapGuide", "Snap guide", &EditorColors::snapGuide,
	 "The magnet line, while a drag is held on it"},
	{"fade", "Audio fade", &EditorColors::fade, "Fade envelope and its grips"},
};
} // namespace

EditorColors DevPanel::loadColors()
{
	const EditorColors d; // struct defaults ARE the shipped palette
	EditorColors c;
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	for (const ColorDef &cd : kColorDefs) {
		const QString key = QStringLiteral("color/") + QLatin1String(cd.key);
		const QString saved = s.value(key).toString();
		const QColor parsed(saved);
		// An unset or unparseable entry keeps the shipped colour rather than
		// painting the editor black.
		c.*(cd.field) = parsed.isValid() ? parsed : d.*(cd.field);
	}
	s.endGroup();
	return c;
}

void DevPanel::saveColors(const EditorColors &c)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	for (const ColorDef &cd : kColorDefs)
		s.setValue(QStringLiteral("color/") + QLatin1String(cd.key),
			   (c.*(cd.field)).name(QColor::HexRgb));
	s.endGroup();
}

void DevPanel::paintSwatch(const ColorRow &r) const
{
	const QColor c = colors_.*(r.field);
	// Readable label whichever way the colour goes.
	const QString fg = (c.lightness() > 140) ? QStringLiteral("#101214")
						 : QStringLiteral("#f0f0f0");
	r.btn->setText(c.name(QColor::HexRgb).toUpper());
	r.btn->setStyleSheet(QStringLiteral("background:%1; color:%2; border:1px solid #444; "
					    "padding:4px 8px; text-align:left;")
				     .arg(c.name(QColor::HexRgb), fg));
}

void DevPanel::applyColors()
{
	saveColors(colors_);
	if (fullTimeline_)
		fullTimeline_->setColors(colors_);
	if (tracks_)
		tracks_->setColors(colors_);
	if (voice_)
		voice_->setColors(colors_);
}

void DevPanel::previewColor(int rowIdx, const QColor &candidate)
{
	if (rowIdx < 0 || rowIdx >= colorRows_.size() || !candidate.isValid())
		return;
	colors_.*(colorRows_[rowIdx].field) = candidate;
	paintSwatch(colorRows_[rowIdx]);
	// Straight to the widgets, NOT through applyColors(): that saves to
	// QSettings, and this runs on every movement inside the colour picker.
	if (fullTimeline_)
		fullTimeline_->setColors(colors_);
	if (tracks_)
		tracks_->setColors(colors_);
	if (voice_)
		voice_->setColors(colors_);
}

void DevPanel::finishColorPick(int rowIdx, const QColor &original, bool accepted, const QColor &picked)
{
	if (rowIdx < 0 || rowIdx >= colorRows_.size())
		return;
	// Cancel means "as if I never opened the dialog": whatever the live
	// preview painted in the meantime is rolled back to the colour the row
	// had before. Accept keeps the choice. Either way the palette is saved
	// exactly once, here, not per mouse-move.
	colors_.*(colorRows_[rowIdx].field) = (accepted && picked.isValid()) ? picked : original;
	paintSwatch(colorRows_[rowIdx]);
	applyColors();
}

TimelineViewParams DevPanel::loadFullTimeline()
{
	TimelineViewParams p; // struct defaults are the shipped values
	const TimelineViewParams d;
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	p.gutterW = s.value(QStringLiteral("ft/gutterW"), d.gutterW).toInt();
	p.rulerH = s.value(QStringLiteral("ft/rulerH"), d.rulerH).toInt();
	p.videoLaneH = s.value(QStringLiteral("ft/videoLaneH"), d.videoLaneH).toInt();
	p.audioLaneH = s.value(QStringLiteral("ft/audioLaneH"), d.audioLaneH).toInt();
	p.laneGap = s.value(QStringLiteral("ft/laneGap"), d.laneGap).toInt();
	p.margin = s.value(QStringLiteral("ft/margin"), d.margin).toInt();
	p.minClipW = s.value(QStringLiteral("ft/minClipW"), d.minClipW).toInt();
	p.clipGap = s.value(QStringLiteral("ft/clipGap"), d.clipGap).toInt();
	p.clipRadius = s.value(QStringLiteral("ft/clipRadius"), d.clipRadius).toInt();
	p.splitSeamW = s.value(QStringLiteral("ft/splitSeamW"), d.splitSeamW).toInt();
	p.snapPx = s.value(QStringLiteral("ft/snapPx"), d.snapPx).toInt();
	p.dropBandPx = s.value(QStringLiteral("ft/dropBandPx"), d.dropBandPx).toInt();
	p.segFontPx = s.value(QStringLiteral("ft/segFontPx"), d.segFontPx).toInt();
	p.maxZoom = s.value(QStringLiteral("ft/maxZoom"), d.maxZoom).toDouble();
	s.endGroup();
	return p;
}

KeyframeLayoutParams DevPanel::loadKeyframe()
{
	KeyframeLayoutParams p;
	const KeyframeLayoutParams d; // struct defaults ARE the shipped values
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	p.margin = s.value(QStringLiteral("kf/margin"), d.margin).toInt();
	p.rulerH = s.value(QStringLiteral("kf/rulerH"), d.rulerH).toInt();
	p.grab = s.value(QStringLiteral("kf/grab"), d.grab).toInt();
	p.diamond = s.value(QStringLiteral("kf/diamond"), d.diamond).toInt();
	p.laneMinH = s.value(QStringLiteral("kf/laneMinH"), d.laneMinH).toInt();
	p.maxZoom = s.value(QStringLiteral("kf/maxZoom"), d.maxZoom).toDouble();
	s.endGroup();
	return p;
}

void DevPanel::saveKeyframe(const KeyframeLayoutParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("kf/margin"), p.margin);
	s.setValue(QStringLiteral("kf/rulerH"), p.rulerH);
	s.setValue(QStringLiteral("kf/grab"), p.grab);
	s.setValue(QStringLiteral("kf/diamond"), p.diamond);
	s.setValue(QStringLiteral("kf/laneMinH"), p.laneMinH);
	s.setValue(QStringLiteral("kf/maxZoom"), p.maxZoom);
	s.endGroup();
}

void DevPanel::saveFullTimeline(const TimelineViewParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("ft/gutterW"), p.gutterW);
	s.setValue(QStringLiteral("ft/rulerH"), p.rulerH);
	s.setValue(QStringLiteral("ft/videoLaneH"), p.videoLaneH);
	s.setValue(QStringLiteral("ft/audioLaneH"), p.audioLaneH);
	s.setValue(QStringLiteral("ft/laneGap"), p.laneGap);
	s.setValue(QStringLiteral("ft/margin"), p.margin);
	s.setValue(QStringLiteral("ft/minClipW"), p.minClipW);
	s.setValue(QStringLiteral("ft/clipGap"), p.clipGap);
	s.setValue(QStringLiteral("ft/clipRadius"), p.clipRadius);
	s.setValue(QStringLiteral("ft/splitSeamW"), p.splitSeamW);
	s.setValue(QStringLiteral("ft/snapPx"), p.snapPx);
	s.setValue(QStringLiteral("ft/dropBandPx"), p.dropBandPx);
	s.setValue(QStringLiteral("ft/segFontPx"), p.segFontPx);
	s.setValue(QStringLiteral("ft/maxZoom"), p.maxZoom);
	s.endGroup();
}

void DevPanel::loadInto(TimelineLayoutParams &tl, TrackLayoutParams &tr, VoiceoverLayoutParams &vo,
			PreviewLayoutParams &pv)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	tl.pad = s.value(QStringLiteral("tl/pad"), tl.pad).toInt();
	tl.barTop = s.value(QStringLiteral("tl/barTop"), tl.barTop).toInt();
	tl.barH = s.value(QStringLiteral("tl/barH"), tl.barH).toInt();
	tl.handleW = s.value(QStringLiteral("tl/handleW"), tl.handleW).toInt();
	tl.tileGap = s.value(QStringLiteral("tl/tileGap"), tl.tileGap).toInt();
	tl.fontPx = s.value(QStringLiteral("tl/fontPx"), tl.fontPx).toInt();
	tl.maxZoom = s.value(QStringLiteral("tl/maxZoom"), tl.maxZoom).toDouble();
	tr.margin = s.value(QStringLiteral("tr/margin"), tr.margin).toInt();
	tr.captionH = s.value(QStringLiteral("tr/captionH"), tr.captionH).toInt();
	tr.srcH = s.value(QStringLiteral("tr/srcH"), tr.srcH).toInt();
	tr.trackGap = s.value(QStringLiteral("tr/trackGap"), tr.trackGap).toInt();
	tr.outH = s.value(QStringLiteral("tr/outH"), tr.outH).toInt();
	tr.segGap = s.value(QStringLiteral("tr/segGap"), tr.segGap).toInt();
	tr.minSegW = s.value(QStringLiteral("tr/minSegW"), tr.minSegW).toInt();
	tr.hardMinSegW = s.value(QStringLiteral("tr/hardMinSegW"), tr.hardMinSegW).toInt();
	tr.tileGap = s.value(QStringLiteral("tr/tileGap"), tr.tileGap).toInt();
	tr.captionFontPx = s.value(QStringLiteral("tr/captionFontPx"), tr.captionFontPx).toInt();
	tr.segFontPx = s.value(QStringLiteral("tr/segFontPx"), tr.segFontPx).toInt();
	tr.maxZoom = s.value(QStringLiteral("tr/maxZoom"), tr.maxZoom).toDouble();
	pv.minW = s.value(QStringLiteral("pv/minW"), pv.minW).toInt();
	pv.minH = s.value(QStringLiteral("pv/minH"), pv.minH).toInt();
	vo.margin = s.value(QStringLiteral("vo/margin"), vo.margin).toInt();
	vo.captionH = s.value(QStringLiteral("vo/captionH"), vo.captionH).toInt();
	vo.trackH = s.value(QStringLiteral("vo/trackH"), vo.trackH).toInt();
	vo.minClipW = s.value(QStringLiteral("vo/minClipW"), vo.minClipW).toInt();
	vo.edgeZone = s.value(QStringLiteral("vo/edgeZone"), vo.edgeZone).toInt();
	s.endGroup();
}

void DevPanel::saveFrom(const TimelineLayoutParams &tl, const TrackLayoutParams &tr,
			const VoiceoverLayoutParams &vo, const PreviewLayoutParams &pv)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("tl/pad"), tl.pad);
	s.setValue(QStringLiteral("tl/barTop"), tl.barTop);
	s.setValue(QStringLiteral("tl/barH"), tl.barH);
	s.setValue(QStringLiteral("tl/handleW"), tl.handleW);
	s.setValue(QStringLiteral("tl/tileGap"), tl.tileGap);
	s.setValue(QStringLiteral("tl/fontPx"), tl.fontPx);
	s.setValue(QStringLiteral("tl/maxZoom"), tl.maxZoom);
	s.setValue(QStringLiteral("tr/margin"), tr.margin);
	s.setValue(QStringLiteral("tr/captionH"), tr.captionH);
	s.setValue(QStringLiteral("tr/srcH"), tr.srcH);
	s.setValue(QStringLiteral("tr/trackGap"), tr.trackGap);
	s.setValue(QStringLiteral("tr/outH"), tr.outH);
	s.setValue(QStringLiteral("tr/segGap"), tr.segGap);
	s.setValue(QStringLiteral("tr/minSegW"), tr.minSegW);
	s.setValue(QStringLiteral("tr/hardMinSegW"), tr.hardMinSegW);
	s.setValue(QStringLiteral("tr/tileGap"), tr.tileGap);
	s.setValue(QStringLiteral("tr/captionFontPx"), tr.captionFontPx);
	s.setValue(QStringLiteral("tr/segFontPx"), tr.segFontPx);
	s.setValue(QStringLiteral("tr/maxZoom"), tr.maxZoom);
	s.setValue(QStringLiteral("pv/minW"), pv.minW);
	s.setValue(QStringLiteral("pv/minH"), pv.minH);
	s.setValue(QStringLiteral("vo/margin"), vo.margin);
	s.setValue(QStringLiteral("vo/captionH"), vo.captionH);
	s.setValue(QStringLiteral("vo/trackH"), vo.trackH);
	s.setValue(QStringLiteral("vo/minClipW"), vo.minClipW);
	s.setValue(QStringLiteral("vo/edgeZone"), vo.edgeZone);
	s.endGroup();
}

DevPanel::DevPanel(Timeline *timeline, TrackEditor *tracks, VoiceoverTrack *voice,
		   PreviewCanvas *preview, TimelineView *fullTimeline, QWidget *parent)
	: QDialog(parent), timeline_(timeline), tracks_(tracks), voice_(voice), preview_(preview),
	  fullTimeline_(fullTimeline)
{
	setWindowTitle(QStringLiteral("Developer Panel — timeline layout"));
	// A floating tool window: stays above the editor but never blocks it, so
	// every tweak is visible immediately on the live timelines.
	setWindowFlags(Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	setModal(false);

	auto spin = [this](int min, int max, int value, void (DevPanel::*apply)()) {
		auto *s = new QSpinBox(this);
		s->setRange(min, max);
		s->setValue(value);
		connect(s, &QSpinBox::valueChanged, this, [this, apply]() {
			if (!loading_)
				(this->*apply)();
		});
		return s;
	};
	auto dspin = [this](double min, double max, double value, void (DevPanel::*apply)()) {
		auto *s = new QDoubleSpinBox(this);
		s->setRange(min, max);
		s->setDecimals(1);
		s->setSingleStep(1.0);
		s->setValue(value);
		connect(s, &QDoubleSpinBox::valueChanged, this, [this, apply]() {
			if (!loading_)
				(this->*apply)();
		});
		return s;
	};

	// One tab per section. Everything used to be stacked in a single scroll, so
	// reaching the Multi-Cut or Voiceover numbers meant scrolling past all the
	// others; each section is now a page of its own.
	auto *tabs = new QTabWidget(this);
	tabs_ = tabs;
	tabs->setDocumentMode(true);

	// Each page scrolls independently, so a long section stays usable in a short
	// window and the tab bar never moves.
	// `id` is what Reset uses to find the section again. Not the index: two of
	// the pages only exist when their widget does, so no index is a constant.
	auto addPage = [&](const QString &id, const QString &title, const QString &blurb) {
		auto *page = new QWidget;
		auto *pv = new QVBoxLayout(page);
		pv->setContentsMargins(12, 10, 12, 10);
		pv->setSpacing(8);
		if (!blurb.isEmpty()) {
			auto *lb = new QLabel(blurb, page);
			lb->setWordWrap(true);
			lb->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
			pv->addWidget(lb);
		}
		auto *form = new QFormLayout;
		form->setLabelAlignment(Qt::AlignLeft);
		form->setHorizontalSpacing(12);
		form->setVerticalSpacing(6);
		pv->addLayout(form);
		pv->addStretch(1);

		auto *sc = new QScrollArea(tabs);
		sc->setWidget(page);
		sc->setWidgetResizable(true);
		sc->setFrameShape(QFrame::NoFrame);
		tabs->addTab(sc, title);
		tabIds_.append(id);
		return form;
	};

	// Window-level toolbar tweaks (not part of any timeline layout struct).
	const EditorChromeParams ch = loadChrome();
	QFormLayout *winForm = addPage(
		QStringLiteral("window"), QStringLiteral("Window"),
		QStringLiteral("Toolbar and panel sizing for the editor window itself."));
	winForm->addRow(QStringLiteral("Button height"),
			winBtnH_ = spin(16, 64, ch.buttonH, &DevPanel::applyChrome));
	winForm->addRow(QStringLiteral("Timecode font size"),
			winTcFont_ = spin(8, 48, ch.timecodeFontPx, &DevPanel::applyChrome));
	winForm->addRow(QStringLiteral("Inspector font size"),
			winInsFont_ = spin(8, 32, ch.inspectorFontPx, &DevPanel::applyChrome));
	winForm->addRow(QStringLiteral("Speed slider min width"),
			winSpeedW_ = spin(60, 600, ch.speedSliderMinW, &DevPanel::applyChrome));
	winForm->addRow(QStringLiteral("Speed value box width"),
			winSpinW_ = spin(48, 160, ch.speedSpinW, &DevPanel::applyChrome));
	winPowerSave_ = new QCheckBox(QStringLiteral("Pause playback when the app is in the background"),
				      this);
	winPowerSave_->setChecked(ch.powerSaveOnBlur);
	winPowerSave_->setToolTip(QStringLiteral(
		"Preview playback is the only thing that keeps working on its own, so pausing it "
		"is what stops the editor draining the battery while you are in another app. "
		"Recording and exporting are never interrupted."));
	connect(winPowerSave_, &QCheckBox::toggled, this, [this]() {
		if (!loading_)
			applyChrome();
	});
	winForm->addRow(QString(), winPowerSave_);

	const TimelineLayoutParams tl = timeline_->layoutParams();
	QFormLayout *tlForm = addPage(QStringLiteral("trim"), QStringLiteral("Trim"),
				      QStringLiteral("The single-range timeline in Simple Trim mode."));
	tlForm->addRow(QStringLiteral("Side padding"),
		       tlPad_ = spin(0, 64, tl.pad, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Bar top offset"),
		       tlBarTop_ = spin(0, 64, tl.barTop, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Bar height (thumb size)"),
		       tlBarH_ = spin(16, 160, tl.barH, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Handle width"),
		       tlHandleW_ = spin(4, 24, tl.handleW, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Thumbnail gap"),
		       tlTileGap_ = spin(0, 16, tl.tileGap, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Font size"),
		       tlFontPx_ = spin(6, 40, tl.fontPx, &DevPanel::applyTimeline));
	tlForm->addRow(QStringLiteral("Max zoom"),
		       tlMaxZoom_ = dspin(1.0, 256.0, tl.maxZoom, &DevPanel::applyTimeline));

	const PreviewLayoutParams pv = preview_->layoutParams();
	QFormLayout *pvForm = addPage(QStringLiteral("preview"), QStringLiteral("Preview"),
				      QStringLiteral("The video preview, shared by every mode."));
	pvForm->addRow(QStringLiteral("Preview min width"),
		       pvW_ = spin(160, 1920, pv.minW, &DevPanel::applyPreview));
	pvForm->addRow(QStringLiteral("Preview min height"),
		       pvH_ = spin(90, 1080, pv.minH, &DevPanel::applyPreview));

	const TrackLayoutParams tr = tracks_->layoutParams();
	const EditorInspectorParams ip = loadInspector();
	QFormLayout *insForm = addPage(
		QStringLiteral("inspector"), QStringLiteral("Inspector"),
		QStringLiteral("The properties panel on the right. Open width is what it "
			       "gets the first time you show it; drag the splitter to override."));
	insForm->addRow(QStringLiteral("Minimum width"),
			insMinW_ = spin(160, 700, ip.minWidth, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Open width"),
			insOpenW_ = spin(180, 900, ip.openWidth, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Content margin"),
			insMargin_ = spin(0, 40, ip.margin, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Section spacing"),
			insSpacing_ = spin(0, 30, ip.spacing, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Label spacing"),
			insLabelSp_ = spin(0, 40, ip.labelSpacing, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Row spacing"),
			insRowSp_ = spin(0, 24, ip.rowSpacing, &DevPanel::applyInspector));
	insForm->addRow(QStringLiteral("Script list height"),
			insScriptH_ = spin(48, 400, ip.scriptListH, &DevPanel::applyInspector));

	QFormLayout *trForm = addPage(
		QStringLiteral("multicut"), QStringLiteral("Multi-Cut"),
		QStringLiteral("The source and output tracks in Multi-Cut mode."));
	trForm->addRow(QStringLiteral("Margin"),
		       trMargin_ = spin(0, 64, tr.margin, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Caption height"),
		       trCaptionH_ = spin(10, 40, tr.captionH, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Source track height (thumb size)"),
		       trSrcH_ = spin(16, 160, tr.srcH, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Track gap"),
		       trTrackGap_ = spin(0, 48, tr.trackGap, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Output track height"),
		       trOutH_ = spin(16, 160, tr.outH, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Segment gap"),
		       trSegGap_ = spin(0, 24, tr.segGap, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Min segment width"),
		       trMinSegW_ = spin(8, 200, tr.minSegW, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Hard min segment width"),
		       trHardMinSegW_ = spin(4, 200, tr.hardMinSegW, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Thumbnail gap"),
		       trTileGap_ = spin(0, 16, tr.tileGap, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Caption font size"),
		       trCaptionFontPx_ = spin(6, 40, tr.captionFontPx, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Segment font size"),
		       trSegFontPx_ = spin(6, 40, tr.segFontPx, &DevPanel::applyTracks));
	trForm->addRow(QStringLiteral("Max zoom"),
		       trMaxZoom_ = dspin(1.0, 256.0, tr.maxZoom, &DevPanel::applyTracks));

	const VoiceoverLayoutParams vo = voice_->layoutParams();
	// Full-editing multi-track timeline (only when that widget exists).
	{
		// Keyframe lanes. Signal-based: the keyframe editor is a dialog that
		// only exists while open, so the values are saved and broadcast and the
		// window applies them whenever it builds one.
		const KeyframeLayoutParams kf = loadKeyframe();
		QFormLayout *kfForm = addPage(
			QStringLiteral("keyframes"), QStringLiteral("Keyframes"),
			QStringLiteral("The per-channel keyframe lanes (Keyframes… on a clip)."));
		kfForm->addRow(QStringLiteral("Lane margin"),
			       kfMargin_ = spin(0, 40, kf.margin, &DevPanel::applyKeyframe));
		kfForm->addRow(QStringLiteral("Ruler height"),
			       kfRulerH_ = spin(8, 48, kf.rulerH, &DevPanel::applyKeyframe));
		kfForm->addRow(QStringLiteral("Key grab radius"),
			       kfGrab_ = spin(3, 24, kf.grab, &DevPanel::applyKeyframe));
		kfGrab_->setToolTip(QStringLiteral(
			"How close the pointer has to be to catch a key. Bigger than the diamond "
			"on purpose -- a marker you can see but not reliably grab is the usual "
			"complaint about keyframe editors."));
		kfForm->addRow(QStringLiteral("Key marker size"),
			       kfDiamond_ = spin(2, 20, kf.diamond, &DevPanel::applyKeyframe));
		kfForm->addRow(QStringLiteral("Lane minimum height"),
			       kfLaneMinH_ = spin(60, 400, kf.laneMinH, &DevPanel::applyKeyframe));
		kfMaxZoom_ = new QDoubleSpinBox(this);
		kfMaxZoom_->setRange(1.0, 512.0);
		kfMaxZoom_->setDecimals(1);
		kfMaxZoom_->setValue(kf.maxZoom);
		connect(kfMaxZoom_, &QDoubleSpinBox::valueChanged, this, [this]() {
			if (!loading_)
				applyKeyframe();
		});
		kfForm->addRow(QStringLiteral("Max zoom"), kfMaxZoom_);
	}

	if (fullTimeline_) {
		// The widget was already given the saved values at startup, so reading
		// them back keeps the boxes in step with what's on screen.
		const TimelineViewParams ft = fullTimeline_->layoutParams();
		QFormLayout *ftForm =
			addPage(QStringLiteral("fulledit"), QStringLiteral("Full editing"),
				QStringLiteral("The multi-track timeline in Full editing mode."));
		ftForm->addRow(QStringLiteral("Track header width"),
			       ftGutterW_ = spin(60, 320, ft.gutterW, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Ruler height"),
			       ftRulerH_ = spin(10, 60, ft.rulerH, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Video lane height"),
			       ftVideoLaneH_ = spin(20, 200, ft.videoLaneH, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Audio lane height"),
			       ftAudioLaneH_ = spin(20, 200, ft.audioLaneH, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Lane gap"),
			       ftLaneGap_ = spin(0, 24, ft.laneGap, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Margin"),
			       ftMargin_ = spin(0, 40, ft.margin, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Min clip width"),
			       ftMinClipW_ = spin(2, 80, ft.minClipW, &DevPanel::applyFullTimeline));
		// The three that decide how visible a cut is. Live, because "obvious
		// enough" is a judgement that has to be made against real footage.
		ftForm->addRow(QStringLiteral("Clip gap (px per side)"),
			       ftClipGap_ = spin(0, 6, ft.clipGap, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Clip corner radius"),
			       ftClipRadius_ = spin(0, 14, ft.clipRadius, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Split seam width"),
			       ftSplitSeamW_ = spin(1, 5, ft.splitSeamW, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Snap distance (px)"),
			       ftSnapPx_ = spin(0, 40, ft.snapPx, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("New-track drop band (px)"),
			       ftDropBandPx_ = spin(2, 30, ft.dropBandPx, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Clip label font size"),
			       ftSegFontPx_ = spin(6, 40, ft.segFontPx, &DevPanel::applyFullTimeline));
		ftForm->addRow(QStringLiteral("Max zoom"),
			       ftMaxZoom_ = dspin(1.0, 512.0, ft.maxZoom, &DevPanel::applyFullTimeline));
	}

	QFormLayout *voForm = addPage(QStringLiteral("voiceover"), QStringLiteral("Voiceover"),
				      QStringLiteral("The narration track under the editing area."));
	voForm->addRow(QStringLiteral("Margin"),
		       voMargin_ = spin(0, 48, vo.margin, &DevPanel::applyVoice));
	voForm->addRow(QStringLiteral("Caption height"),
		       voCaptionH_ = spin(10, 40, vo.captionH, &DevPanel::applyVoice));
	voForm->addRow(QStringLiteral("Track height"),
		       voTrackH_ = spin(16, 160, vo.trackH, &DevPanel::applyVoice));
	voForm->addRow(QStringLiteral("Min clip width"),
		       voMinClipW_ = spin(2, 60, vo.minClipW, &DevPanel::applyVoice));
	voForm->addRow(QStringLiteral("Trim edge zone"),
		       voEdgeZone_ = spin(2, 24, vo.edgeZone, &DevPanel::applyVoice));

	// ---- Colours -------------------------------------------------------
	// One swatch per palette entry. The button IS the colour, so the page reads
	// as a palette rather than as a list of hex strings.
	colors_ = loadColors();
	QFormLayout *colForm = addPage(
		QStringLiteral("colors"), QStringLiteral("Colors"),
		QStringLiteral("The editor palette, shared by all three track widgets. Click a "
			       "swatch to pick a new colour — the timeline previews it LIVE "
			       "while you drag around the picker; Cancel puts it back."));
	for (const ColorDef &cd : kColorDefs) {
		ColorRow row;
		row.key = QLatin1String(cd.key);
		row.field = cd.field;
		row.btn = new QPushButton(this);
		row.btn->setAutoFillBackground(true);
		if (cd.hint[0])
			row.btn->setToolTip(QLatin1String(cd.hint));
		colForm->addRow(QLatin1String(cd.label), row.btn);
		colorRows_.append(row);
		const int idx = colorRows_.size() - 1;
		connect(row.btn, &QPushButton::clicked, this, [this, idx]() {
			const QColor original = colors_.*(colorRows_[idx].field);
			// A real dialog instead of the static getColor(), because the
			// static one only answers on OK -- and the whole point of tuning
			// a timeline colour is seeing it ON THE TIMELINE while dragging
			// around the picker. Every movement previews live; Cancel puts
			// everything back exactly as it was.
			QColorDialog dlg(original, this);
			dlg.setWindowTitle(QStringLiteral("Pick a colour"));
			connect(&dlg, &QColorDialog::currentColorChanged, this,
				[this, idx](const QColor &c) { previewColor(idx, c); });
			const bool accepted = (dlg.exec() == QDialog::Accepted);
			finishColorPick(idx, original, accepted, dlg.selectedColor());
		});
		paintSwatch(colorRows_.back());
	}

	// Come back to the tab you were last on, like every other value here.
	{
		QSettings st = devSettings();
		st.beginGroup(QStringLiteral("devLayout"));
		const int want = st.value(QStringLiteral("tab"), 0).toInt();
		st.endGroup();
		if (want >= 0 && want < tabs->count())
			tabs->setCurrentIndex(want);
	}
	auto syncResetLabel = [this]() {
		if (!resetTabBtn_ || !tabs_)
			return;
		const QString name = tabs_->tabText(tabs_->currentIndex());
		resetTabBtn_->setText(QStringLiteral("Reset “%1”").arg(name));
		resetTabBtn_->setToolTip(
			QStringLiteral("Restore the shipped values for the %1 page only. "
				       "Every other page keeps its settings.")
				.arg(name));
	};
	connect(tabs, &QTabWidget::currentChanged, this, [this, syncResetLabel](int i) {
		QSettings st = devSettings();
		st.beginGroup(QStringLiteral("devLayout"));
		st.setValue(QStringLiteral("tab"), i);
		st.endGroup();
		syncResetLabel();
	});

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *hint = new QLabel(
		QStringLiteral("Changes apply live to the editor and are auto-saved — they persist "
			       "across launches. Reset to defaults restores the shipped values."),
		this);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8; padding:8px 12px 0 12px;"));
	outer->addWidget(hint);
	outer->addWidget(tabs, 1);

	auto *btnRow = new QHBoxLayout;
	btnRow->setContentsMargins(10, 6, 10, 8);
	// Named after the tab it will act on, because a button labelled "Reset to
	// defaults" sitting under nine tabs does not say which of them it means --
	// and it used to mean all nine, which is how one unwanted colour cost you
	// every other tweak in the panel.
	resetTabBtn_ = new QPushButton(this);
	connect(resetTabBtn_, &QPushButton::clicked, this, &DevPanel::resetCurrentTab);
	btnRow->addWidget(resetTabBtn_);
	auto *resetAllBtn = new QPushButton(QStringLiteral("Reset all tabs"), this);
	resetAllBtn->setToolTip(QStringLiteral("Restore the shipped values on every page."));
	connect(resetAllBtn, &QPushButton::clicked, this, &DevPanel::resetDefaults);
	btnRow->addWidget(resetAllBtn);
	btnRow->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
	btnRow->addWidget(closeBtn);
	outer->addLayout(btnRow);

	syncResetLabel(); // the panel opens on a remembered tab, so name it now

	// Wide enough for all eight tab labels; below this the tab bar turns into a
	// pair of scroll arrows and half the sections stop being discoverable.
	resize(700, 680);
}

void DevPanel::applyTimeline()
{
	TimelineLayoutParams p;
	p.pad = tlPad_->value();
	p.barTop = tlBarTop_->value();
	p.barH = tlBarH_->value();
	p.handleW = tlHandleW_->value();
	p.tileGap = tlTileGap_->value();
	p.fontPx = tlFontPx_->value();
	p.maxZoom = tlMaxZoom_->value();
	timeline_->setLayoutParams(p);
	saveFrom(p, tracks_->layoutParams(), voice_->layoutParams(), preview_->layoutParams());
}

void DevPanel::applyTracks()
{
	TrackLayoutParams p;
	p.margin = trMargin_->value();
	p.captionH = trCaptionH_->value();
	p.srcH = trSrcH_->value();
	p.trackGap = trTrackGap_->value();
	p.outH = trOutH_->value();
	p.segGap = trSegGap_->value();
	p.minSegW = trMinSegW_->value();
	p.hardMinSegW = trHardMinSegW_->value();
	p.tileGap = trTileGap_->value();
	p.captionFontPx = trCaptionFontPx_->value();
	p.segFontPx = trSegFontPx_->value();
	p.maxZoom = trMaxZoom_->value();
	tracks_->setLayoutParams(p);
	saveFrom(timeline_->layoutParams(), p, voice_->layoutParams(), preview_->layoutParams());
}

void DevPanel::applyFullTimeline()
{
	if (!fullTimeline_ || !ftGutterW_)
		return;
	TimelineViewParams p;
	p.gutterW = ftGutterW_->value();
	p.rulerH = ftRulerH_->value();
	p.videoLaneH = ftVideoLaneH_->value();
	p.audioLaneH = ftAudioLaneH_->value();
	p.laneGap = ftLaneGap_->value();
	p.margin = ftMargin_->value();
	p.minClipW = ftMinClipW_->value();
	p.clipGap = ftClipGap_->value();
	p.clipRadius = ftClipRadius_->value();
	p.splitSeamW = ftSplitSeamW_->value();
	p.snapPx = ftSnapPx_->value();
	p.dropBandPx = ftDropBandPx_->value();
	p.segFontPx = ftSegFontPx_->value();
	p.maxZoom = ftMaxZoom_->value();
	fullTimeline_->setLayoutParams(p);
	saveFullTimeline(p); // auto-saved, like every other group
}

void DevPanel::applyPreview()
{
	PreviewLayoutParams p;
	p.minW = pvW_->value();
	p.minH = pvH_->value();
	preview_->setLayoutParams(p);
	saveFrom(timeline_->layoutParams(), tracks_->layoutParams(), voice_->layoutParams(), p);
}

void DevPanel::applyVoice()
{
	VoiceoverLayoutParams p;
	p.margin = voMargin_->value();
	p.captionH = voCaptionH_->value();
	p.trackH = voTrackH_->value();
	p.minClipW = voMinClipW_->value();
	p.edgeZone = voEdgeZone_->value();
	voice_->setLayoutParams(p);
	saveFrom(timeline_->layoutParams(), tracks_->layoutParams(), p, preview_->layoutParams());
}

void DevPanel::applyKeyframe()
{
	KeyframeLayoutParams p;
	p.margin = kfMargin_->value();
	p.rulerH = kfRulerH_->value();
	p.grab = kfGrab_->value();
	p.diamond = kfDiamond_->value();
	p.laneMinH = kfLaneMinH_->value();
	p.maxZoom = kfMaxZoom_->value();
	saveKeyframe(p);
	emit keyframeChanged(p);
}

void DevPanel::applyChrome()
{
	EditorChromeParams p;
	p.buttonH = winBtnH_->value();
	p.timecodeFontPx = winTcFont_->value();
	p.inspectorFontPx = winInsFont_->value();
	p.speedSliderMinW = winSpeedW_->value();
	p.speedSpinW = winSpinW_->value();
	p.powerSaveOnBlur = winPowerSave_->isChecked();
	saveChrome(p);
	emit chromeChanged(p);
}

// Each section restores its own struct defaults and then goes through the SAME
// apply function the spin boxes use. That is what keeps a reset and a hand edit
// from drifting apart: there is one path from widget values to the app, and
// reset walks it rather than duplicating the save-and-broadcast half.
//
// loading_ is held across the setValue calls so the apply fires once at the end
// instead of once per box.

void DevPanel::resetWindowTab()
{
	const EditorChromeParams d;
	loading_ = true;
	winBtnH_->setValue(d.buttonH);
	winTcFont_->setValue(d.timecodeFontPx);
	winInsFont_->setValue(d.inspectorFontPx);
	winSpeedW_->setValue(d.speedSliderMinW);
	winSpinW_->setValue(d.speedSpinW);
	winPowerSave_->setChecked(d.powerSaveOnBlur);
	loading_ = false;
	applyChrome();
}

void DevPanel::resetTrimTab()
{
	const TimelineLayoutParams d;
	loading_ = true;
	tlPad_->setValue(d.pad);
	tlBarTop_->setValue(d.barTop);
	tlBarH_->setValue(d.barH);
	tlHandleW_->setValue(d.handleW);
	tlTileGap_->setValue(d.tileGap);
	tlFontPx_->setValue(d.fontPx);
	tlMaxZoom_->setValue(d.maxZoom);
	loading_ = false;
	applyTimeline();
}

void DevPanel::resetPreviewTab()
{
	const PreviewLayoutParams d;
	loading_ = true;
	pvW_->setValue(d.minW);
	pvH_->setValue(d.minH);
	loading_ = false;
	applyPreview();
}

void DevPanel::resetInspectorTab()
{
	const EditorInspectorParams d;
	loading_ = true;
	insMinW_->setValue(d.minWidth);
	insOpenW_->setValue(d.openWidth);
	insMargin_->setValue(d.margin);
	insSpacing_->setValue(d.spacing);
	insLabelSp_->setValue(d.labelSpacing);
	insRowSp_->setValue(d.rowSpacing);
	insScriptH_->setValue(d.scriptListH);
	loading_ = false;
	applyInspector();
}

void DevPanel::resetMultiCutTab()
{
	const TrackLayoutParams d;
	loading_ = true;
	trMargin_->setValue(d.margin);
	trCaptionH_->setValue(d.captionH);
	trSrcH_->setValue(d.srcH);
	trTrackGap_->setValue(d.trackGap);
	trOutH_->setValue(d.outH);
	trSegGap_->setValue(d.segGap);
	trMinSegW_->setValue(d.minSegW);
	trHardMinSegW_->setValue(d.hardMinSegW);
	trTileGap_->setValue(d.tileGap);
	trCaptionFontPx_->setValue(d.captionFontPx);
	trSegFontPx_->setValue(d.segFontPx);
	trMaxZoom_->setValue(d.maxZoom);
	loading_ = false;
	applyTracks();
}

void DevPanel::resetKeyframeTab()
{
	if (!kfMargin_)
		return; // the page only exists when the editor offers keyframes
	const KeyframeLayoutParams d;
	loading_ = true;
	kfMargin_->setValue(d.margin);
	kfRulerH_->setValue(d.rulerH);
	kfGrab_->setValue(d.grab);
	kfDiamond_->setValue(d.diamond);
	kfLaneMinH_->setValue(d.laneMinH);
	kfMaxZoom_->setValue(d.maxZoom);
	loading_ = false;
	applyKeyframe();
}

void DevPanel::resetFullEditTab()
{
	if (!ftGutterW_)
		return; // no Full-editing timeline in this window
	const TimelineViewParams d;
	loading_ = true;
	ftGutterW_->setValue(d.gutterW);
	ftRulerH_->setValue(d.rulerH);
	ftVideoLaneH_->setValue(d.videoLaneH);
	ftAudioLaneH_->setValue(d.audioLaneH);
	ftLaneGap_->setValue(d.laneGap);
	ftMargin_->setValue(d.margin);
	ftMinClipW_->setValue(d.minClipW);
	ftSnapPx_->setValue(d.snapPx);
	ftDropBandPx_->setValue(d.dropBandPx);
	ftSegFontPx_->setValue(d.segFontPx);
	ftMaxZoom_->setValue(d.maxZoom);
	loading_ = false;
	applyFullTimeline();
}

void DevPanel::resetVoiceoverTab()
{
	const VoiceoverLayoutParams d;
	loading_ = true;
	voMargin_->setValue(d.margin);
	voCaptionH_->setValue(d.captionH);
	voTrackH_->setValue(d.trackH);
	voMinClipW_->setValue(d.minClipW);
	voEdgeZone_->setValue(d.edgeZone);
	loading_ = false;
	applyVoice();
}

void DevPanel::resetColorsTab()
{
	colors_ = EditorColors(); // the struct's defaults are the shipped palette
	for (const ColorRow &r : colorRows_)
		paintSwatch(r);
	applyColors();
}

// The page in front of you, and nothing else.
void DevPanel::resetCurrentTab()
{
	if (!tabs_)
		return;
	const int i = tabs_->currentIndex();
	if (i < 0 || i >= tabIds_.size())
		return;
	const QString id = tabIds_.at(i);
	if (id == QLatin1String("window"))
		resetWindowTab();
	else if (id == QLatin1String("trim"))
		resetTrimTab();
	else if (id == QLatin1String("preview"))
		resetPreviewTab();
	else if (id == QLatin1String("inspector"))
		resetInspectorTab();
	else if (id == QLatin1String("multicut"))
		resetMultiCutTab();
	else if (id == QLatin1String("keyframes"))
		resetKeyframeTab();
	else if (id == QLatin1String("fulledit"))
		resetFullEditTab();
	else if (id == QLatin1String("voiceover"))
		resetVoiceoverTab();
	else if (id == QLatin1String("colors"))
		resetColorsTab();
}

// Every page, for when that is genuinely what you want. Expressed as the sum of
// the parts so the two can never disagree about what a default is.
void DevPanel::resetDefaults()
{
	resetWindowTab();
	resetTrimTab();
	resetPreviewTab();
	resetInspectorTab();
	resetMultiCutTab();
	resetKeyframeTab();
	resetFullEditTab();
	resetVoiceoverTab();
	resetColorsTab();
}

} // namespace harpia
