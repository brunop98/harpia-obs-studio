#include "PresetEditorDialog.hpp"

#include "MousePreview.hpp"
#include "ShortcutConflictDialog.hpp"
#include "AudioPanel.hpp"
#include "SpotlightPreview.hpp"
#include "core/CaptureManager.hpp"
#include "core/EncoderFactory.hpp"
#include "core/WebcamRecorder.hpp"
#include "core/ModeCapabilities.hpp"   // one table of what each mode supports
#include "core/ShortcutConflicts.hpp" // the no-duplicate-shortcuts rule
#include "core/SpotlightFx.hpp"     // SpotlightParams limits, shared with the overlay
#include "core/ZoomMode.hpp"      // ZoomParams::kMinPercent / kMaxPercent
#include "core/AudioManager.hpp" // AudioDevice
#include "model/FileNameTemplate.hpp"
#include "platform/CameraAccess.hpp"

#include <map>

#include <algorithm>

#include <QAbstractButton>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace harpia {

namespace {

const char *codecLabel(VideoCodec c)
{
	switch (c) {
	case VideoCodec::H264:
		return "H.264";
	case VideoCodec::HEVC:
		return "H.265 / HEVC";
	case VideoCodec::AV1:
		return "AV1";
	}
	return "H.264";
}

// Paint a color-swatch button with the given color.
void setButtonColor(QPushButton *b, const QColor &c)
{
	b->setStyleSheet(QStringLiteral("background:%1; border:1px solid #555; border-radius:4px;")
				 .arg(c.name()));
	b->setText(c.name());
}

// Create an empty settings page: a scrollable widget whose inner QVBoxLayout is
// returned via `outLayout` for callers to fill with fields.
QWidget *makePage(QVBoxLayout *&outLayout)
{
	auto *inner = new QWidget;
	outLayout = new QVBoxLayout(inner);
	outLayout->setContentsMargins(4, 4, 12, 4);
	outLayout->setSpacing(4);

	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(inner);
	return scroll;
}

// A titled setting: bold label, one short gray line, then the control.
//
// Returned as a single widget so a caller can hide the whole setting -- label,
// description and control together -- with one call. That is what dependsOn()
// below is built on; hiding only the control would leave a heading over an
// empty space.
//
// The gap below the setting is the row's own bottom margin rather than spacing
// added to the page, because spacing added to the page stays behind when the
// row is hidden and the page ends up full of holes.
QWidget *addField(QVBoxLayout *v, const QString &title, const QString &desc, QWidget *control)
{
	auto *row = new QWidget;
	auto *rv = new QVBoxLayout(row);
	rv->setContentsMargins(0, 0, 0, 12);
	rv->setSpacing(4);
	auto *t = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
	rv->addWidget(t);
	if (!desc.isEmpty()) {
		auto *d = new QLabel(desc);
		d->setWordWrap(true);
		d->setStyleSheet(QStringLiteral("color:#8a8f98;"));
		rv->addWidget(d);
	}
	rv->addWidget(control);
	v->addWidget(row);
	return row;
}

// A slider with its live value spelled out beside it. A bare slider makes the
// user guess what "somewhere past the middle" means, and these settings (a
// padding percentage, a duration in ms) have numbers worth reading.
QWidget *makeSliderRow(QWidget *parent, QSlider *&slider, int min, int max, int value,
		       const QString &suffix, int step = 1)
{
	slider = new QSlider(Qt::Horizontal, parent);
	slider->setRange(min, max);
	slider->setSingleStep(step);
	slider->setPageStep(step * 5);
	slider->setValue(std::clamp(value, min, max));
	auto *row = new QWidget(parent);
	auto *lay = new QHBoxLayout(row);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(8);
	auto *label = new QLabel(QStringLiteral("%1%2").arg(slider->value()).arg(suffix), parent);
	label->setMinimumWidth(48);
	QObject::connect(slider, &QSlider::valueChanged, label, [label, suffix](int val) {
		label->setText(QStringLiteral("%1%2").arg(val).arg(suffix));
	});
	lay->addWidget(slider, 1);
	lay->addWidget(label);
	return row;
}

// A color setting on one compact row: [ bold title ]  [ small swatch ].
// Keeps the swatch small (not full-width) and aligns swatches across fields.
QWidget *addColorField(QVBoxLayout *v, const QString &title, QPushButton *swatch)
{
	auto *row = new QWidget;
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 8);
	h->setSpacing(10);
	auto *t = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
	t->setMinimumWidth(130); // aligns the three swatches on the Mouse page
	swatch->setFixedWidth(130);
	h->addWidget(t);
	h->addWidget(swatch);
	h->addStretch(1);
	v->addWidget(row);
	return row;
}

// A checkbox setting: the checkbox is its own label; the description sits beneath.
QWidget *addCheck(QVBoxLayout *v, QCheckBox *check, const QString &desc)
{
	auto *row = new QWidget;
	auto *rv = new QVBoxLayout(row);
	rv->setContentsMargins(0, 0, 0, 12);
	rv->setSpacing(4);
	rv->addWidget(check);
	if (!desc.isEmpty()) {
		auto *d = new QLabel(desc);
		d->setWordWrap(true);
		d->setStyleSheet(QStringLiteral("color:#8a8f98; margin-left:22px;"));
		rv->addWidget(d);
	}
	v->addWidget(row);
	return row;
}

// Settings that only mean something while `parent` is ticked: hidden while it
// is not, rather than greyed.
//
// Greying was the old behaviour and the argument for it was that a greyed
// setting can still be read before you decide to turn the feature on. In
// practice it made every page look like a wall of dead controls -- the Zoom
// page is six sliders and a shortcut, all inert, under one checkbox. Hiding
// them means the page is the size of what it currently does.
void dependsOn(QCheckBox *parent, const QVector<QWidget *> &rows)
{
	const auto apply = [parent, rows]() {
		const bool on = parent->isChecked();
		for (QWidget *row : rows)
			if (row)
				row->setVisible(on);
	};
	QObject::connect(parent, &QCheckBox::toggled, parent, apply);
	apply();
}

// Wrap a control + trailing "Browse…" button into a single row widget.
QWidget *folderRowWidget(QLineEdit *edit, QPushButton *browse)
{
	auto *w = new QWidget;
	auto *h = new QHBoxLayout(w);
	h->setContentsMargins(0, 0, 0, 0);
	h->addWidget(edit, 1);
	h->addWidget(browse);
	return w;
}

} // namespace

