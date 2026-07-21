#include "MainDevPanel.hpp"

#include "MainWindow.hpp"

#include <QFormLayout>
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

void MainDevPanel::loadInto(MainLayoutParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/main"));
	auto i = [&](const QString &k, int def) { return s.value(k, def).toInt(); };
	p.rootMarginH = i(QStringLiteral("rootMarginH"), p.rootMarginH);
	p.rootMarginV = i(QStringLiteral("rootMarginV"), p.rootMarginV);
	p.rootSpacing = i(QStringLiteral("rootSpacing"), p.rootSpacing);
	p.row1Spacing = i(QStringLiteral("row1Spacing"), p.row1Spacing);
	p.presetComboW = i(QStringLiteral("presetComboW"), p.presetComboW);
	p.monitorComboMinW = i(QStringLiteral("monitorComboMinW"), p.monitorComboMinW);
	p.monitorComboMaxW = i(QStringLiteral("monitorComboMaxW"), p.monitorComboMaxW);
	p.behaviorComboW = i(QStringLiteral("behaviorComboW"), p.behaviorComboW);
	p.behaviorLabelW = i(QStringLiteral("behaviorLabelW"), p.behaviorLabelW);
	p.behaviorColSpacing = i(QStringLiteral("behaviorColSpacing"), p.behaviorColSpacing);
	p.behaviorRowSpacing = i(QStringLiteral("behaviorRowSpacing"), p.behaviorRowSpacing);
	p.middleSpacing = i(QStringLiteral("middleSpacing"), p.middleSpacing);
	p.controlsSpacing = i(QStringLiteral("controlsSpacing"), p.controlsSpacing);
	p.recordBtnW = i(QStringLiteral("recordBtnW"), p.recordBtnW);
	p.recordBtnH = i(QStringLiteral("recordBtnH"), p.recordBtnH);
	p.pauseBtnW = i(QStringLiteral("pauseBtnW"), p.pauseBtnW);
	p.pauseBtnH = i(QStringLiteral("pauseBtnH"), p.pauseBtnH);
	p.separatorH = i(QStringLiteral("separatorH"), p.separatorH);
	p.webcamPreviewW = i(QStringLiteral("webcamPreviewW"), p.webcamPreviewW);
	p.webcamPreviewH = i(QStringLiteral("webcamPreviewH"), p.webcamPreviewH);
	p.stripThumbW = i(QStringLiteral("stripThumbW"), p.stripThumbW);
	p.stripThumbH = i(QStringLiteral("stripThumbH"), p.stripThumbH);
	p.stripCardExtraW = i(QStringLiteral("stripCardExtraW"), p.stripCardExtraW);
	p.stripCardExtraH = i(QStringLiteral("stripCardExtraH"), p.stripCardExtraH);
	p.stripSpacing = i(QStringLiteral("stripSpacing"), p.stripSpacing);
	s.endGroup();
}

void MainDevPanel::saveFrom(const MainLayoutParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/main"));
	s.setValue(QStringLiteral("rootMarginH"), p.rootMarginH);
	s.setValue(QStringLiteral("rootMarginV"), p.rootMarginV);
	s.setValue(QStringLiteral("rootSpacing"), p.rootSpacing);
	s.setValue(QStringLiteral("row1Spacing"), p.row1Spacing);
	s.setValue(QStringLiteral("presetComboW"), p.presetComboW);
	s.setValue(QStringLiteral("monitorComboMinW"), p.monitorComboMinW);
	s.setValue(QStringLiteral("monitorComboMaxW"), p.monitorComboMaxW);
	s.setValue(QStringLiteral("behaviorComboW"), p.behaviorComboW);
	s.setValue(QStringLiteral("behaviorLabelW"), p.behaviorLabelW);
	s.setValue(QStringLiteral("behaviorColSpacing"), p.behaviorColSpacing);
	s.setValue(QStringLiteral("behaviorRowSpacing"), p.behaviorRowSpacing);
	s.setValue(QStringLiteral("middleSpacing"), p.middleSpacing);
	s.setValue(QStringLiteral("controlsSpacing"), p.controlsSpacing);
	s.setValue(QStringLiteral("recordBtnW"), p.recordBtnW);
	s.setValue(QStringLiteral("recordBtnH"), p.recordBtnH);
	s.setValue(QStringLiteral("pauseBtnW"), p.pauseBtnW);
	s.setValue(QStringLiteral("pauseBtnH"), p.pauseBtnH);
	s.setValue(QStringLiteral("separatorH"), p.separatorH);
	s.setValue(QStringLiteral("webcamPreviewW"), p.webcamPreviewW);
	s.setValue(QStringLiteral("webcamPreviewH"), p.webcamPreviewH);
	s.setValue(QStringLiteral("stripThumbW"), p.stripThumbW);
	s.setValue(QStringLiteral("stripThumbH"), p.stripThumbH);
	s.setValue(QStringLiteral("stripCardExtraW"), p.stripCardExtraW);
	s.setValue(QStringLiteral("stripCardExtraH"), p.stripCardExtraH);
	s.setValue(QStringLiteral("stripSpacing"), p.stripSpacing);
	s.endGroup();
}

