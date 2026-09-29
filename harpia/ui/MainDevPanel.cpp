#include "MainDevPanel.hpp"

#include "MainWindow.hpp"

#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QHideEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
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
} // namespace

void MainDevPanel::loadInto(MainLayoutParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/main"));
	for (const MainLayoutField &f : mainLayoutFields())
		p.*(f.member) = s.value(QString::fromLatin1(f.key), p.*(f.member)).toInt();
	s.endGroup();
}

void MainDevPanel::saveFrom(const MainLayoutParams &p)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/main"));
	for (const MainLayoutField &f : mainLayoutFields())
		s.setValue(QString::fromLatin1(f.key), p.*(f.member));
	s.endGroup();
}

void MainDevPanel::loadAreaStyle(AreaStyle &st)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/areas"));
	QJsonObject o;
	for (const AreaStyleField &f : areaStyleFields()) {
		const QString k = QString::fromLatin1(f.key);
		if (!s.contains(k))
			continue;
		if (f.intMember)
			o[k] = s.value(k).toInt();
		else
			o[k] = s.value(k).toString();
	}
	s.endGroup();
	areaStyleFromJson(o, st); // same clamping and colour checks as Paste JSON
}

void MainDevPanel::saveAreaStyle(const AreaStyle &st)
{
	QSettings s = devSettings();
	s.beginGroup(QStringLiteral("devLayout/areas"));
	const QJsonObject o = areaStyleToJson(st);
	for (auto it = o.begin(); it != o.end(); ++it)
		s.setValue(it.key(), it.value().toVariant());
	s.endGroup();
}

MainDevPanel::MainDevPanel(MainWindow *win, QWidget *parent) : QDialog(parent), win_(win)
{
	setWindowTitle(QStringLiteral("Developer Panel"));
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

	// Which box edits which size -- what apply, Reset and Paste JSON walk.
	spins_ = {{rootMarginH_, &MainLayoutParams::rootMarginH}, {rootMarginV_, &MainLayoutParams::rootMarginV},
		  {rootSpacing_, &MainLayoutParams::rootSpacing}, {row1Spacing_, &MainLayoutParams::row1Spacing},
		  {presetComboW_, &MainLayoutParams::presetComboW}, {monitorMinW_, &MainLayoutParams::monitorComboMinW},
		  {monitorMaxW_, &MainLayoutParams::monitorComboMaxW},
		  {behaviorComboW_, &MainLayoutParams::behaviorComboW},
		  {behaviorLabelW_, &MainLayoutParams::behaviorLabelW},
		  {behaviorColSpacing_, &MainLayoutParams::behaviorColSpacing},
		  {behaviorRowSpacing_, &MainLayoutParams::behaviorRowSpacing},
		  {middleSpacing_, &MainLayoutParams::middleSpacing},
		  {controlsSpacing_, &MainLayoutParams::controlsSpacing}, {recordBtnW_, &MainLayoutParams::recordBtnW},
		  {recordBtnH_, &MainLayoutParams::recordBtnH}, {pauseBtnW_, &MainLayoutParams::pauseBtnW},
		  {pauseBtnH_, &MainLayoutParams::pauseBtnH}, {separatorH_, &MainLayoutParams::separatorH},
		  {webcamPreviewW_, &MainLayoutParams::webcamPreviewW},
		  {webcamPreviewH_, &MainLayoutParams::webcamPreviewH}, {stripThumbW_, &MainLayoutParams::stripThumbW},
		  {stripThumbH_, &MainLayoutParams::stripThumbH}, {stripCardExtraW_, &MainLayoutParams::stripCardExtraW},
		  {stripCardExtraH_, &MainLayoutParams::stripCardExtraH},
		  {stripSpacing_, &MainLayoutParams::stripSpacing}};

	// The panel can get tall — make it scroll rather than force a huge window.
	auto *scroll = new QScrollArea(this);
	scroll->setWidget(inner);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	tabs_ = new QTabWidget(this);
	tabs_->addTab(scroll, QStringLiteral("Window layout"));
	areasTab_ = buildAreasTab();
	tabs_->addTab(areasTab_, QStringLiteral("Areas"));
	outer->addWidget(tabs_, 1);

	auto *btnRow = new QHBoxLayout;
	btnRow->setContentsMargins(10, 6, 10, 8);
	auto *resetBtn = new QPushButton(QStringLiteral("Reset to defaults"), this);
	resetBtn->setToolTip(QStringLiteral("Reset the tab you are on."));
	connect(resetBtn, &QPushButton::clicked, this, &MainDevPanel::resetDefaults);
	btnRow->addWidget(resetBtn);
	// Copy / Paste JSON, like the editor's Developer Panel: send the values
	// to have them made the shipped defaults, or bring a set back.
	auto *copyBtn = new QPushButton(QStringLiteral("Copy JSON"), this);
	copyBtn->setToolTip(QStringLiteral("Copy every value on both tabs as JSON, to paste into a message."));
	connect(copyBtn, &QPushButton::clicked, this, &MainDevPanel::copyJson);
	btnRow->addWidget(copyBtn);
	auto *pasteBtn = new QPushButton(QStringLiteral("Paste JSON"), this);
	pasteBtn->setToolTip(QStringLiteral("Apply values from JSON on the clipboard (as Copy JSON makes)."));
	connect(pasteBtn, &QPushButton::clicked, this, &MainDevPanel::pasteJson);
	btnRow->addWidget(pasteBtn);
	btnRow->addStretch(1);
	auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
	connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
	btnRow->addWidget(closeBtn);
	outer->addLayout(btnRow);
	jsonStatus_ = new QLabel(this);
	jsonStatus_->setStyleSheet(QStringLiteral("color:#9a9fa8; padding:0 10px 6px 10px;"));
	outer->addWidget(jsonStatus_);

	resize(420, 560);
}