PresetEditorDialog::PresetEditorDialog(const Preset &preset, AudioManager &audio, QWidget *parent)
	: QDialog(parent), result_(preset), original_(preset)
{
	setWindowTitle(QStringLiteral("Preset Settings"));
	setModal(true);

	// ---- Header: preset name (applies to every page) --------------------
	nameEdit_ = new QLineEdit(QString::fromStdString(preset.name), this);
	nameEdit_->setPlaceholderText(QStringLiteral("Preset name"));
	QFont nameFont = nameEdit_->font();
	nameFont.setPointSize(nameFont.pointSize() + 2);
	nameEdit_->setFont(nameFont);

	auto *header = new QVBoxLayout;
	auto *nameCaption = new QLabel(QStringLiteral("<b>Preset Name</b>"), this);
	header->addWidget(nameCaption);
	header->addWidget(nameEdit_);

	// ---- Left navigation + stacked pages --------------------------------
	nav_ = new QListWidget(this);
	nav_->setFixedWidth(150);
	nav_->setSpacing(2);
	auto *stack = new QStackedWidget(this);

	// Pages are collected first and added at the end, because whether a page
	// belongs in this dialog depends on what the preset records: an Audio Only
	// preset has no picture, so Video, Mouse, Spotlight, Zoom, Webcam and
	// Advanced are settings it can never use. They were fully editable before,
	// and everything set in them was silently ignored at record time.
	//
	// The capability table answers each one, so the dialog and the recorder
	// cannot disagree about which of them apply.
	mode_ = recordModeFromInt(preset.captureMode);
	QVector<QPair<QString, QWidget *>> pages;
	auto addPage = [&pages](const QString &title, QWidget *page) {
		pages.append({title, page});
	};

	QVBoxLayout *v = nullptr; // filled per page by makePage()

	// ===== Recording (everything about how a take begins and ends) =====
	// This was two pages. "General" held a static note about resolution and the
	// idle auto-pause; both are behaviours around recording, and a page with
	// one setting on it is a page you have to visit to find out it is empty.
	QWidget *recordingPage = makePage(v);
	// The display to record is chosen on the main window (next to Capture),
	// not per preset.
	auto *resNote = new QLabel(
		QStringLiteral("The whole display, or the chosen region, at native resolution."), this);
	resNote->setWordWrap(true);
	resNote->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	addField(v, QStringLiteral("Resolution"), QString(), resNote);

	countdownCombo_ = new QComboBox(this);
	countdownCombo_->addItem(QStringLiteral("Disabled"), 0);
	for (int s = 1; s <= 10; ++s)
		countdownCombo_->addItem(QStringLiteral("%1 second%2").arg(s).arg(s == 1 ? "" : "s"), s);
	{
		int ci = countdownCombo_->findData(preset.countdownSeconds);
		countdownCombo_->setCurrentIndex(ci >= 0 ? ci : 0);
	}
	// The main window has this too, and neither used to mention the other, so
	// the same setting in two places read as two settings that disagreed.
	addField(v, QStringLiteral("Countdown before recording"),
		 QStringLiteral("A countdown first, never recorded. Also on the main window."),
		 countdownCombo_);

	idleSpin_ = new QSpinBox(this);
	idleSpin_->setRange(0, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	idleSpin_->setValue(preset.idleTimeoutSeconds);
	addField(v, QStringLiteral("Auto-pause after idle"),
		 QStringLiteral("Pauses after this long with no input. Also on the main window."),
		 idleSpin_);

	minLengthSpin_ = new QSpinBox(this);
	minLengthSpin_->setRange(0, 3600);
	minLengthSpin_->setSuffix(QStringLiteral(" s"));
	minLengthSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	minLengthSpin_->setValue(preset.minRecordingSeconds);
	addField(v, QStringLiteral("Minimum recording length"),
		 QStringLiteral("Shorter recordings ask before saving. Catches accidental clips."),
		 minLengthSpin_);

	// Focus auto-pause now lives on the main window's "Record only one
	// application" toggle (pick the app there and it pauses when that app isn't
	// focused). The old per-preset checkbox was redundant, so it's removed here.

	borderCheck_ = new QCheckBox(QStringLiteral("Show border around recorded screen"), this);
	borderCheck_->setChecked(preset.showScreenBorder);
	QWidget *borderCheckRow =
		addCheck(v, borderCheck_,
			 QStringLiteral("Outlines the recorded monitor. Kept out of the video."));

	borderColor_ = QColor(QString::fromStdString(preset.screenBorderColor));
	if (!borderColor_.isValid())
		borderColor_ = QColor(0xe5, 0x48, 0x4d);
	borderColorBtn_ = new QPushButton(this);
	setButtonColor(borderColorBtn_, borderColor_);
	connect(borderColorBtn_, &QPushButton::clicked, this,
		[this]() { pickColor(borderColor_, borderColorBtn_); });
	QWidget *borderColorRow = addColorField(v, QStringLiteral("Border color"), borderColorBtn_);

	borderThicknessSpin_ = new QSpinBox(this);
	borderThicknessSpin_->setRange(1, 10);
	borderThicknessSpin_->setSuffix(QStringLiteral(" px"));
	borderThicknessSpin_->setValue(preset.screenBorderThickness > 0 ? preset.screenBorderThickness : 4);
	QWidget *borderThickRow =
		addField(v, QStringLiteral("Border thickness"), QString(), borderThicknessSpin_);
	dependsOn(borderCheck_, {borderColorRow, borderThickRow});
	// The border is drawn around a whole display, so it means nothing when the
	// recording is a region -- and nothing at all with no picture.
	if (!modeSupportsScreenBorder(mode_)) {
		borderCheckRow->hide();
		borderColorRow->hide();
		borderThickRow->hide();
	}

	regionHandleCheck_ = new QCheckBox(QStringLiteral("Show a drag handle on the capture region"), this);
	regionHandleCheck_->setChecked(preset.regionMoveHandle);
	QWidget *regionHandleRow =
		addCheck(v, regionHandleCheck_,
			 QStringLiteral("A grab tab above the frame. Drag it to move the region."));
	if (!modeUsesRegion(mode_))
		regionHandleRow->hide();

	v->addStretch(1);
	addPage(QStringLiteral("Recording"), recordingPage);

	// ===== Video =====
	QWidget *videoPage = makePage(v);
	formatCombo_ = new QComboBox(this);
	formatCombo_->addItem(QStringLiteral("MP4"), int(RecordingFormat::MP4));
	formatCombo_->addItem(QStringLiteral("MKV"), int(RecordingFormat::MKV));
	formatCombo_->addItem(QStringLiteral("MOV"), int(RecordingFormat::MOV));
	formatCombo_->addItem(QStringLiteral("AVI"), int(RecordingFormat::AVI));
	formatCombo_->addItem(QStringLiteral("GIF"), int(RecordingFormat::GIF));
	formatCombo_->setCurrentIndex(formatCombo_->findData(int(preset.format)));
	addField(v, QStringLiteral("Format"),
		 QStringLiteral("MP4 plays everywhere. MKV survives crashes. GIF is short and silent."),
		 formatCombo_);

	codecCombo_ = new QComboBox(this);
	for (VideoCodec c : EncoderFactory::availableCodecs(/*gpuOnly=*/false))
		codecCombo_->addItem(QString::fromUtf8(codecLabel(c)), int(c));
	int codecIdx = codecCombo_->findData(int(preset.codec));
	if (codecIdx < 0) {
		// Keep a saved codec this machine can't encode right now visible and
		// selected — falling back to index 0 would silently overwrite the
		// preset's stored choice as soon as the dialog is saved.
		codecCombo_->addItem(QString::fromUtf8(codecLabel(preset.codec)) +
					     QStringLiteral(" (not available on this PC)"),
				     int(preset.codec));
		codecIdx = codecCombo_->count() - 1;
	}
	codecCombo_->setCurrentIndex(codecIdx);
	codecRow_ = addField(
		v, QStringLiteral("Codec"),
		QStringLiteral("H.264 plays everywhere. HEVC and AV1 are smaller, need newer players."),
		codecCombo_);

	fpsCombo_ = new QComboBox(this);
	fpsCombo_->setEditable(true);
	fpsCombo_->setValidator(new QIntValidator(1, 240, fpsCombo_));
	for (int val : {24, 30, 60, 120})
		fpsCombo_->addItem(QString::number(val));
	fpsCombo_->setCurrentText(QString::number(preset.fps));
	addField(v, QStringLiteral("Frame rate (fps)"),
		 QStringLiteral("30 suits most screen capture. 60 is smoother, and larger."),
		 fpsCombo_);

	gpuCheck_ = new QCheckBox(QStringLiteral("Use GPU compression while recording"), this);
	gpuCheck_->setChecked(preset.gpuCompression);
	addCheck(v, gpuCheck_,
		 QStringLiteral("Encodes on the graphics card. Much lighter on the system."));

	validationLabel_ = new QLabel(this);
	validationLabel_->setWordWrap(true);
	validationLabel_->setStyleSheet(QStringLiteral("color:#d29922;"));
	v->addWidget(validationLabel_);
	v->addStretch(1);
	addPage(QStringLiteral("Video"), videoPage);

	// ===== Audio =====
	QWidget *audioPage = makePage(v);
	// Visible only while the format is GIF, which has no audio track at all --
	// otherwise this page is a set of live-looking controls that do nothing.
	audioGifNote_ =
		new QLabel(QStringLiteral("GIF has no audio track. These settings apply elsewhere."), this);
	audioGifNote_->setWordWrap(true);
	audioGifNote_->setStyleSheet(QStringLiteral("color:#d29922;"));
	audioGifNote_->setVisible(false);
	v->addWidget(audioGifNote_);
	// The main window's audio panel, not a second implementation of it. It
	// carries the volume sliders the preset already stored but had nowhere to
	// edit, and live level bars -- which is the only way to answer "is this the
	// right microphone?" without recording something first.
	//
	// It drives the live capture as you tick things, exactly as it does on the
	// main window. The caller re-asserts the active preset's audio afterwards,
	// so Cancel puts the sound back the way it was.
	audioSection_ = new QWidget(this);
	auto *audioLayout = new QVBoxLayout(audioSection_);
	audioLayout->setContentsMargins(0, 0, 0, 12);
	audioLayout->setSpacing(6);
	audioPanel_ = new AudioPanel(audio, audioSection_);
	audioPanel_->load(preset.recordDesktopAudio, preset.micDeviceIds, preset.desktopVolume,
			  preset.micVolumes);
	audioLayout->addWidget(audioPanel_);

	auto *rescanBtn = new QPushButton(QStringLiteral("Rescan devices"), this);
	rescanBtn->setToolTip(QStringLiteral("Look for microphones plugged in since this opened."));
	connect(rescanBtn, &QPushButton::clicked, this, [this]() { audioPanel_->rescanDevices(); });
	auto *rescanRow = new QHBoxLayout;
	rescanRow->setContentsMargins(0, 0, 0, 0);
	rescanRow->addWidget(rescanBtn);
	rescanRow->addStretch(1);
	audioLayout->addLayout(rescanRow);
	v->addWidget(audioSection_);

	// The bars only move while something is polling them.
	auto *meterTimer = new QTimer(this);
	meterTimer->setInterval(100);
	connect(meterTimer, &QTimer::timeout, audioPanel_, &AudioPanel::updateMeters);
	meterTimer->start();

	audioBitrateCombo_ = new QComboBox(this);
	for (int kbps : {96, 128, 160, 192, 256, 320})
		audioBitrateCombo_->addItem(QStringLiteral("%1 kbps").arg(kbps), kbps);
	{
		const int saved = preset.audioBitrateKbps > 0 ? preset.audioBitrateKbps : 160;
		int bi = audioBitrateCombo_->findData(saved);
		if (bi < 0) {
			audioBitrateCombo_->addItem(QStringLiteral("%1 kbps").arg(saved), saved);
			bi = audioBitrateCombo_->count() - 1;
		}
		audioBitrateCombo_->setCurrentIndex(bi);
	}
	audioBitrateRow_ = addField(v, QStringLiteral("Audio quality"),
				    QStringLiteral("160 kbps is transparent for voice. Go higher for music."),
				    audioBitrateCombo_);

	v->addStretch(1);
	addPage(QStringLiteral("Audio"), audioPage);

	// ===== Output =====
	QWidget *outputPage = makePage(v);
	folderEdit_ = new QLineEdit(QString::fromStdString(preset.outputFolder), this);
	folderEdit_->setPlaceholderText(QStringLiteral("Choose a folder for recordings…"));
	auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browse, &QPushButton::clicked, this, &PresetEditorDialog::browseFolder);
	addField(v, QStringLiteral("Output folder"),
		 QStringLiteral("Where finished recordings are saved."),
		 folderRowWidget(folderEdit_, browse));

	templateEdit_ = new QLineEdit(QString::fromStdString(preset.filenameTemplate), this);
	templateEdit_->setPlaceholderText(
		QStringLiteral("Recording_{Year}-{Month}-{Day}_{Hour}-{Minute}-{Second}"));
	addField(v, QStringLiteral("Filename template"),
		 QStringLiteral("Names each file. Click a token to insert it."),
		 templateEdit_);

	// Quick-insert token buttons — no need to remember token names.
	struct Tok {
		const char *label;
		const char *token;
	};
	const Tok toks[] = {
		{"Year", "{Year}"},         {"Month", "{Month}"},   {"Month (Jul)", "{MonthShort}"},
		{"Month (July)", "{MonthLong}"}, {"Day", "{Day}"},  {"Hour", "{Hour}"},
		{"Minute", "{Minute}"},     {"Second", "{Second}"}, {"Counter", "{Counter}"},
		{"Preset", "{Preset}"},     {"Resolution", "{Resolution}"}, {"FPS", "{FPS}"},
		{"Codec", "{Codec}"},       {"Random", "{Random}"},
	};
	auto *tokGridHost = new QWidget(this);
	auto *tokGrid = new QGridLayout(tokGridHost);
	tokGrid->setContentsMargins(0, 0, 0, 0);
	tokGrid->setSpacing(4);
	int col = 0, rowN = 0;
	for (const Tok &t : toks) {
		auto *b = new QPushButton(QString::fromUtf8(t.label), this);
		b->setToolTip(QString::fromUtf8(t.token));
		const QString token = QString::fromUtf8(t.token);
		connect(b, &QPushButton::clicked, this, [this, token]() {
			templateEdit_->insert(token); // inserts at the cursor
			templateEdit_->setFocus();
			updateFilenamePreview();
		});
		tokGrid->addWidget(b, rowN, col);
		if (++col == 4) {
			col = 0;
			++rowN;
		}
	}
	v->addWidget(tokGridHost);
	v->addSpacing(8);

	templatePreview_ = new QLabel(this);
	templatePreview_->setWordWrap(true);
	templatePreview_->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	v->addWidget(templatePreview_);

	driveLinkEdit_ = new QLineEdit(QString::fromStdString(preset.googleDriveLink), this);
	driveLinkEdit_->setPlaceholderText(QStringLiteral("https://drive.google.com/…"));
	addField(v, QStringLiteral("Google Drive share link"),
		 QStringLiteral("Optional. Adds a Drive shortcut to the main window."),
		 driveLinkEdit_);

	connect(templateEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(nameEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(fpsCombo_, &QComboBox::currentTextChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(codecCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(formatCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(folderEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::updateFilenamePreview);

	v->addStretch(1);
	addPage(QStringLiteral("Output"), outputPage);

	// ===== Mouse =====
	QWidget *mousePage = makePage(v);
	mouseCursorCheck_ = new QCheckBox(QStringLiteral("Show mouse cursor"), this);
	mouseCursorCheck_->setChecked(preset.showMouseCursor);
	addCheck(v, mouseCursorCheck_,
		 QStringLiteral("Includes the cursor in the recording."));

	mouseAreaCheck_ = new QCheckBox(QStringLiteral("Highlight around cursor"), this);
	mouseAreaCheck_->setChecked(preset.showMouseArea);
	addCheck(v, mouseAreaCheck_,
		 QStringLiteral("A soft coloured ring follows the cursor."));

	highlightColor_ = QColor(QString::fromStdString(preset.mouseHighlightColor));
	if (!highlightColor_.isValid())
		highlightColor_ = QColor(0xff, 0xd5, 0x4a);
	highlightColorBtn_ = new QPushButton(this);
	setButtonColor(highlightColorBtn_, highlightColor_);
	connect(highlightColorBtn_, &QPushButton::clicked, this,
		[this]() { pickColor(highlightColor_, highlightColorBtn_); });
	QWidget *highlightColorRow = addColorField(v, QStringLiteral("Highlight color"), highlightColorBtn_);

	highlightSizeSlider_ = new QSlider(Qt::Horizontal, this);
	highlightSizeSlider_->setRange(10, 200);
	highlightSizeSlider_->setValue(preset.mouseHighlightSize > 0 ? preset.mouseHighlightSize : 60);
	// Show the exact pixel value beside the slider.
	auto *sizeRow = new QWidget(this);
	auto *sizeLayout = new QHBoxLayout(sizeRow);
	sizeLayout->setContentsMargins(0, 0, 0, 0);
	sizeLayout->setSpacing(8);
	auto *sizeValue = new QLabel(QStringLiteral("%1 px").arg(highlightSizeSlider_->value()), this);
	sizeValue->setMinimumWidth(48);
	connect(highlightSizeSlider_, &QSlider::valueChanged, sizeValue,
		[sizeValue](int px) { sizeValue->setText(QStringLiteral("%1 px").arg(px)); });
	sizeLayout->addWidget(highlightSizeSlider_, 1);
	sizeLayout->addWidget(sizeValue);
	QWidget *highlightSizeRow = addField(v, QStringLiteral("Highlight size"),
					     QStringLiteral("Diameter of the ring, in pixels."), sizeRow);
	dependsOn(mouseAreaCheck_, {highlightColorRow, highlightSizeRow});

	mouseClicksCheck_ = new QCheckBox(QStringLiteral("Show click animations"), this);
	mouseClicksCheck_->setChecked(preset.recordMouseClicks);
	addCheck(v, mouseClicksCheck_,
		 QStringLiteral("Ripples where you click, in the colours below."));

	leftColor_ = QColor(QString::fromStdString(preset.leftClickColor));
	if (!leftColor_.isValid())
		leftColor_ = QColor(0x4a, 0x90, 0xe2);
	leftColorBtn_ = new QPushButton(this);
	setButtonColor(leftColorBtn_, leftColor_);
	connect(leftColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(leftColor_, leftColorBtn_); });
	QWidget *leftColorRow = addColorField(v, QStringLiteral("Left click color"), leftColorBtn_);

	rightColor_ = QColor(QString::fromStdString(preset.rightClickColor));
	if (!rightColor_.isValid())
		rightColor_ = QColor(0xe2, 0x53, 0x4a);
	rightColorBtn_ = new QPushButton(this);
	setButtonColor(rightColorBtn_, rightColor_);
	connect(rightColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(rightColor_, rightColorBtn_); });
	QWidget *rightColorRow = addColorField(v, QStringLiteral("Right click color"), rightColorBtn_);
	dependsOn(mouseClicksCheck_, {leftColorRow, rightColorRow});

	// ---- Follow Mouse ----
	followCheck_ = new QCheckBox(QStringLiteral("Follow Mouse"), this);
	followCheck_->setChecked(preset.followMouse);
	QWidget *followCheckRow = addCheck(v, followCheck_,
		 QStringLiteral("The region pans to keep the cursor framed. Its size never changes."));

	followProfileCombo_ = new QComboBox(this);
	followProfileCombo_->addItem(QStringLiteral("Instant"));        // 0
	followProfileCombo_->addItem(QStringLiteral("Smooth"));         // 1
	followProfileCombo_->addItem(QStringLiteral("Cinematic"));      // 2
	followProfileCombo_->addItem(QStringLiteral("Mobile Tutorial")); // 3
	followProfileCombo_->addItem(QStringLiteral("Custom"));         // 4
	followProfileCombo_->setCurrentIndex(std::clamp(preset.followProfile, 0, 4));
	QWidget *followProfileRow = addField(v, QStringLiteral("Follow profile"),
					     QStringLiteral("Sets both sliders below. Move one and this becomes Custom."),
					     followProfileCombo_);

	// The two sliders, each with the live value label beside it.
	const auto sliderRow = [this](QSlider *&slider, int min, int max, int value, const QString &suffix) {
		return makeSliderRow(this, slider, min, max, value, suffix);
	};

	QWidget *padRow = sliderRow(followPaddingSlider_, 0, 45, preset.followPaddingPct, QStringLiteral("%"));
	QWidget *followPadRow = addField(
		v, QStringLiteral("Mouse padding"),
		QStringLiteral("How far the cursor wanders before the region starts following."), padRow);

	QWidget *smoothRow = sliderRow(followSmoothSlider_, 0, 100, preset.followSmoothness, QString());
	QWidget *followSmoothRow =
		addField(v, QStringLiteral("Smoothness"),
			 QStringLiteral("0 reacts instantly. Higher glides — slower and steadier."),
			 smoothRow);

	followAxisCombo_ = new QComboBox(this);
	followAxisCombo_->addItem(QStringLiteral("Both directions"));  // 0
	followAxisCombo_->addItem(QStringLiteral("Horizontal only")); // 1
	followAxisCombo_->addItem(QStringLiteral("Vertical only"));   // 2
	followAxisCombo_->setCurrentIndex(std::clamp(preset.followAxis, 0, 2));
	QWidget *followAxisRow = addField(v, QStringLiteral("Follow direction"),
					  QStringLiteral("Lock an axis so it only slides one way."),
					  followAxisCombo_);

	followShortcutEdit_ = new QKeySequenceEdit(
		QKeySequence(QString::fromStdString(preset.followShortcut)), this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	followShortcutEdit_->setMaximumSequenceLength(1);
#endif
	QWidget *followShortcutRow =
		addField(v, QStringLiteral("Follow shortcut"),
			 QStringLiteral("Parks the camera mid-recording. Switching off glides back."),
			 followShortcutEdit_);
	dependsOn(followCheck_, {followProfileRow, followPadRow, followSmoothRow, followAxisRow,
				 followShortcutRow});
	// Only a region has anywhere to pan to. Previously this was a greyed
	// checkbox with the reason on a tooltip -- which is a tooltip nobody hovers.
	if (!modeSupportsFollowMouse(mode_)) {
		followCheckRow->hide();
		followProfileRow->hide();
		followPadRow->hide();
		followSmoothRow->hide();
		followAxisRow->hide();
		followShortcutRow->hide();
	}

	// Profile -> sliders. Values chosen so the names mean what they say:
	// Instant snaps, Cinematic trails on a long leash, Mobile Tutorial keeps a
	// tall strip steady with a generous safe zone.
	connect(followProfileCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
		[this](int idx) {
			struct P {
				int pad, smooth;
			};
			static constexpr P kProfiles[] = {{10, 0}, {20, 50}, {30, 85}, {25, 65}};
			if (idx < 0 || idx > 3)
				return; // Custom: leave the sliders as the user set them
			followProfileApplying_ = true;
			followPaddingSlider_->setValue(kProfiles[idx].pad);
			followSmoothSlider_->setValue(kProfiles[idx].smooth);
			followProfileApplying_ = false;
		});
	// Sliders -> Custom, but only when the USER drags them -- the combo writing
	// its own profile values must not immediately un-select itself.
	const auto toCustom = [this]() {
		if (!followProfileApplying_)
			followProfileCombo_->setCurrentIndex(4);
	};
	connect(followPaddingSlider_, &QSlider::valueChanged, this, toCustom);
	connect(followSmoothSlider_, &QSlider::valueChanged, this, toCustom);

	mousePreview_ = new MousePreview(this);
	addField(v, QStringLiteral("Preview"), QString(), mousePreview_);
	connect(mouseAreaCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(mouseClicksCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(highlightSizeSlider_, &QSlider::valueChanged, this, &PresetEditorDialog::updateMousePreview);
	v->addStretch(1);
	addPage(QStringLiteral("Mouse"), mousePage);

	// ===== Spotlight =====
	// The recorded display's width, for the preview's scaling. Resolved here
	// rather than in the preview so the maths lives with the preset that knows
	// which monitor it records.
	{
		const QList<QScreen *> screens = QGuiApplication::screens();
		if (!screens.isEmpty()) {
			const int idx = std::clamp(preset.monitorIndex, 0, int(screens.size()) - 1);
			const QScreen *sc = screens.at(idx);
			spotScreenW_ = std::max(320, int(sc->geometry().width() * sc->devicePixelRatio()));
		}
	}
	// Everything but a patch around the cursor goes dark. Drawn by the same
	// desktop overlay as the cursor highlight, which is what puts it in the
	// recording -- and also means your own screen really does go dark while it
	// is lit. That is a feature: you see what the viewer sees.
	QWidget *spotPage = makePage(v);
	spotCheck_ = new QCheckBox(QStringLiteral("Spotlight"), this);
	spotCheck_->setChecked(preset.spotlightEnabled);
	addCheck(v, spotCheck_,
		 QStringLiteral("Darkens everything but an area around the mouse. Your screen too."));

	spotShortcutEdit_ = new QKeySequenceEdit(
		QKeySequence(QString::fromStdString(preset.spotlightShortcut)), this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	spotShortcutEdit_->setMaximumSequenceLength(1);
#endif
	QWidget *spotShortcutRow =
		addField(v, QStringLiteral("Spotlight shortcut"),
			 QStringLiteral("Press to turn on, again to turn off. Works system-wide."),
			 spotShortcutEdit_);

	QSlider *spotSize = nullptr;
	QWidget *spotSizeRow = makeSliderRow(this, spotSize, SpotlightParams::kMinSize,
					     SpotlightParams::kMaxSize, preset.spotlightSize,
					     QStringLiteral(" px"), 20);
	spotSizeSlider_ = spotSize;
	QWidget *spotSizeField = addField(v, QStringLiteral("Area size"),
					  QStringLiteral("Width of the lit area, in screen pixels."),
					  spotSizeRow);

	QSlider *spotDark = nullptr;
	QWidget *spotDarkRow = makeSliderRow(this, spotDark, 0, SpotlightParams::kMaxDark,
					     preset.spotlightDarkPct, QStringLiteral("%"), 5);
	spotDarkSlider_ = spotDark;
	QWidget *spotDarkField = addField(v, QStringLiteral("Dark area opacity"),
					  QStringLiteral("How dark the rest goes. Never fully black, deliberately."),
					  spotDarkRow);

	QSlider *spotRound = nullptr;
	QWidget *spotRoundRow = makeSliderRow(this, spotRound, 0, 100, preset.spotlightRoundness,
					      QStringLiteral("%"), 5);
	spotRoundSlider_ = spotRound;
	QWidget *spotRoundField = addField(v, QStringLiteral("Area roundness"),
					   QStringLiteral("0% is a rectangle, 100% a circle."),
					   spotRoundRow);

	spotStartOnCheck_ = new QCheckBox(QStringLiteral("Start recordings with the spotlight on"), this);
	spotStartOnCheck_->setChecked(preset.spotlightStartOn);
	QWidget *spotStartRow =
		addCheck(v, spotStartOnCheck_,
			 QStringLiteral("Otherwise recordings start normal and wait for the shortcut."));

	spotPreview_ = new SpotlightPreview(this);
	QWidget *spotPreviewRow = addField(v, QStringLiteral("Preview"),
					   QStringLiteral("Move the mouse here to see it follow."),
					   spotPreview_);
	// The preview goes with the rest: with the feature off there is nothing to
	// preview, and a live spotlight demo under an unticked box invites the
	// question of whether it is on.
	dependsOn(spotCheck_, {spotShortcutRow, spotSizeField, spotDarkField, spotRoundField,
			       spotStartRow, spotPreviewRow});

	// Every setting on this page feeds the preview, and the preview is the
	// reason the page is usable at all — the numbers alone say very little
	// about what 70% dark or 40% round looks like.
	const auto spotChanged = [this]() { syncSpotlightPreview(); };
	connect(spotCheck_, &QCheckBox::toggled, this, spotChanged);
	connect(spotSizeSlider_, &QSlider::valueChanged, this, spotChanged);
	connect(spotDarkSlider_, &QSlider::valueChanged, this, spotChanged);
	connect(spotRoundSlider_, &QSlider::valueChanged, this, spotChanged);
	syncSpotlightPreview();

	v->addStretch(1);
	addPage(QStringLiteral("Spotlight"), spotPage);

	// ===== Zoom =====
	// Press a key mid-recording and the picture pushes in on the cursor; press
	// it again and it pulls back out. Works in every capture mode: it scales
	// the captured picture inside whatever canvas the recording has, so it does
	// not care whether that canvas is a display or a region.
	QWidget *zoomPage = makePage(v);
	zoomCheck_ = new QCheckBox(QStringLiteral("Automatic Zoom"), this);
	zoomCheck_->setChecked(preset.zoomEnabled);
	addCheck(v, zoomCheck_,
		 QStringLiteral("A shortcut zooms in on the mouse, and back out again."));

	zoomShortcutEdit_ = new QKeySequenceEdit(
		QKeySequence(QString::fromStdString(preset.zoomShortcut)), this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	// One chord: RegisterHotKey knows nothing about sequences.
	zoomShortcutEdit_->setMaximumSequenceLength(1);
#endif
	QWidget *zoomShortcutRow =
		addField(v, QStringLiteral("Zoom shortcut"),
			 QStringLiteral("Press to zoom in, again to zoom out. Works system-wide."),
			 zoomShortcutEdit_);

	QSlider *zoomPercentSlider = nullptr;
	QWidget *zoomPctRow = makeSliderRow(this, zoomPercentSlider, ZoomParams::kMinPercent,
					    ZoomParams::kMaxPercent, preset.zoomPercent,
					    QStringLiteral("%"), 5);
	zoomPercentSlider_ = zoomPercentSlider;
	QWidget *zoomPctField =
		addField(v, QStringLiteral("Zoom percentage"),
			 QStringLiteral("200% shows half the screen, twice the size. Past 300% softens."),
			 zoomPctRow);

	QSlider *zoomAnimSlider = nullptr;
	QWidget *zoomAnimRow = makeSliderRow(this, zoomAnimSlider, 0, 1500, preset.zoomAnimMs,
					     QStringLiteral(" ms"), 50);
	zoomAnimSlider_ = zoomAnimSlider;
	QWidget *zoomAnimField = addField(v, QStringLiteral("Zoom animation speed"),
					  QStringLiteral("How long the push-in takes. 0 cuts straight there."),
					  zoomAnimRow);

	QSlider *zoomSmoothSlider = nullptr;
	QWidget *zoomSmoothRow = makeSliderRow(this, zoomSmoothSlider, 0, 100,
					       preset.zoomFollowSmoothness, QString());
	zoomFollowSmoothSlider_ = zoomSmoothSlider;
	QWidget *zoomSmoothField = addField(v, QStringLiteral("Mouse follow speed"),
					    QStringLiteral("How the zoomed frame chases the cursor. 0 is glued."),
					    zoomSmoothRow);

	QSlider *zoomPadSlider = nullptr;
	QWidget *zoomPadRow = makeSliderRow(this, zoomPadSlider, 0, 45, preset.zoomFollowPaddingPct,
					    QStringLiteral("%"), 5);
	zoomFollowPadSlider_ = zoomPadSlider;
	QWidget *zoomPadField = addField(v, QStringLiteral("Mouse padding"),
					 QStringLiteral("How far the cursor wanders before the frame follows."),
					 zoomPadRow);

	zoomFollowAxisCombo_ = new QComboBox(this);
	zoomFollowAxisCombo_->addItem(QStringLiteral("Both directions")); // 0
	zoomFollowAxisCombo_->addItem(QStringLiteral("Horizontal only")); // 1
	zoomFollowAxisCombo_->addItem(QStringLiteral("Vertical only"));   // 2
	zoomFollowAxisCombo_->setCurrentIndex(std::clamp(preset.zoomFollowAxis, 0, 2));
	QWidget *zoomAxisField = addField(v, QStringLiteral("Mouse follow mode"),
					  QStringLiteral("Lock an axis so the zoom only slides one way."),
					  zoomFollowAxisCombo_);

	dependsOn(zoomCheck_, {zoomShortcutRow, zoomPctField, zoomAnimField, zoomSmoothField,
			       zoomPadField, zoomAxisField});

	v->addStretch(1);
	addPage(QStringLiteral("Zoom"), zoomPage);

	// ===== Webcam (recorded as a separate synchronized file) =====
	QWidget *webcamPage = makePage(v);
	webcamCheck_ = new QCheckBox(QStringLiteral("Record webcam as a separate video file"), this);
	webcamCheck_->setChecked(preset.webcamEnabled);
	addCheck(v, webcamCheck_,
		 QStringLiteral("Saves the camera to its own file, kept in sync."));

	const std::vector<AudioDevice> cams = WebcamRecorder::cameras();
	webcamDeviceCombo_ = new QComboBox(this);
	for (const AudioDevice &cam : cams)
		webcamDeviceCombo_->addItem(QString::fromStdString(cam.name), QString::fromStdString(cam.id));
	if (webcamDeviceCombo_->count() == 0)
		webcamDeviceCombo_->addItem(QStringLiteral("(no camera detected)"), QString());
	{
		// Keep a saved camera that isn't connected right now selected instead
		// of silently switching (and re-saving) the first list entry.
		const QString savedId = QString::fromStdString(preset.webcamDeviceId);
		int di = webcamDeviceCombo_->findData(savedId);
		if (di < 0 && !savedId.isEmpty()) {
			webcamDeviceCombo_->addItem(QStringLiteral("(saved camera — not connected)"), savedId);
			di = webcamDeviceCombo_->count() - 1;
		}
		if (di >= 0)
			webcamDeviceCombo_->setCurrentIndex(di);
	}
	QWidget *webcamDeviceRow = addField(v, QStringLiteral("Camera"),
					    QStringLiteral("Which webcam to record."), webcamDeviceCombo_);

	// Diagnostic status line: explain an empty list (plugin missing / privacy /
	// not connected) or confirm success.
	auto *camStatus = new QLabel(this);
	camStatus->setWordWrap(true);
	QString statusText;
	QString statusColor;
	if (!WebcamRecorder::supported()) {
		statusText = QStringLiteral(
			"Webcam capture is not available in this build. Install the Visual Studio "
			"<a style=\"color:#f0a0a3;\" "
			"href=\"https://learn.microsoft.com/cpp/build/vscpp-step-0-installation\">"
			"\"C++ ATL for latest v143 build tools\"</a> component and rebuild to enable it.");
		statusColor = QStringLiteral("#e5484d");
	} else if (!cams.empty()) {
		statusText = cams.size() == 1
				     ? QStringLiteral("Camera detected: %1")
					       .arg(QString::fromStdString(cams.front().name))
				     : QStringLiteral("%1 cameras detected.").arg(cams.size());
		statusColor = QStringLiteral("#3fb950");
	} else if (cameraAccessStatus() == CameraAccess::DeniedByPrivacy) {
		statusText = QStringLiteral(
			"No cameras detected. This may be because camera access is disabled for desktop "
			"applications. Check your Windows Privacy & Security > Camera settings and ensure "
			"\"Let desktop apps access your camera\" is enabled.");
		statusColor = QStringLiteral("#e5484d");
	} else {
		statusText = QStringLiteral(
			"No cameras detected. Make sure a camera is connected and not already in use by "
			"another application. If it still doesn't appear, check Windows Privacy & Security "
			"> Camera settings and ensure desktop apps are allowed to access the camera.");
		statusColor = QStringLiteral("#d29922");
	}
	camStatus->setText(statusText);
	camStatus->setStyleSheet(QStringLiteral("color:%1;").arg(statusColor));
	camStatus->setOpenExternalLinks(true); // the ATL install link opens in the browser
	camStatus->setTextInteractionFlags(Qt::TextBrowserInteraction);
	v->addWidget(camStatus);
	v->addSpacing(8);

	webcamResCombo_ = new QComboBox(this);
	for (const char *r : {"1920x1080", "1280x720", "640x480"})
		webcamResCombo_->addItem(QString::fromUtf8(r), QString::fromUtf8(r));
	{
		const QString wres = QStringLiteral("%1x%2").arg(preset.webcamWidth).arg(preset.webcamHeight);
		int ri = webcamResCombo_->findData(wres);
		if (ri < 0) {
			webcamResCombo_->addItem(wres, wres);
			ri = webcamResCombo_->count() - 1;
		}
		webcamResCombo_->setCurrentIndex(ri);
	}
	QWidget *webcamResRow =
		addField(v, QStringLiteral("Webcam resolution"),
			 QStringLiteral("Capture size of the camera file. Independent of the screen."),
			 webcamResCombo_);

	webcamFpsCombo_ = new QComboBox(this);
	webcamFpsCombo_->setEditable(true);
	webcamFpsCombo_->setValidator(new QIntValidator(1, 240, webcamFpsCombo_));
	for (int val : {24, 30, 60})
		webcamFpsCombo_->addItem(QString::number(val));
	webcamFpsCombo_->setCurrentText(QString::number(preset.webcamFps > 0 ? preset.webcamFps : 30));
	QWidget *webcamFpsRow = addField(v, QStringLiteral("Webcam frame rate"),
					 QStringLiteral("Frames per second for the camera file."),
					 webcamFpsCombo_);

	webcamCustomFolderCheck_ = new QCheckBox(QStringLiteral("Use a custom folder for the webcam file"), this);
	webcamCustomFolderCheck_->setChecked(preset.webcamUseCustomFolder);
	QWidget *webcamFolderCheckRow =
		addCheck(v, webcamCustomFolderCheck_,
			 QStringLiteral("Send the camera file somewhere other than beside the recording."));

	webcamFolderEdit_ = new QLineEdit(QString::fromStdString(preset.webcamFolder), this);
	webcamFolderEdit_->setPlaceholderText(QStringLiteral("Same folder as the screen recording"));
	auto *wcBrowse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(wcBrowse, &QPushButton::clicked, this, [this]() {
		const QString dir =
			QFileDialog::getExistingDirectory(this, QStringLiteral("Webcam output folder"),
							  webcamFolderEdit_->text());
		if (!dir.isEmpty())
			webcamFolderEdit_->setText(dir);
	});
	QWidget *webcamFolderRow = addField(v, QStringLiteral("Webcam folder"), QString(),
					    folderRowWidget(webcamFolderEdit_, wcBrowse));

	// Two levels deep: the folder picker needs the webcam on AND a custom
	// folder asked for, so it cannot go through dependsOn() alone -- that
	// watches one box, and the outer one would show the picker again the
	// moment the webcam was switched back on.
	const auto syncWebcamRows = [this, webcamDeviceRow, webcamResRow, webcamFpsRow,
				     webcamFolderCheckRow, webcamFolderRow]() {
		const bool on = webcamCheck_->isChecked();
		webcamDeviceRow->setVisible(on);
		webcamResRow->setVisible(on);
		webcamFpsRow->setVisible(on);
		webcamFolderCheckRow->setVisible(on);
		webcamFolderRow->setVisible(on && webcamCustomFolderCheck_->isChecked());
	};
	connect(webcamCheck_, &QCheckBox::toggled, this, syncWebcamRows);
	connect(webcamCustomFolderCheck_, &QCheckBox::toggled, this, syncWebcamRows);
	syncWebcamRows();
	v->addStretch(1);
	addPage(QStringLiteral("Webcam"), webcamPage);

	// ===== Hotkeys =====
	// App-wide, not per preset: which key starts a recording is a property of
	// the keyboard in front of the user, not of the format being recorded.
	// Stored in QSettings; the main window re-registers on save.
	QWidget *hotkeysPage = makePage(v);
	{
		// The one page in a per-preset dialog whose settings are not per
		// preset. It says so in a caption at the bottom already; the caption
		// is easy to miss when the two fields above it look exactly like the
		// forty other fields in this dialog.
		auto *banner = new QLabel(QStringLiteral("These keys belong to the app, not to this "
							 "preset — changing them changes them everywhere."),
					  this);
		banner->setWordWrap(true);
		banner->setStyleSheet(QStringLiteral("color:#d29922; border:1px solid #4a3f1e; "
						     "background:#2b2415; border-radius:4px; padding:6px;"));
		v->addWidget(banner);
		v->addSpacing(10);
		QSettings hk(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		recordKeyEdit_ = new QKeySequenceEdit(
			QKeySequence(hk.value(QStringLiteral("hotkeys/record"), QStringLiteral("F9"))
					     .toString()),
			this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		// One chord only -- RegisterHotKey knows no sequences. Older Qt lacks
		// the setter; the mapping refuses multi-chord input regardless.
		recordKeyEdit_->setMaximumSequenceLength(1);
#endif
		addField(v, QStringLiteral("Start / stop recording"),
			 QStringLiteral("Works system-wide on Windows. Harpia does not need focus."),
			 recordKeyEdit_);
		pauseKeyEdit_ = new QKeySequenceEdit(
			QKeySequence(hk.value(QStringLiteral("hotkeys/pause"), QStringLiteral("F10"))
					     .toString()),
			this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		pauseKeyEdit_->setMaximumSequenceLength(1);
#endif
		addField(v, QStringLiteral("Pause / resume"),
			 QStringLiteral("Same rules. GIF recordings cannot pause at all."),
			 pauseKeyEdit_);
	}
	v->addStretch(1);
	addPage(QStringLiteral("Hotkeys"), hotkeysPage);

	// ===== Advanced =====
	QWidget *advancedPage = makePage(v);
	frameRateModeCombo_ = new QComboBox(this);
	frameRateModeCombo_->addItem(QStringLiteral("Constant (CFR)"), int(FrameRateMode::CFR));
	frameRateModeCombo_->addItem(QStringLiteral("Variable (VFR)"), int(FrameRateMode::VFR));
	frameRateModeCombo_->setCurrentIndex(frameRateModeCombo_->findData(int(preset.frameRateMode)));
	frameRateModeRow_ = addField(
		v, QStringLiteral("Frame rate mode"),
		QStringLiteral("CFR is safest for editing. VFR saves space, some editors struggle."),
		frameRateModeCombo_);

	// Bitrate: a dropdown of common presets, plus Auto and a Custom escape hatch
	// that reveals the numeric field below.
	bitrateCombo_ = new QComboBox(this);
	bitrateCombo_->addItem(QStringLiteral("Auto (Recommended)"), 0); // 0 = let the encoder decide
	for (int kbps : {4000, 6000, 8000, 10000, 12000, 16000, 20000, 30000, 40000, 50000, 80000, 100000})
		bitrateCombo_->addItem(QStringLiteral("%L1 kbps").arg(kbps), kbps);
	bitrateCombo_->addItem(QStringLiteral("Custom…"), -1);

	bitrateSpin_ = new QSpinBox(this);
	bitrateSpin_->setRange(500, 500000);
	bitrateSpin_->setSingleStep(500);
	bitrateSpin_->setSuffix(QStringLiteral(" kbps"));
	bitrateSpin_->setValue(preset.videoBitrateKbps > 0 ? preset.videoBitrateKbps : 12000);

	// Seed the dropdown from the stored value: 0 = Auto, a listed value selects
	// it, anything else becomes Custom with the numeric field showing the value.
	if (preset.videoBitrateKbps <= 0) {
		bitrateCombo_->setCurrentIndex(0);
	} else {
		const int idx = bitrateCombo_->findData(preset.videoBitrateKbps);
		if (idx >= 0)
			bitrateCombo_->setCurrentIndex(idx);
		else
			bitrateCombo_->setCurrentIndex(bitrateCombo_->count() - 1); // Custom…
	}

	// Also consults the format: GIF hides the whole bitrate row, and the
	// Custom… spin box must not reappear underneath a hidden dropdown.
	auto syncBitrate = [this]() {
		const bool gif = RecordingFormat(formatCombo_->currentData().toInt()) == RecordingFormat::GIF;
		bitrateSpin_->setVisible(!gif && bitrateCombo_->currentData().toInt() == -1);
	};
	connect(bitrateCombo_, &QComboBox::currentIndexChanged, this, syncBitrate);

	bitrateRow_ = addField(v, QStringLiteral("Bitrate"),
			       QStringLiteral("Higher means better quality and bigger files. Auto picks sensibly."),
			       bitrateCombo_);
	v->addWidget(bitrateSpin_);
	v->addSpacing(12);
	syncBitrate();
	v->addStretch(1);
	addPage(QStringLiteral("Advanced"), advancedPage);

	// ---- Which pages this preset actually has ---------------------------
	const auto pageApplies = [this](const QString &title) {
		if (title == QStringLiteral("Video"))
			return modeHasVideo(mode_);
		if (title == QStringLiteral("Advanced"))
			return modeUsesVideoEncoder(mode_);
		if (title == QStringLiteral("Mouse"))
			return modeSupportsMouseFx(mode_);
		if (title == QStringLiteral("Spotlight"))
			return modeSupportsSpotlight(mode_);
		if (title == QStringLiteral("Zoom"))
			return modeSupportsZoom(mode_);
		if (title == QStringLiteral("Webcam"))
			return modeSupportsWebcam(mode_);
		return true; // Recording, Audio, Output, Hotkeys apply to everything
	};

	int hidden = 0;
	for (const auto &entry : pages) {
		if (pageApplies(entry.first)) {
			// A thin unselectable gap above Hotkeys, so the app-wide page
			// reads as a different kind of thing rather than the last item
			// in a list of preset settings.
			if (entry.first == QStringLiteral("Hotkeys")) {
				auto *gap = new QListWidgetItem;
				gap->setFlags(Qt::NoItemFlags);
				gap->setSizeHint(QSize(0, 10));
				nav_->addItem(gap);
			}
			const int pageIndex = stack->addWidget(entry.second);
			auto *item = new QListWidgetItem(entry.first);
			// Which page this row shows, because the separators mean the
			// row number and the page number no longer agree.
			item->setData(Qt::UserRole, pageIndex);
			nav_->addItem(item);
		} else {
			// Still built, still parented, just not reachable: every widget
			// on it keeps the preset's stored value, and accept() writes it
			// back untouched. Switching the capture mode back brings the page
			// and its settings back exactly as they were.
			++hidden;
			entry.second->setParent(this);
			entry.second->hide();
		}
	}

	// ---- Search: every setting, by anything written on it ----------------
	// Eleven pages and forty-odd settings, and the only way to find one was to
	// remember which page it lived on. The text comes from the rows themselves
	// -- their labels, descriptions and checkbox captions -- so a setting is
	// findable by its description as well as its title, and nothing has to be
	// kept in step by hand.
	for (int i = 0; i < nav_->count(); ++i) {
		QListWidgetItem *navItem = nav_->item(i);
		if (!navItem->data(Qt::UserRole).isValid())
			continue; // a separator
		auto *page = qobject_cast<QScrollArea *>(stack->widget(navItem->data(Qt::UserRole).toInt()));
		if (!page || !page->widget())
			continue;
		for (QObject *child : page->widget()->children()) {
			auto *row = qobject_cast<QWidget *>(child);
			if (!row)
				continue;
			QString text = navItem->text();
			for (const QLabel *l : row->findChildren<QLabel *>())
				text += QLatin1Char(' ') + l->text();
			for (const QAbstractButton *b : row->findChildren<QAbstractButton *>())
				text += QLatin1Char(' ') + b->text();
			if (auto *asLabel = qobject_cast<QLabel *>(row))
				text += QLatin1Char(' ') + asLabel->text();
			searchRows_.push_back({row, i, text.toLower()});
		}
	}

	searchEdit_ = new QLineEdit(this);
	searchEdit_->setPlaceholderText(QStringLiteral("Search settings…"));
	searchEdit_->setClearButtonEnabled(true);
	searchEdit_->setFixedWidth(nav_->width());
	connect(searchEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::applySearch);

	// ---- Assemble: header on top, nav | pages, buttons at the bottom ----
	connect(nav_, &QListWidget::currentItemChanged, this,
		[stack](QListWidgetItem *cur, QListWidgetItem *) {
			if (cur && cur->data(Qt::UserRole).isValid())
				stack->setCurrentIndex(cur->data(Qt::UserRole).toInt());
		});
	nav_->setCurrentRow(0);

	auto *navColumn = new QVBoxLayout;
	navColumn->setContentsMargins(0, 0, 0, 0);
	navColumn->addWidget(searchEdit_);
	navColumn->addWidget(nav_, 1);
	if (hidden > 0) {
		// Said out loud, because a page that is simply gone reads as a bug --
		// and the setting that brings it back is on the main window, not here.
		auto *note = new QLabel(QStringLiteral("Some pages don't apply to %1.")
						.arg(recordModeLabel(mode_)),
					this);
		note->setWordWrap(true);
		note->setStyleSheet(QStringLiteral("color:#8a8f98; font-size:11px;"));
		note->setFixedWidth(nav_->width());
		navColumn->addWidget(note);
	}

	auto *body = new QHBoxLayout;
	body->addLayout(navColumn);
	body->addWidget(stack, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &PresetEditorDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	// Enter in a line edit (name, filename template, editable fps combos) must
	// not trigger Save-and-close mid-edit — no default button in this dialog.
	for (QAbstractButton *b : buttons->buttons()) {
		if (auto *pb = qobject_cast<QPushButton *>(b)) {
			pb->setAutoDefault(false);
			pb->setDefault(false);
		}
	}

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(header);
	layout->addLayout(body, 1);
	layout->addWidget(buttons);

	connect(formatCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateValidation);
	connect(codecCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateValidation);
	updateValidation();
	updateMousePreview();
	updateFilenamePreview();

	// Reopen where it was left. The size mattered more once pages started
	// hiding what does not apply: the window was sized for the fullest page
	// this dialog ever had, and most of them no longer fill it.
	{
		QSettings ui(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const QSize saved = ui.value(QStringLiteral("presetEditor/size")).toSize();
		resize(saved.isValid() && saved.width() > 300 && saved.height() > 300 ? saved
										     : QSize(640, 580));
		const QString page = ui.value(QStringLiteral("presetEditor/page")).toString();
		if (!page.isEmpty())
			showPage(page); // no-op if that page does not apply to this preset
	}
}

void PresetEditorDialog::applySearch(const QString &needle)
{
	const QString n = needle.trimmed().toLower();

	// Tinted rather than hidden. Visibility on these rows already has two
	// owners -- the capability table and the depends-on-a-checkbox rule -- and
	// a third would fight them: a search that "revealed" a Zoom slider while
	// Zoom is switched off would be showing a control that does nothing.
	int firstHit = -1;
	QSet<int> pagesWithHits;
	for (const SearchRow &r : searchRows_) {
		const bool hit = !n.isEmpty() && r.text.contains(n);
		r.row->setStyleSheet(hit ? QStringLiteral("background:#2a3550; border-radius:4px;")
					 : QString());
		if (hit) {
			pagesWithHits.insert(r.navIndex);
			if (firstHit < 0)
				firstHit = r.navIndex;
		}
	}

	for (int i = 0; i < nav_->count(); ++i) {
		QListWidgetItem *item = nav_->item(i);
		if (!item->data(Qt::UserRole).isValid()) {
			item->setHidden(!n.isEmpty()); // the Hotkeys gap, while filtering
			continue;
		}
		item->setHidden(!n.isEmpty() && !pagesWithHits.contains(i));
	}

	// Land on the first page that has a match, so one keystroke gets you there.
	if (firstHit >= 0 && nav_->currentRow() != firstHit)
		nav_->setCurrentRow(firstHit);
}

void PresetEditorDialog::showPage(const QString &title)
{
	if (!nav_)
		return;
	for (int i = 0; i < nav_->count(); ++i) {
		// Separator rows have no text and cannot be selected, so a lookup that
		// matched one would leave the dialog showing nothing.
		if (nav_->item(i)->data(Qt::UserRole).isValid() && nav_->item(i)->text() == title) {
			nav_->setCurrentRow(i);
			return;
		}
	}
}

void PresetEditorDialog::pickColor(QColor &target, QPushButton *button)
{
	const QColor c = QColorDialog::getColor(target, this, QStringLiteral("Choose color"));
	if (c.isValid()) {
		target = c;
		setButtonColor(button, c);
		updateMousePreview();
	}
}

void PresetEditorDialog::syncSpotlightPreview()
{
	if (!spotPreview_)
		return;
	SpotlightParams sp;
	sp.sizePx = spotSizeSlider_->value();
	sp.darkPct = spotDarkSlider_->value();
	sp.roundnessPct = spotRoundSlider_->value();
	// Shown against the display this preset actually records, so "320 px" reads
	// as the fraction of the screen it will really be -- the same number is a
	// large patch on 1080p and a small one on 4K.
	spotPreview_->configure(sp, spotScreenW_);

}

void PresetEditorDialog::updateMousePreview()
{
	if (!mousePreview_)
		return;
	mousePreview_->configure(mouseAreaCheck_->isChecked(), highlightColor_, highlightSizeSlider_->value(),
				 mouseClicksCheck_->isChecked(), leftColor_, rightColor_);
}

void PresetEditorDialog::updateFilenamePreview()
{
	if (!templatePreview_)
		return;

	FileNameTemplate t;
	std::map<std::string, std::string> vars;
	vars["Preset"] = nameEdit_->text().trimmed().toStdString();
	vars["Resolution"] = "1920x1080"; // sample; the real size is known at record time
	QString fpsText = fpsCombo_->currentText().trimmed();
	if (fpsText.isEmpty())
		fpsText = QStringLiteral("30");
	vars["FPS"] = (fpsText + QStringLiteral("fps")).toStdString();
	vars["Codec"] = QString::fromUtf8(codecToString(VideoCodec(codecCombo_->currentData().toInt())))
				.toUpper()
				.toStdString();
	vars["Counter"] =
		QStringLiteral("%1").arg(result_.recordingCounter, 4, 10, QLatin1Char('0')).toStdString();

	const std::string base = t.expand(templateEdit_->text().toStdString(), vars);
	const auto fmt = RecordingFormat(formatCombo_->currentData().toInt());
	const QString ext = QString::fromUtf8(formatExtension(fmt));
	// Show the full destination path (middle-elided) so the folder is visible
	// too, not just the file name.
	QString full = QStringLiteral("%1.%2").arg(QString::fromStdString(base), ext);
	const QString folder = folderEdit_->text().trimmed();
	if (!folder.isEmpty())
		full = QDir::toNativeSeparators(QDir(folder).filePath(full));
	const QFontMetrics fm(templatePreview_->font());
	templatePreview_->setText(
		QStringLiteral("Preview: %1").arg(fm.elidedText(full, Qt::ElideMiddle, 460)));
	templatePreview_->setToolTip(full);
}

void PresetEditorDialog::browseFolder()
{
	const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose output folder"),
							      folderEdit_->text());
	if (!dir.isEmpty())
		folderEdit_->setText(dir);
}

void PresetEditorDialog::updateValidation()
{
	const auto format = RecordingFormat(formatCombo_->currentData().toInt());
	const auto codec = VideoCodec(codecCombo_->currentData().toInt());

	// GIF quietly overrides half this dialog, so the dialog stops offering the
	// parts it overrides. Hidden rather than greyed, which is what every other
	// inapplicable setting here now does -- and a page of live-looking controls
	// that silently do nothing is how "my settings don't work" reports happen.
	// The notes below still name each override, so the reason is on screen.
	const bool isGif = (format == RecordingFormat::GIF);
	const auto showRow = [](QWidget *row, bool on) {
		if (row)
			row->setVisible(on);
	};
	showRow(codecRow_, !isGif);
	showRow(bitrateRow_, !isGif);
	showRow(frameRateModeRow_, !isGif);
	bitrateSpin_->setVisible(!isGif && bitrateCombo_->currentData().toInt() == -1);
	showRow(audioSection_, !isGif);
	showRow(audioBitrateRow_, !isGif);
	if (audioGifNote_)
		audioGifNote_->setVisible(isGif);

	QString msg;
	if (isGif) {
		msg = QStringLiteral("GIF ignores codec and bitrate, caps at 15 fps, and cannot pause.");
	} else if (format == RecordingFormat::AVI && codec != VideoCodec::H264) {
		msg = QStringLiteral("AVI has poor support for HEVC/AV1 — MP4 or MKV is recommended.");
	} else if (format == RecordingFormat::MP4 && codec == VideoCodec::HEVC) {
		msg = QStringLiteral("HEVC in MP4 may not play everywhere. MKV is safest.");
	}
	validationLabel_->setText(msg);
	validationLabel_->setVisible(!msg.isEmpty());
}

bool PresetEditorDialog::resolveShortcutConflicts()
{
	struct Row {
		const char *id;
		const char *label;
		QKeySequenceEdit *edit;
	};
	const Row rows[] = {
		{"record", "Start / stop recording", recordKeyEdit_},
		{"pause", "Pause / resume", pauseKeyEdit_},
		{"zoom", "Zoom in / out", zoomShortcutEdit_},
		{"spotlight", "Spotlight on / off", spotShortcutEdit_},
		{"follow", "Follow Mouse on / off", followShortcutEdit_},
	};

	QVector<ShortcutBinding> bindings;
	for (const Row &r : rows) {
		if (!r.edit)
			continue;
		bindings.append({QString::fromLatin1(r.id), QString::fromLatin1(r.label),
				 r.edit->keySequence()});
	}
	if (!hasShortcutConflict(bindings))
		return true;

	ShortcutConflictDialog dlg(bindings, QSet<QString>(), this);
	if (dlg.exec() != QDialog::Accepted)
		return false; // cancelled: nothing saved, nothing changed

	// Write the resolution back into the editors rather than straight into the
	// result, so what the user ends up with is visible on the page they came
	// from -- and so a later validation failure does not silently discard it.
	const QVector<ShortcutBinding> fixed = dlg.result();
	for (const ShortcutBinding &b : fixed)
		for (const Row &r : rows)
			if (r.edit && b.id == QLatin1String(r.id))
				r.edit->setKeySequence(b.key);
	return true;
}

void PresetEditorDialog::collectInto(Preset &out) const
{
	// Every widget on every page, written into `out`. Split out of accept()
	// so Cancel can build the same thing and compare: the only honest way to
	// answer "did anything change?" is to collect it and look.
	out.name = nameEdit_->text().trimmed().toStdString();
	out.format = RecordingFormat(formatCombo_->currentData().toInt());
	out.codec = VideoCodec(codecCombo_->currentData().toInt());
	out.frameRateMode = FrameRateMode(frameRateModeCombo_->currentData().toInt());
	out.fps = qMax(1, fpsCombo_->currentText().toInt());
	{
		const int sel = bitrateCombo_->currentData().toInt();
		if (sel == 0)
			out.videoBitrateKbps = 0; // Auto
		else if (sel == -1)
			out.videoBitrateKbps = bitrateSpin_->value(); // Custom
		else
			out.videoBitrateKbps = sel; // a listed preset
	}

	// Hotkeys are app-wide QSettings, written on OK like everything else here
	// (Cancel leaves them untouched). Empty means "back to the default".
	{
		QSettings hk(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
		const QString rec = recordKeyEdit_->keySequence().toString();
		const QString pause = pauseKeyEdit_->keySequence().toString();
		hk.setValue(QStringLiteral("hotkeys/record"),
			    rec.isEmpty() ? QStringLiteral("F9") : rec);
		hk.setValue(QStringLiteral("hotkeys/pause"),
			    pause.isEmpty() ? QStringLiteral("F10") : pause);
	}

	out.outputFolder = folderEdit_->text().trimmed().toStdString();
	// monitorIndex and captureMode are intentionally not edited here — the main
	// window owns both (its display and capture combos write through).
	out.gpuCompression = gpuCheck_->isChecked();
	out.idleTimeoutSeconds = idleSpin_->value();
	out.countdownSeconds = countdownCombo_->currentData().toInt();
	out.minRecordingSeconds = minLengthSpin_->value();
	out.showScreenBorder = borderCheck_->isChecked();
	out.screenBorderColor = borderColor_.name().toStdString();
	out.screenBorderThickness = borderThicknessSpin_->value();
	out.regionMoveHandle = regionHandleCheck_->isChecked();
	out.filenameTemplate = templateEdit_->text().trimmed().toStdString();
	// An empty template would expand to an extension-only (hidden) filename
	// like ".mp4" — restore the default naming instead of saving it.
	if (out.filenameTemplate.empty())
		out.filenameTemplate = Preset::makeDefault("").filenameTemplate;

	out.googleDriveLink = driveLinkEdit_->text().trimmed().toStdString();

	out.recordDesktopAudio = audioPanel_->desktopOn();
	out.audioBitrateKbps = audioBitrateCombo_->currentData().toInt();
	// Volumes come back too. They were stored in the preset all along and could
	// only be changed from the main window, so opening this dialog and saving
	// used to be able to leave them behind.
	out.micDeviceIds = audioPanel_->enabledMicIds();
	out.desktopVolume = audioPanel_->desktopVolume();
	out.micVolumes = audioPanel_->micVolumes();

	out.showMouseCursor = mouseCursorCheck_->isChecked();
	out.showMouseArea = mouseAreaCheck_->isChecked();
	out.mouseHighlightColor = highlightColor_.name().toStdString();
	out.mouseHighlightSize = highlightSizeSlider_->value();
	out.recordMouseClicks = mouseClicksCheck_->isChecked();
	out.leftClickColor = leftColor_.name().toStdString();
	out.rightClickColor = rightColor_.name().toStdString();
	out.followMouse = followCheck_->isChecked();
	out.followPaddingPct = followPaddingSlider_->value();
	out.followSmoothness = followSmoothSlider_->value();
	out.followAxis = followAxisCombo_->currentIndex();
	out.followProfile = followProfileCombo_->currentIndex();
	out.followShortcut = followShortcutEdit_->keySequence().toString().toStdString();

	out.spotlightEnabled = spotCheck_->isChecked();
	out.spotlightStartOn = spotStartOnCheck_->isChecked();
	out.spotlightSize = spotSizeSlider_->value();
	out.spotlightDarkPct = spotDarkSlider_->value();
	out.spotlightRoundness = spotRoundSlider_->value();
	// Same rule as the zoom key: an empty sequence means "no shortcut", not
	// "put the default back".
	out.spotlightShortcut = spotShortcutEdit_->keySequence().toString().toStdString();

	out.zoomEnabled = zoomCheck_->isChecked();
	out.zoomPercent = zoomPercentSlider_->value();
	out.zoomAnimMs = zoomAnimSlider_->value();
	out.zoomFollowSmoothness = zoomFollowSmoothSlider_->value();
	out.zoomFollowPaddingPct = zoomFollowPadSlider_->value();
	out.zoomFollowAxis = zoomFollowAxisCombo_->currentIndex();
	// An empty shortcut means "no zoom hotkey" rather than a broken one -- the
	// main window simply registers nothing, and the feature is unreachable
	// until a key is set. Better than silently reinstating the default the user
	// just cleared.
	out.zoomShortcut = zoomShortcutEdit_->keySequence().toString().toStdString();

	out.webcamEnabled = webcamCheck_->isChecked();
	out.webcamDeviceId = webcamDeviceCombo_->currentData().toString().toStdString();
	{
		const QStringList wh = webcamResCombo_->currentData().toString().split(QLatin1Char('x'));
		out.webcamWidth = wh.value(0).toInt();
		out.webcamHeight = wh.value(1).toInt();
	}
	out.webcamFps = qMax(1, webcamFpsCombo_->currentText().toInt());
	out.webcamUseCustomFolder = webcamCustomFolderCheck_->isChecked();
	out.webcamFolder = webcamFolderEdit_->text().trimmed().toStdString();
}

// Both ways out remember the window, so it does not matter which was pressed.
void PresetEditorDialog::rememberGeometry() const
{
	QSettings ui(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
	ui.setValue(QStringLiteral("presetEditor/size"), size());
	if (QListWidgetItem *cur = nav_ ? nav_->currentItem() : nullptr)
		if (cur->data(Qt::UserRole).isValid())
			ui.setValue(QStringLiteral("presetEditor/page"), cur->text());
}

void PresetEditorDialog::reject()
{
	rememberGeometry();
	// Cancel used to throw the whole edit away without a word -- the only
	// destructive action in the app that did not ask. It still does not ask
	// when there is nothing to lose, which is most of the time: the dialog is
	// opened to look at something far more often than to change it.
	Preset edited = original_;
	collectInto(edited);
	if (edited != original_) {
		const auto btn = QMessageBox::question(
			this, windowTitle(),
			QStringLiteral("Discard the changes to this preset?"),
			QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
		if (btn != QMessageBox::Discard)
			return;
	}
	QDialog::reject();
}

void PresetEditorDialog::accept()
{
	rememberGeometry();
	const QString name = nameEdit_->text().trimmed();
	if (name.isEmpty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Please enter a preset name."));
		return;
	}
	if (folderEdit_->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, windowTitle(), QStringLiteral("Please choose an output folder."));
		return;
	}
	// "Custom folder" ticked with no folder chosen used to fall back to the
	// screen folder SILENTLY -- the checkbox said one thing and the files did
	// another. Make it a decision here instead of a surprise later.
	if (webcamCheck_ && webcamCheck_->isChecked() && webcamCustomFolderCheck_ &&
	    webcamCustomFolderCheck_->isChecked() && webcamFolderEdit_->text().trimmed().isEmpty()) {
		QMessageBox::warning(this, windowTitle(),
				     QStringLiteral("Webcam \"custom folder\" is ticked but no folder is "
						    "chosen.\n\nPick a folder, or untick it to save the "
						    "webcam beside the screen recording."));
		return;
	}
	// Four keyboard shortcuts are reachable from this dialog -- Start/stop and
	// Pause from the Hotkeys page, Zoom and Spotlight from their own -- and two
	// of them landing on the same key is the sort of thing nothing ever reports:
	// the second feature just never fires. So it is settled here, before
	// anything is written, and there is no way through that leaves a duplicate.
	//
	// Checked whether or not the feature is switched ON. A key stored against a
	// disabled Zoom is not live today, but the moment the box is ticked it is --
	// and being told about the clash at that point, in a different session, with
	// no idea which of the two was set first, is exactly the confusion this is
	// meant to prevent.
	if (!resolveShortcutConflicts())
		return;
	{
		// Catch a typo'd folder here instead of at record time.
		const QString folder = folderEdit_->text().trimmed();
		if (!QDir(folder).exists()) {
			const auto btn = QMessageBox::question(
				this, windowTitle(),
				QStringLiteral("The output folder does not exist:\n%1\n\nCreate it?").arg(folder),
				QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
			if (btn != QMessageBox::Yes)
				return;
			if (!QDir().mkpath(folder)) {
				QMessageBox::warning(this, windowTitle(),
						     QStringLiteral("Could not create the folder."));
				return;
			}
		}
	}

	collectInto(result_);

	QDialog::accept();
}

} // namespace harpia