MainDevPanel::MainDevPanel(MainWindow *win, QWidget *parent) : QDialog(parent), win_(win)
{
	setWindowTitle(QStringLiteral("Developer Panel — window layout"));
	// A floating tool window so tweaks stay visible on the live window.
	setWindowFlags(Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
	setModal(false);

	const MainLayoutParams cur = win_->layoutParams();

	auto mk = [this](int min, int max, int value) {
		auto *s = new QSpinBox(this);
		s->setRange(min, max);
		s->setValue(value);
		connect(s, &QSpinBox::valueChanged, this, [this]() {
			if (!loading_)
				apply();
		});
		return s;
	};

	auto *inner = new QWidget;
	auto *root = new QVBoxLayout(inner);
	auto *hint = new QLabel(
		QStringLiteral("Changes apply live and are saved automatically — they persist across "
			       "launches. (Recent-card thumbnails are re-scaled for preview; the "
			       "baked-in resolution updates once the values become defaults.)"),
		inner);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	root->addWidget(hint);

	auto *winBox = new QGroupBox(QStringLiteral("Window"), inner);
	auto *winForm = new QFormLayout(winBox);
	winForm->addRow(QStringLiteral("Outer padding X"), rootMarginH_ = mk(0, 80, cur.rootMarginH));
	winForm->addRow(QStringLiteral("Outer padding Y"), rootMarginV_ = mk(0, 80, cur.rootMarginV));
	winForm->addRow(QStringLiteral("Section spacing"), rootSpacing_ = mk(0, 48, cur.rootSpacing));
	root->addWidget(winBox);

	auto *tbBox = new QGroupBox(QStringLiteral("Toolbar"), inner);
	auto *tbForm = new QFormLayout(tbBox);
	tbForm->addRow(QStringLiteral("Row item spacing"), row1Spacing_ = mk(0, 48, cur.row1Spacing));
	tbForm->addRow(QStringLiteral("Preset combo min width"),
		       presetComboW_ = mk(60, 500, cur.presetComboW));
	tbForm->addRow(QStringLiteral("Display combo min width"),
		       monitorMinW_ = mk(60, 500, cur.monitorComboMinW));
	tbForm->addRow(QStringLiteral("Display combo max width"),
		       monitorMaxW_ = mk(60, 600, cur.monitorComboMaxW));
	root->addWidget(tbBox);

	auto *behBox = new QGroupBox(QStringLiteral("Behavior column"), inner);
	auto *behForm = new QFormLayout(behBox);
	behForm->addRow(QStringLiteral("Combo width"), behaviorComboW_ = mk(80, 500, cur.behaviorComboW));
	behForm->addRow(QStringLiteral("Label width"), behaviorLabelW_ = mk(40, 300, cur.behaviorLabelW));
	behForm->addRow(QStringLiteral("Row spacing"),
			behaviorColSpacing_ = mk(0, 48, cur.behaviorColSpacing));
	behForm->addRow(QStringLiteral("Label↔combo gap"),
			behaviorRowSpacing_ = mk(0, 40, cur.behaviorRowSpacing));
	behForm->addRow(QStringLiteral("Column↔controls gap"),
			middleSpacing_ = mk(0, 64, cur.middleSpacing));
	root->addWidget(behBox);

	auto *ctlBox = new QGroupBox(QStringLiteral("Record controls"), inner);
	auto *ctlForm = new QFormLayout(ctlBox);
	ctlForm->addRow(QStringLiteral("Controls spacing"),
			controlsSpacing_ = mk(0, 64, cur.controlsSpacing));
	ctlForm->addRow(QStringLiteral("Record button width"), recordBtnW_ = mk(80, 400, cur.recordBtnW));
	ctlForm->addRow(QStringLiteral("Record button height"), recordBtnH_ = mk(24, 120, cur.recordBtnH));
	ctlForm->addRow(QStringLiteral("Pause button width"), pauseBtnW_ = mk(60, 400, cur.pauseBtnW));
	ctlForm->addRow(QStringLiteral("Pause button height"), pauseBtnH_ = mk(24, 120, cur.pauseBtnH));
	ctlForm->addRow(QStringLiteral("Separator height"), separatorH_ = mk(8, 96, cur.separatorH));
	ctlForm->addRow(QStringLiteral("Webcam preview width"),
			webcamPreviewW_ = mk(40, 400, cur.webcamPreviewW));
	ctlForm->addRow(QStringLiteral("Webcam preview height"),
			webcamPreviewH_ = mk(24, 300, cur.webcamPreviewH));
	root->addWidget(ctlBox);

	auto *stBox = new QGroupBox(QStringLiteral("Recent recordings"), inner);
	auto *stForm = new QFormLayout(stBox);
	stForm->addRow(QStringLiteral("Thumbnail width"), stripThumbW_ = mk(60, 480, cur.stripThumbW));
	stForm->addRow(QStringLiteral("Thumbnail height"), stripThumbH_ = mk(40, 320, cur.stripThumbH));
	stForm->addRow(QStringLiteral("Card extra width"),
		       stripCardExtraW_ = mk(0, 120, cur.stripCardExtraW));
	stForm->addRow(QStringLiteral("Card extra height"),
		       stripCardExtraH_ = mk(0, 120, cur.stripCardExtraH));
	stForm->addRow(QStringLiteral("Card spacing"), stripSpacing_ = mk(0, 40, cur.stripSpacing));
	root->addWidget(stBox);

	root->addStretch(1);

	// The panel can get tall — make it scroll rather than force a huge window.
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
	connect(resetBtn, &QPushButton::clicked, this, &MainDevPanel::resetDefaults);
	btnRow->addWidget(resetBtn);
	btnRow->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
	btnRow->addWidget(closeBtn);
	outer->addLayout(btnRow);

	resize(360, 560);
}

void MainDevPanel::apply()
{
	MainLayoutParams p;
	p.rootMarginH = rootMarginH_->value();
	p.rootMarginV = rootMarginV_->value();
	p.rootSpacing = rootSpacing_->value();
	p.row1Spacing = row1Spacing_->value();
	p.presetComboW = presetComboW_->value();
	p.monitorComboMinW = monitorMinW_->value();
	p.monitorComboMaxW = monitorMaxW_->value();
	p.behaviorComboW = behaviorComboW_->value();
	p.behaviorLabelW = behaviorLabelW_->value();
	p.behaviorColSpacing = behaviorColSpacing_->value();
	p.behaviorRowSpacing = behaviorRowSpacing_->value();
	p.middleSpacing = middleSpacing_->value();
	p.controlsSpacing = controlsSpacing_->value();
	p.recordBtnW = recordBtnW_->value();
	p.recordBtnH = recordBtnH_->value();
	p.pauseBtnW = pauseBtnW_->value();
	p.pauseBtnH = pauseBtnH_->value();
	p.separatorH = separatorH_->value();
	p.webcamPreviewW = webcamPreviewW_->value();
	p.webcamPreviewH = webcamPreviewH_->value();
	p.stripThumbW = stripThumbW_->value();
	p.stripThumbH = stripThumbH_->value();
	p.stripCardExtraW = stripCardExtraW_->value();
	p.stripCardExtraH = stripCardExtraH_->value();
	p.stripSpacing = stripSpacing_->value();
	win_->setLayoutParams(p);
	saveFrom(p);
}

void MainDevPanel::resetDefaults()
{
	const MainLayoutParams d; // struct defaults ARE the app defaults
	loading_ = true;
	rootMarginH_->setValue(d.rootMarginH);
	rootMarginV_->setValue(d.rootMarginV);
	rootSpacing_->setValue(d.rootSpacing);
	row1Spacing_->setValue(d.row1Spacing);
	presetComboW_->setValue(d.presetComboW);
	monitorMinW_->setValue(d.monitorComboMinW);
	monitorMaxW_->setValue(d.monitorComboMaxW);
	behaviorComboW_->setValue(d.behaviorComboW);
	behaviorLabelW_->setValue(d.behaviorLabelW);
	behaviorColSpacing_->setValue(d.behaviorColSpacing);
	behaviorRowSpacing_->setValue(d.behaviorRowSpacing);
	middleSpacing_->setValue(d.middleSpacing);
	controlsSpacing_->setValue(d.controlsSpacing);
	recordBtnW_->setValue(d.recordBtnW);
	recordBtnH_->setValue(d.recordBtnH);
	pauseBtnW_->setValue(d.pauseBtnW);
	pauseBtnH_->setValue(d.pauseBtnH);
	separatorH_->setValue(d.separatorH);
	webcamPreviewW_->setValue(d.webcamPreviewW);
	webcamPreviewH_->setValue(d.webcamPreviewH);
	stripThumbW_->setValue(d.stripThumbW);
	stripThumbH_->setValue(d.stripThumbH);
	stripCardExtraW_->setValue(d.stripCardExtraW);
	stripCardExtraH_->setValue(d.stripCardExtraH);
	stripSpacing_->setValue(d.stripSpacing);
	loading_ = false;
	apply();
}

} // namespace harpia