MainLayoutParams MainDevPanel::current() const
{
	MainLayoutParams p;
	for (const auto &sp : spins_)
		p.*(sp.second) = sp.first->value();
	return p;
}

void MainDevPanel::showValues(const MainLayoutParams &p)
{
	loading_ = true;
	for (const auto &sp : spins_)
		sp.first->setValue(p.*(sp.second));
	loading_ = false;
	apply();
}

void MainDevPanel::apply()
{
	const MainLayoutParams p = current();
	win_->setLayoutParams(p);
	saveFrom(p);
}

void MainDevPanel::resetDefaults()
{
	// Struct defaults ARE the app defaults.
	if (tabs_ && tabs_->currentWidget() == areasTab_)
		showAreaStyle(AreaStyle());
	else
		showValues(MainLayoutParams());
}

void MainDevPanel::copyJson()
{
	// Both tabs in one message: the window sizes as before, plus "areaStyle".
	QJsonObject root = QJsonDocument::fromJson(mainLayoutToJson(current())).object();
	root[QStringLiteral("areaStyle")] = areaStyleToJson(currentAreaStyle());
	QGuiApplication::clipboard()->setText(
		QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
	jsonStatus_->setText(QStringLiteral("Copied %1 values as JSON.")
				     .arg(mainLayoutFields().size() + areaStyleFields().size()));
}

void MainDevPanel::pasteJson()
{
	const QByteArray text = QGuiApplication::clipboard()->text().toUtf8();
	MainLayoutParams p = current();
	QString err;
	const int n = mainLayoutFromJson(text, p, &err);
	AreaStyle st = currentAreaStyle();
	const QJsonObject root = QJsonDocument::fromJson(text).object();
	const int na = root.value(QStringLiteral("areaStyle")).isObject()
			       ? areaStyleFromJson(root.value(QStringLiteral("areaStyle")).toObject(), st)
			       : 0;
	if (n == 0 && na == 0) {
		jsonStatus_->setText(QStringLiteral("Nothing pasted: %1.").arg(err));
		return;
	}
	if (n > 0)
		showValues(p);
	if (na > 0)
		showAreaStyle(st);
	jsonStatus_->setText(QStringLiteral("Applied %1 values from the clipboard.").arg(n + na));
}

// ---- Areas tab --------------------------------------------------------------

void MainDevPanel::paintSwatch(QPushButton *b, const QColor &c)
{
	b->setText(c.name(QColor::HexRgb));
	b->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:%2;border:1px solid #555;"
					"border-radius:4px;padding:4px 10px;}")
				 .arg(c.name(), c.lightness() > 140 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

QWidget *MainDevPanel::buildAreasTab()
{
	const AreaStyle cur = win_->areaStyle();
	auto *inner = new QWidget;
	auto *root = new QVBoxLayout(inner);
	auto *hint = new QLabel(
		QStringLiteral("How the Multiple-areas layout is drawn on the desktop. Changes apply live "
			       "and are saved. Turn on the preview to see them without recording."),
		inner);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	root->addWidget(hint);

	auto *pvBox = new QGroupBox(QStringLiteral("Preview"), inner);
	auto *pvForm = new QFormLayout(pvBox);
	areaPreview_ = new QComboBox(pvBox);
	areaPreview_->addItem(QStringLiteral("Off"));
	areaPreview_->addItem(QStringLiteral("As when idle"));
	areaPreview_->addItem(QStringLiteral("As while recording"));
	areaPreview_->setToolTip(QStringLiteral("Show the areas on screen with this style. Uses your "
						"layout, or three sample areas if there is none."));
	connect(areaPreview_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int i) { win_->setAreaStylePreview(i); });
	pvForm->addRow(QStringLiteral("Show on screen"), areaPreview_);
	root->addWidget(pvBox);

	auto *lineBox = new QGroupBox(QStringLiteral("Outlines"), inner);
	auto *lineForm = new QFormLayout(lineBox);
	auto *badgeBox = new QGroupBox(QStringLiteral("Number badges"), inner);
	auto *badgeForm = new QFormLayout(badgeBox);
	auto *arrBox = new QGroupBox(QStringLiteral("Arrange mode"), inner);
	auto *arrForm = new QFormLayout(arrBox);

	const QVector<AreaStyleField> &fields = areaStyleFields();
	areaSpins_.fill(nullptr, fields.size());
	areaSwatches_.fill(nullptr, fields.size());
	areaColors_.fill(QColor(), fields.size());
	for (int i = 0; i < fields.size(); ++i) {
		const AreaStyleField &f = fields[i];
		const QString key = QString::fromLatin1(f.key);
		QFormLayout *form = key.startsWith(QLatin1String("badge"))                          ? badgeForm
				    : key.startsWith(QLatin1String("arrange")) || key.startsWith(QLatin1String("hover")) ||
						      key.startsWith(QLatin1String("ghost"))
					    ? arrForm
					    : lineForm;
		QWidget *control = nullptr;
		if (f.colorMember) {
			auto *b = new QPushButton(inner);
			areaColors_[i] = cur.*(f.colorMember);
			paintSwatch(b, areaColors_[i]);
			connect(b, &QPushButton::clicked, this, [this, i, b]() {
				const QColor c = QColorDialog::getColor(areaColors_[i], this,
									 QStringLiteral("Pick a color"));
				if (!c.isValid())
					return;
				areaColors_[i] = c;
				paintSwatch(b, c);
				if (!loading_)
					applyAreaStyle();
			});
			areaSwatches_[i] = b;
			control = b;
		} else if (f.intMember == &AreaStyle::lineStyle) {
			lineStyleCombo_ = new QComboBox(inner);
			lineStyleCombo_->addItem(QStringLiteral("Solid"));
			lineStyleCombo_->addItem(QStringLiteral("Dashed"));
			lineStyleCombo_->addItem(QStringLiteral("Dotted"));
			lineStyleCombo_->addItem(QStringLiteral("Dash-dot"));
			lineStyleCombo_->setCurrentIndex(std::clamp(cur.lineStyle, 0, 3));
			connect(lineStyleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
				if (!loading_)
					applyAreaStyle();
			});
			control = lineStyleCombo_;
		} else {
			auto *sp = new QSpinBox(inner);
			sp->setRange(f.min, f.max);
			sp->setValue(cur.*(f.intMember));
			connect(sp, &QSpinBox::valueChanged, this, [this]() {
				if (!loading_)
					applyAreaStyle();
			});
			areaSpins_[i] = sp;
			control = sp;
		}
		form->addRow(QString::fromLatin1(f.label).remove(QStringLiteral("Arrange: ")), control);
	}
	root->addWidget(lineBox);
	root->addWidget(badgeBox);
	root->addWidget(arrBox);
	root->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(inner);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	return scroll;
}

