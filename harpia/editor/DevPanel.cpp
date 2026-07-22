#include "DevPanel.hpp"

#include "EditorWidgets.hpp"
#include "TrackEditor.hpp"
#include "VoiceoverTrack.hpp"
// PreviewCanvas + PreviewLayoutParams come from EditorWidgets.hpp.

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace harpia {

namespace {
QSettings devSettings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
} // namespace

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
		   PreviewCanvas *preview, QWidget *parent)
	: QDialog(parent), timeline_(timeline), tracks_(tracks), voice_(voice), preview_(preview)
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

	// Content lives in a scrollable inner widget so the panel stays usable even
	// with every group expanded; the buttons stay pinned below.
	auto *inner = new QWidget;
	auto *root = new QVBoxLayout(inner);
	auto *hint = new QLabel(
		QStringLiteral("Changes apply live to the editor and are auto-saved — they persist "
			       "across launches. Reset to defaults restores the shipped values."),
		inner);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	root->addWidget(hint);

	const TimelineLayoutParams tl = timeline_->layoutParams();
	auto *tlBox = new QGroupBox(QStringLiteral("Simple Trim timeline"), this);
	auto *tlForm = new QFormLayout(tlBox);
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
	root->addWidget(tlBox);

	const PreviewLayoutParams pv = preview_->layoutParams();
	auto *pvBox = new QGroupBox(QStringLiteral("Preview (both modes)"), inner);
	auto *pvForm = new QFormLayout(pvBox);
	pvForm->addRow(QStringLiteral("Preview min width"),
		       pvW_ = spin(160, 1920, pv.minW, &DevPanel::applyPreview));
	pvForm->addRow(QStringLiteral("Preview min height"),
		       pvH_ = spin(90, 1080, pv.minH, &DevPanel::applyPreview));
	root->addWidget(pvBox);

	const TrackLayoutParams tr = tracks_->layoutParams();
	auto *trBox = new QGroupBox(QStringLiteral("Multi-Cut tracks"), this);
	auto *trForm = new QFormLayout(trBox);
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
	root->addWidget(trBox);

	const VoiceoverLayoutParams vo = voice_->layoutParams();
	auto *voBox = new QGroupBox(QStringLiteral("Voiceover track"), inner);
	auto *voForm = new QFormLayout(voBox);
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
	root->addWidget(voBox);
	root->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(inner);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(scroll, 1);

	auto *btnRow = new QHBoxLayout;
	btnRow->setContentsMargins(10, 6, 10, 8);
	auto *resetBtn = new QPushButton(QStringLiteral("Reset to defaults"), this);
	connect(resetBtn, &QPushButton::clicked, this, &DevPanel::resetDefaults);
	btnRow->addWidget(resetBtn);
	btnRow->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
	btnRow->addWidget(closeBtn);
	outer->addLayout(btnRow);

	resize(380, 620);
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

void DevPanel::resetDefaults()
{
	const TimelineLayoutParams tl; // struct defaults ARE the app defaults
	const TrackLayoutParams tr;
	const VoiceoverLayoutParams vo;
	const PreviewLayoutParams pv;
	loading_ = true;
	tlPad_->setValue(tl.pad);
	tlBarTop_->setValue(tl.barTop);
	tlBarH_->setValue(tl.barH);
	tlHandleW_->setValue(tl.handleW);
	tlTileGap_->setValue(tl.tileGap);
	tlFontPx_->setValue(tl.fontPx);
	tlMaxZoom_->setValue(tl.maxZoom);
	pvW_->setValue(pv.minW);
	pvH_->setValue(pv.minH);
	trMargin_->setValue(tr.margin);
	trCaptionH_->setValue(tr.captionH);
	trSrcH_->setValue(tr.srcH);
	trTrackGap_->setValue(tr.trackGap);
	trOutH_->setValue(tr.outH);
	trSegGap_->setValue(tr.segGap);
	trMinSegW_->setValue(tr.minSegW);
	trHardMinSegW_->setValue(tr.hardMinSegW);
	trTileGap_->setValue(tr.tileGap);
	trCaptionFontPx_->setValue(tr.captionFontPx);
	trSegFontPx_->setValue(tr.segFontPx);
	trMaxZoom_->setValue(tr.maxZoom);
	voMargin_->setValue(vo.margin);
	voCaptionH_->setValue(vo.captionH);
	voTrackH_->setValue(vo.trackH);
	voMinClipW_->setValue(vo.minClipW);
	voEdgeZone_->setValue(vo.edgeZone);
	loading_ = false;
	timeline_->setLayoutParams(tl);
	tracks_->setLayoutParams(tr);
	voice_->setLayoutParams(vo);
	preview_->setLayoutParams(pv);
	saveFrom(tl, tr, vo, pv);
}

} // namespace harpia
