#include "DevPanel.hpp"

#include "EditorWidgets.hpp"
#include "TrackEditor.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

void DevPanel::loadInto(TimelineLayoutParams &tl, TrackLayoutParams &tr)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	tl.pad = s.value(QStringLiteral("tl/pad"), tl.pad).toInt();
	tl.barTop = s.value(QStringLiteral("tl/barTop"), tl.barTop).toInt();
	tl.barH = s.value(QStringLiteral("tl/barH"), tl.barH).toInt();
	tl.handleW = s.value(QStringLiteral("tl/handleW"), tl.handleW).toInt();
	tl.tileGap = s.value(QStringLiteral("tl/tileGap"), tl.tileGap).toInt();
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
	tr.maxZoom = s.value(QStringLiteral("tr/maxZoom"), tr.maxZoom).toDouble();
	s.endGroup();
}

void DevPanel::saveFrom(const TimelineLayoutParams &tl, const TrackLayoutParams &tr)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout"));
	s.setValue(QStringLiteral("tl/pad"), tl.pad);
	s.setValue(QStringLiteral("tl/barTop"), tl.barTop);
	s.setValue(QStringLiteral("tl/barH"), tl.barH);
	s.setValue(QStringLiteral("tl/handleW"), tl.handleW);
	s.setValue(QStringLiteral("tl/tileGap"), tl.tileGap);
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
	s.setValue(QStringLiteral("tr/maxZoom"), tr.maxZoom);
	s.endGroup();
}

DevPanel::DevPanel(Timeline *timeline, TrackEditor *tracks, QWidget *parent)
	: QDialog(parent), timeline_(timeline), tracks_(tracks)
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

	auto *root = new QVBoxLayout(this);
	auto *hint = new QLabel(
		QStringLiteral("Changes apply live to the editor. Values are not saved — "
			       "note down what looks right and make it the new default."),
		this);
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
	tlForm->addRow(QStringLiteral("Max zoom"),
		       tlMaxZoom_ = dspin(1.0, 256.0, tl.maxZoom, &DevPanel::applyTimeline));
	root->addWidget(tlBox);

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
	trForm->addRow(QStringLiteral("Max zoom"),
		       trMaxZoom_ = dspin(1.0, 256.0, tr.maxZoom, &DevPanel::applyTracks));
	root->addWidget(trBox);

	auto *btnRow = new QHBoxLayout;
	auto *resetBtn = new QPushButton(QStringLiteral("Reset to defaults"), this);
	connect(resetBtn, &QPushButton::clicked, this, &DevPanel::resetDefaults);
	btnRow->addWidget(resetBtn);
	btnRow->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
	btnRow->addWidget(closeBtn);
	root->addLayout(btnRow);
}

void DevPanel::applyTimeline()
{
	TimelineLayoutParams p;
	p.pad = tlPad_->value();
	p.barTop = tlBarTop_->value();
	p.barH = tlBarH_->value();
	p.handleW = tlHandleW_->value();
	p.tileGap = tlTileGap_->value();
	p.maxZoom = tlMaxZoom_->value();
	timeline_->setLayoutParams(p);
	saveFrom(p, tracks_->layoutParams());
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
	p.maxZoom = trMaxZoom_->value();
	tracks_->setLayoutParams(p);
	saveFrom(timeline_->layoutParams(), p);
}

void DevPanel::resetDefaults()
{
	const TimelineLayoutParams tl; // struct defaults ARE the app defaults
	const TrackLayoutParams tr;
	loading_ = true;
	tlPad_->setValue(tl.pad);
	tlBarTop_->setValue(tl.barTop);
	tlBarH_->setValue(tl.barH);
	tlHandleW_->setValue(tl.handleW);
	tlTileGap_->setValue(tl.tileGap);
	tlMaxZoom_->setValue(tl.maxZoom);
	trMargin_->setValue(tr.margin);
	trCaptionH_->setValue(tr.captionH);
	trSrcH_->setValue(tr.srcH);
	trTrackGap_->setValue(tr.trackGap);
	trOutH_->setValue(tr.outH);
	trSegGap_->setValue(tr.segGap);
	trMinSegW_->setValue(tr.minSegW);
	trHardMinSegW_->setValue(tr.hardMinSegW);
	trTileGap_->setValue(tr.tileGap);
	trMaxZoom_->setValue(tr.maxZoom);
	loading_ = false;
	timeline_->setLayoutParams(tl);
	tracks_->setLayoutParams(tr);
	saveFrom(tl, tr);
}

} // namespace harpia