AreaStyle MainDevPanel::currentAreaStyle() const
{
	AreaStyle st;
	const QVector<AreaStyleField> &fields = areaStyleFields();
	for (int i = 0; i < fields.size() && i < areaSpins_.size(); ++i) {
		const AreaStyleField &f = fields[i];
		if (f.colorMember)
			st.*(f.colorMember) = areaColors_[i];
		else if (f.intMember == &AreaStyle::lineStyle)
			st.lineStyle = lineStyleCombo_ ? lineStyleCombo_->currentIndex() : st.lineStyle;
		else if (areaSpins_[i])
			st.*(f.intMember) = areaSpins_[i]->value();
	}
	return st;
}

void MainDevPanel::showAreaStyle(const AreaStyle &st)
{
	loading_ = true;
	const QVector<AreaStyleField> &fields = areaStyleFields();
	for (int i = 0; i < fields.size() && i < areaSpins_.size(); ++i) {
		const AreaStyleField &f = fields[i];
		if (f.colorMember) {
			areaColors_[i] = st.*(f.colorMember);
			paintSwatch(areaSwatches_[i], areaColors_[i]);
		} else if (f.intMember == &AreaStyle::lineStyle) {
			lineStyleCombo_->setCurrentIndex(std::clamp(st.lineStyle, 0, 3));
		} else if (areaSpins_[i]) {
			areaSpins_[i]->setValue(st.*(f.intMember));
		}
	}
	loading_ = false;
	applyAreaStyle();
}

void MainDevPanel::applyAreaStyle()
{
	const AreaStyle st = currentAreaStyle();
	win_->setAreaStyle(st);
	saveAreaStyle(st);
}

void MainDevPanel::hideEvent(QHideEvent *e)
{
	// A preview left on after the panel closes would be a set of green boxes
	// on the desktop that nothing explains.
	if (areaPreview_)
		areaPreview_->setCurrentIndex(0);
	QDialog::hideEvent(e);
}

} // namespace harpia
