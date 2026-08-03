#include "PresetEditorDialog.hpp"

#include "MousePreview.hpp"
#include "ShortcutConflictDialog.hpp"
#include "SpotlightPreview.hpp"
#include "core/CaptureManager.hpp"
#include "core/EncoderFactory.hpp"
#include "core/WebcamRecorder.hpp"
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

// A titled setting: bold label, a small gray description, then the control.
// PowerRec/VS-style — every setting explains itself in one line.
void addField(QVBoxLayout *v, const QString &title, const QString &desc, QWidget *control)
{
	auto *t = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
	v->addWidget(t);
	if (!desc.isEmpty()) {
		auto *d = new QLabel(desc);
		d->setWordWrap(true);
		d->setStyleSheet(QStringLiteral("color:#8a8f98;"));
		v->addWidget(d);
	}
	v->addWidget(control);
	v->addSpacing(12);
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
void addColorField(QVBoxLayout *v, const QString &title, QPushButton *swatch)
{
	auto *row = new QWidget;
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(10);
	auto *t = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
	t->setMinimumWidth(130); // aligns the three swatches on the Mouse page
	swatch->setFixedWidth(130);
	h->addWidget(t);
	h->addWidget(swatch);
	h->addStretch(1);
	v->addWidget(row);
	v->addSpacing(8);
}

// A checkbox setting: the checkbox is its own label; the description sits beneath.
void addCheck(QVBoxLayout *v, QCheckBox *check, const QString &desc)
{
	v->addWidget(check);
	if (!desc.isEmpty()) {
		auto *d = new QLabel(desc);
		d->setWordWrap(true);
		d->setStyleSheet(QStringLiteral("color:#8a8f98; margin-left:22px;"));
		v->addWidget(d);
	}
	v->addSpacing(12);
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

PresetEditorDialog::PresetEditorDialog(const Preset &preset, QWidget *parent)
	: QDialog(parent), result_(preset)
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

	auto addPage = [&](const QString &title, QWidget *page) {
		nav_->addItem(title);
		stack->addWidget(page);
	};

	QVBoxLayout *v = nullptr; // filled per page by makePage()

	// ===== General =====
	QWidget *generalPage = makePage(v);
	// The display to record is chosen on the main window (next to Capture),
	// not per preset.
	auto *resNote = new QLabel(QStringLiteral("Recordings use the full display — or the selected "
						  "capture region — at its native resolution."),
				   this);
	resNote->setWordWrap(true);
	resNote->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	addField(v, QStringLiteral("Resolution"), QString(), resNote);

	idleSpin_ = new QSpinBox(this);
	idleSpin_->setRange(0, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	idleSpin_->setValue(preset.idleTimeoutSeconds);
	addField(v, QStringLiteral("Auto-pause after idle"),
		 QStringLiteral("Pause recording automatically after this many seconds of no "
				"mouse/keyboard activity. Set to Disabled to always keep recording."),
		 idleSpin_);

	v->addStretch(1);
	addPage(QStringLiteral("General"), generalPage);

	// ===== Recording (behavior around start/stop) =====
	QWidget *recordingPage = makePage(v);
	countdownCombo_ = new QComboBox(this);
	countdownCombo_->addItem(QStringLiteral("Disabled"), 0);
	for (int s = 1; s <= 10; ++s)
		countdownCombo_->addItem(QStringLiteral("%1 second%2").arg(s).arg(s == 1 ? "" : "s"), s);
	{
		int ci = countdownCombo_->findData(preset.countdownSeconds);
		countdownCombo_->setCurrentIndex(ci >= 0 ? ci : 0);
	}
	addField(v, QStringLiteral("Countdown before recording"),
		 QStringLiteral("Show a large countdown on screen before recording starts, so you can get "
				"ready. The countdown itself is never part of the recording."),
		 countdownCombo_);

	minLengthSpin_ = new QSpinBox(this);
	minLengthSpin_->setRange(0, 3600);
	minLengthSpin_->setSuffix(QStringLiteral(" s"));
	minLengthSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	minLengthSpin_->setValue(preset.minRecordingSeconds);
	addField(v, QStringLiteral("Minimum recording length"),
		 QStringLiteral("If a finished recording is shorter than this (excluding paused time), you'll "
				"be asked whether to discard it — handy for throwing away accidental clips."),
		 minLengthSpin_);

	// Focus auto-pause now lives on the main window's "Record only one
	// application" toggle (pick the app there and it pauses when that app isn't
	// focused). The old per-preset checkbox was redundant, so it's removed here.

	borderCheck_ = new QCheckBox(QStringLiteral("Show border around recorded screen (Full Screen only)"),
				     this);
	borderCheck_->setChecked(preset.showScreenBorder);
	addCheck(v, borderCheck_,
		 QStringLiteral("Outline the recorded monitor while recording so you can see which screen is "
				"captured. The border is excluded from the video (Windows) and only appears in "
				"Full Screen capture mode."));

	borderColor_ = QColor(QString::fromStdString(preset.screenBorderColor));
	if (!borderColor_.isValid())
		borderColor_ = QColor(0xe5, 0x48, 0x4d);
	borderColorBtn_ = new QPushButton(this);
	setButtonColor(borderColorBtn_, borderColor_);
	connect(borderColorBtn_, &QPushButton::clicked, this,
		[this]() { pickColor(borderColor_, borderColorBtn_); });
	addColorField(v, QStringLiteral("Border color"), borderColorBtn_);

	borderThicknessSpin_ = new QSpinBox(this);
	borderThicknessSpin_->setRange(1, 10);
	borderThicknessSpin_->setSuffix(QStringLiteral(" px"));
	borderThicknessSpin_->setValue(preset.screenBorderThickness > 0 ? preset.screenBorderThickness : 4);
	addField(v, QStringLiteral("Border thickness"), QString(), borderThicknessSpin_);

	regionHandleCheck_ = new QCheckBox(QStringLiteral("Show a drag handle on the capture region "
							  "(Custom Region only)"),
					   this);
	regionHandleCheck_->setChecked(preset.regionMoveHandle);
	addCheck(v, regionHandleCheck_,
		 QStringLiteral("Adds a small grab tab just above the region frame. Drag it to move the "
				"region — including while another app is in front, or while recording, "
				"which dragging the middle of the frame cannot do. Only appears when the "
				"capture mode is Custom Region."));

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
		 QStringLiteral("Container/file type. MP4 is the most compatible; MKV survives crashes "
				"best; GIF makes a short silent animation."),
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
	addField(v, QStringLiteral("Codec"),
		 QStringLiteral("How video is compressed. H.264 plays everywhere; HEVC/AV1 give smaller "
				"files at the same quality but need newer players. Only codecs your PC "
				"can encode are listed."),
		 codecCombo_);

	fpsCombo_ = new QComboBox(this);
	fpsCombo_->setEditable(true);
	fpsCombo_->setValidator(new QIntValidator(1, 240, fpsCombo_));
	for (int val : {24, 30, 60, 120})
		fpsCombo_->addItem(QString::number(val));
	fpsCombo_->setCurrentText(QString::number(preset.fps));
	addField(v, QStringLiteral("Frame rate (fps)"),
		 QStringLiteral("Frames per second. 30 is fine for most screen capture; 60 is smoother "
				"for fast motion and games but makes larger files."),
		 fpsCombo_);

	gpuCheck_ = new QCheckBox(QStringLiteral("Use GPU compression while recording"), this);
	gpuCheck_->setChecked(preset.gpuCompression);
	addCheck(v, gpuCheck_,
		 QStringLiteral("Encode with the graphics card (NVENC/AMF/QSV) instead of the CPU. Much "
				"lighter on the system while recording; falls back to CPU if unavailable."));

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
	audioGifNote_ = new QLabel(QStringLiteral("GIF has no audio track — these settings apply to the "
						  "other formats."),
				   this);
	audioGifNote_->setWordWrap(true);
	audioGifNote_->setStyleSheet(QStringLiteral("color:#d29922;"));
	audioGifNote_->setVisible(false);
	v->addWidget(audioGifNote_);
	desktopAudioCheck_ = new QCheckBox(QStringLiteral("Record PC audio"), this);
	desktopAudioCheck_->setChecked(preset.recordDesktopAudio);
	addCheck(v, desktopAudioCheck_,
		 QStringLiteral("Capture the system/desktop sound — anything you hear from the speakers."));

	auto *micCaption = new QLabel(QStringLiteral("<b>Microphones</b>"), this);
	v->addWidget(micCaption);
	auto *micDesc = new QLabel(QStringLiteral("Tick each input device to mix into the recording. Each "
						 "is captured live so you can watch its level on the main window."),
				   this);
	micDesc->setWordWrap(true);
	micDesc->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	v->addWidget(micDesc);

	for (const AudioDevice &d : AudioManager::inputDevices()) {
		// Skip the synthetic "default" entry, matching the main window's
		// AudioPanel — a mic ticked here must be displayable there.
		if (d.id == "default")
			continue;
		auto *c = new QCheckBox(QString::fromStdString(d.name), this);
		const QString id = QString::fromStdString(d.id);
		c->setChecked(std::find(preset.micDeviceIds.begin(), preset.micDeviceIds.end(), d.id) !=
			      preset.micDeviceIds.end());
		micChecks_.append(c);
		micIds_.append(id);
		v->addWidget(c);
	}
	if (micChecks_.isEmpty()) {
		auto *none = new QLabel(QStringLiteral("(no microphones detected)"), this);
		none->setStyleSheet(QStringLiteral("color:#8a8f98;"));
		v->addWidget(none);
	}

	v->addSpacing(8);
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
	addField(v, QStringLiteral("Audio quality"),
		 QStringLiteral("Bitrate of the recorded audio track. 160 kbps is transparent for voice and "
				"desktop sound; go higher for music."),
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
		 QStringLiteral("Names each file. Click a token below to insert it at the cursor."),
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
		 QStringLiteral("Optional. When set, a clickable \"Google Drive\" shortcut appears at the "
				"bottom of the main window and opens this link."),
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
		 QStringLiteral("Include the cursor in the recording. Turn off for cursor-free captures."));

	mouseAreaCheck_ = new QCheckBox(QStringLiteral("Highlight around cursor"), this);
	mouseAreaCheck_->setChecked(preset.showMouseArea);
	addCheck(v, mouseAreaCheck_,
		 QStringLiteral("Draw a soft colored ring following the cursor so viewers can find it."));

	highlightColor_ = QColor(QString::fromStdString(preset.mouseHighlightColor));
	if (!highlightColor_.isValid())
		highlightColor_ = QColor(0xff, 0xd5, 0x4a);
	highlightColorBtn_ = new QPushButton(this);
	setButtonColor(highlightColorBtn_, highlightColor_);
	connect(highlightColorBtn_, &QPushButton::clicked, this,
		[this]() { pickColor(highlightColor_, highlightColorBtn_); });
	addColorField(v, QStringLiteral("Highlight color"), highlightColorBtn_);

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
	addField(v, QStringLiteral("Highlight size"),
		 QStringLiteral("Diameter of the cursor highlight ring, in pixels."), sizeRow);

	mouseClicksCheck_ = new QCheckBox(QStringLiteral("Show click animations"), this);
	mouseClicksCheck_->setChecked(preset.recordMouseClicks);
	addCheck(v, mouseClicksCheck_,
		 QStringLiteral("Ripple where you click — left and right buttons use the colors below."));

	leftColor_ = QColor(QString::fromStdString(preset.leftClickColor));
	if (!leftColor_.isValid())
		leftColor_ = QColor(0x4a, 0x90, 0xe2);
	leftColorBtn_ = new QPushButton(this);
	setButtonColor(leftColorBtn_, leftColor_);
	connect(leftColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(leftColor_, leftColorBtn_); });
	addColorField(v, QStringLiteral("Left click color"), leftColorBtn_);

	rightColor_ = QColor(QString::fromStdString(preset.rightClickColor));
	if (!rightColor_.isValid())
		rightColor_ = QColor(0xe2, 0x53, 0x4a);
	rightColorBtn_ = new QPushButton(this);
	setButtonColor(rightColorBtn_, rightColor_);
	connect(rightColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(rightColor_, rightColorBtn_); });
	addColorField(v, QStringLiteral("Right click color"), rightColorBtn_);

	// ---- Follow Mouse ----
	followCheck_ = new QCheckBox(QStringLiteral("Follow Mouse (Custom Region only)"), this);
	followCheck_->setChecked(preset.followMouse);
	addCheck(v, followCheck_,
		 QStringLiteral("While recording a Custom Region, the region pans to keep the cursor "
				"framed — record mobile-format tutorials without re-framing the video "
				"afterwards. The region's size never changes, only its position."));

	followProfileCombo_ = new QComboBox(this);
	followProfileCombo_->addItem(QStringLiteral("Instant"));        // 0
	followProfileCombo_->addItem(QStringLiteral("Smooth"));         // 1
	followProfileCombo_->addItem(QStringLiteral("Cinematic"));      // 2
	followProfileCombo_->addItem(QStringLiteral("Mobile Tutorial")); // 3
	followProfileCombo_->addItem(QStringLiteral("Custom"));         // 4
	followProfileCombo_->setCurrentIndex(std::clamp(preset.followProfile, 0, 4));
	addField(v, QStringLiteral("Follow profile"),
		 QStringLiteral("A starting point that sets the two sliders below together. Touch a "
				"slider and it becomes Custom."),
		 followProfileCombo_);

	// The two sliders, each with the live value label beside it.
	const auto sliderRow = [this](QSlider *&slider, int min, int max, int value, const QString &suffix) {
		return makeSliderRow(this, slider, min, max, value, suffix);
	};

	QWidget *padRow = sliderRow(followPaddingSlider_, 0, 45, preset.followPaddingPct, QStringLiteral("%"));
	addField(v, QStringLiteral("Mouse padding"),
		 QStringLiteral("The safe zone: how far the cursor can wander from the region's centre "
				"before it starts to follow, as a percentage of the region's size. The "
				"region holds still while the cursor stays inside it."),
		 padRow);

	QWidget *smoothRow = sliderRow(followSmoothSlider_, 0, 100, preset.followSmoothness, QString());
	addField(v, QStringLiteral("Smoothness"),
		 QStringLiteral("How the region catches up. 0 reacts instantly; higher values glide "
				"— slower, steadier, more cinematic."),
		 smoothRow);

	followAxisCombo_ = new QComboBox(this);
	followAxisCombo_->addItem(QStringLiteral("Both directions"));  // 0
	followAxisCombo_->addItem(QStringLiteral("Horizontal only")); // 1
	followAxisCombo_->addItem(QStringLiteral("Vertical only"));   // 2
	followAxisCombo_->setCurrentIndex(std::clamp(preset.followAxis, 0, 2));
	addField(v, QStringLiteral("Follow direction"),
		 QStringLiteral("Lock an axis: a vertical mobile strip usually only slides sideways; a "
				"full-width bar only follows up and down."),
		 followAxisCombo_);

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
		 QStringLiteral("Adds a shortcut that darkens everything except an area around the "
				"mouse, so a viewer's eye lands where you are pointing. Works in "
				"every capture mode. Your own screen darkens too — that is what "
				"gets recorded."));

	spotShortcutEdit_ = new QKeySequenceEdit(
		QKeySequence(QString::fromStdString(preset.spotlightShortcut)), this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	spotShortcutEdit_->setMaximumSequenceLength(1);
#endif
	addField(v, QStringLiteral("Spotlight shortcut"),
		 QStringLiteral("Press once to turn it on, again to turn it off. Works system-wide on "
				"Windows, so it can be used with the app being recorded in front."),
		 spotShortcutEdit_);

	QSlider *spotSize = nullptr;
	QWidget *spotSizeRow = makeSliderRow(this, spotSize, SpotlightParams::kMinSize,
					     SpotlightParams::kMaxSize, preset.spotlightSize,
					     QStringLiteral(" px"), 20);
	spotSizeSlider_ = spotSize;
	addField(v, QStringLiteral("Area size"),
		 QStringLiteral("How wide the lit area is, in screen pixels. The preview below shows "
				"it at the size it will actually be on the display this preset "
				"records."),
		 spotSizeRow);

	QSlider *spotDark = nullptr;
	QWidget *spotDarkRow = makeSliderRow(this, spotDark, 0, SpotlightParams::kMaxDark,
					     preset.spotlightDarkPct, QStringLiteral("%"), 5);
	spotDarkSlider_ = spotDark;
	addField(v, QStringLiteral("Dark area opacity"),
		 QStringLiteral("How dark everything outside the area goes. It stops short of fully "
				"black on purpose — you still have to find the window you are about "
				"to click while the spotlight is on."),
		 spotDarkRow);

	QSlider *spotRound = nullptr;
	QWidget *spotRoundRow = makeSliderRow(this, spotRound, 0, 100, preset.spotlightRoundness,
					      QStringLiteral("%"), 5);
	spotRoundSlider_ = spotRound;
	addField(v, QStringLiteral("Area roundness"),
		 QStringLiteral("0% is a hard-edged rectangle; 100% is a circle. In between, a "
				"rectangle with its corners rounded off."),
		 spotRoundRow);

	spotStartOnCheck_ = new QCheckBox(QStringLiteral("Start recordings with the spotlight on"), this);
	spotStartOnCheck_->setChecked(preset.spotlightStartOn);
	addCheck(v, spotStartOnCheck_,
		 QStringLiteral("Otherwise a recording starts with a normal screen and waits for the "
				"shortcut — which is usually what you want, since the first thing "
				"most recordings show is the whole screen."));

	spotPreview_ = new SpotlightPreview(this);
	addField(v, QStringLiteral("Preview"),
		 QStringLiteral("Move the mouse over this to see the spotlight follow it."),
		 spotPreview_);

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
	// it again and it pulls back out. Full Screen only, because that is where
	// the encoder canvas is the whole display and a smaller crop gets scaled
	// back up to fill it -- which is the zoom.
	QWidget *zoomPage = makePage(v);
	zoomCheck_ = new QCheckBox(QStringLiteral("Automatic Zoom (Full Screen only)"), this);
	zoomCheck_->setChecked(preset.zoomEnabled);
	addCheck(v, zoomCheck_,
		 QStringLiteral("Adds a shortcut that zooms the recording in on the mouse and back out "
				"again — for pointing at a menu or a line of code without editing "
				"afterwards. The zoomed picture follows the cursor."));

	zoomShortcutEdit_ = new QKeySequenceEdit(
		QKeySequence(QString::fromStdString(preset.zoomShortcut)), this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	// One chord: RegisterHotKey knows nothing about sequences.
	zoomShortcutEdit_->setMaximumSequenceLength(1);
#endif
	addField(v, QStringLiteral("Zoom shortcut"),
		 QStringLiteral("Press once to zoom in, again to zoom out. Works system-wide on Windows, "
				"so it can be used with the app being recorded in front. Unlike the "
				"start and pause hotkeys, this one belongs to the preset."),
		 zoomShortcutEdit_);

	QSlider *zoomPercentSlider = nullptr;
	QWidget *zoomPctRow = makeSliderRow(this, zoomPercentSlider, ZoomParams::kMinPercent,
					    ZoomParams::kMaxPercent, preset.zoomPercent,
					    QStringLiteral("%"), 5);
	zoomPercentSlider_ = zoomPercentSlider;
	addField(v, QStringLiteral("Zoom percentage"),
		 QStringLiteral("How far in. 200% shows half the width and half the height, magnified "
				"twice. Past about 300% a 1080p screen starts to look soft, because "
				"there are no more real pixels to enlarge."),
		 zoomPctRow);

	QSlider *zoomAnimSlider = nullptr;
	QWidget *zoomAnimRow = makeSliderRow(this, zoomAnimSlider, 0, 1500, preset.zoomAnimMs,
					     QStringLiteral(" ms"), 50);
	zoomAnimSlider_ = zoomAnimSlider;
	addField(v, QStringLiteral("Zoom animation speed"),
		 QStringLiteral("How long the push-in and the pull-out take. 0 cuts straight to the "
				"zoomed view; a few hundred milliseconds reads as a camera move."),
		 zoomAnimRow);

	QSlider *zoomSmoothSlider = nullptr;
	QWidget *zoomSmoothRow = makeSliderRow(this, zoomSmoothSlider, 0, 100,
					       preset.zoomFollowSmoothness, QString());
	zoomFollowSmoothSlider_ = zoomSmoothSlider;
	addField(v, QStringLiteral("Mouse follow speed"),
		 QStringLiteral("How the zoomed frame catches up with the cursor once it is in. 0 is "
				"glued to the mouse; higher values trail behind it. Separate from the "
				"animation speed above — how fast it pushes in and how tightly it "
				"tracks are different decisions."),
		 zoomSmoothRow);

	QSlider *zoomPadSlider = nullptr;
	QWidget *zoomPadRow = makeSliderRow(this, zoomPadSlider, 0, 45, preset.zoomFollowPaddingPct,
					    QStringLiteral("%"), 5);
	zoomFollowPadSlider_ = zoomPadSlider;
	addField(v, QStringLiteral("Mouse padding"),
		 QStringLiteral("The safe zone inside the zoomed frame. The picture holds still while "
				"the cursor stays within it, so small movements don't swim the shot."),
		 zoomPadRow);

	zoomFollowAxisCombo_ = new QComboBox(this);
	zoomFollowAxisCombo_->addItem(QStringLiteral("Both directions")); // 0
	zoomFollowAxisCombo_->addItem(QStringLiteral("Horizontal only")); // 1
	zoomFollowAxisCombo_->addItem(QStringLiteral("Vertical only"));   // 2
	zoomFollowAxisCombo_->setCurrentIndex(std::clamp(preset.zoomFollowAxis, 0, 2));
	addField(v, QStringLiteral("Mouse follow mode"),
		 QStringLiteral("Lock an axis if the zoom should only slide one way — along a toolbar, "
				"or down a page."),
		 zoomFollowAxisCombo_);

	// Everything below the checkbox is inert while the feature is off. Greyed
	// rather than hidden, so the settings can still be read and understood
	// before deciding to turn it on.
	const auto syncZoomEnabled = [this]() {
		const bool on = zoomCheck_->isChecked();
		zoomShortcutEdit_->setEnabled(on);
		zoomPercentSlider_->setEnabled(on);
		zoomAnimSlider_->setEnabled(on);
		zoomFollowSmoothSlider_->setEnabled(on);
		zoomFollowPadSlider_->setEnabled(on);
		zoomFollowAxisCombo_->setEnabled(on);
	};
	connect(zoomCheck_, &QCheckBox::toggled, this, syncZoomEnabled);
	syncZoomEnabled();

	v->addStretch(1);
	addPage(QStringLiteral("Zoom"), zoomPage);

	// ===== Webcam (recorded as a separate synchronized file) =====
	QWidget *webcamPage = makePage(v);
	webcamCheck_ = new QCheckBox(QStringLiteral("Record webcam as a separate video file"), this);
	webcamCheck_->setChecked(preset.webcamEnabled);
	addCheck(v, webcamCheck_,
		 QStringLiteral("Save the camera to its own file alongside the screen recording, kept in "
				"sync. It is never composited onto the screen video."));

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
	addField(v, QStringLiteral("Camera"), QStringLiteral("Which webcam to record."), webcamDeviceCombo_);

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
	addField(v, QStringLiteral("Webcam resolution"),
		 QStringLiteral("Capture size of the camera file, independent of the screen resolution."),
		 webcamResCombo_);

	webcamFpsCombo_ = new QComboBox(this);
	webcamFpsCombo_->setEditable(true);
	webcamFpsCombo_->setValidator(new QIntValidator(1, 240, webcamFpsCombo_));
	for (int val : {24, 30, 60})
		webcamFpsCombo_->addItem(QString::number(val));
	webcamFpsCombo_->setCurrentText(QString::number(preset.webcamFps > 0 ? preset.webcamFps : 30));
	addField(v, QStringLiteral("Webcam frame rate"), QStringLiteral("Frames per second for the camera file."),
		 webcamFpsCombo_);

	webcamCustomFolderCheck_ = new QCheckBox(QStringLiteral("Use a custom folder for the webcam file"), this);
	webcamCustomFolderCheck_->setChecked(preset.webcamUseCustomFolder);
	addCheck(v, webcamCustomFolderCheck_,
		 QStringLiteral("By default the camera file sits next to the screen recording; enable this "
				"to send it elsewhere."));

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
	addField(v, QStringLiteral("Webcam folder"), QString(), folderRowWidget(webcamFolderEdit_, wcBrowse));
	auto syncWebcamFolder = [this]() {
		webcamFolderEdit_->setEnabled(webcamCustomFolderCheck_->isChecked());
	};
	connect(webcamCustomFolderCheck_, &QCheckBox::toggled, this, syncWebcamFolder);
	syncWebcamFolder();
	v->addStretch(1);
	addPage(QStringLiteral("Webcam"), webcamPage);

	// ===== Hotkeys =====
	// App-wide, not per preset: which key starts a recording is a property of
	// the keyboard in front of the user, not of the format being recorded.
	// Stored in QSettings; the main window re-registers on save.
	QWidget *hotkeysPage = makePage(v);
	{
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
			 QStringLiteral("Works system-wide on Windows — no need to bring Harpia to the "
					"front. If another app already owns the key, it still works "
					"while Harpia is focused."),
			 recordKeyEdit_);
		pauseKeyEdit_ = new QKeySequenceEdit(
			QKeySequence(hk.value(QStringLiteral("hotkeys/pause"), QStringLiteral("F10"))
					     .toString()),
			this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		pauseKeyEdit_->setMaximumSequenceLength(1);
#endif
		addField(v, QStringLiteral("Pause / resume"),
			 QStringLiteral("Same rules. Note GIF recordings can't pause at all."),
			 pauseKeyEdit_);
		auto *shared = new QLabel(QStringLiteral("Hotkeys are shared by every preset."), this);
		shared->setStyleSheet(QStringLiteral("color:#8a8f98;"));
		v->addWidget(shared);
	}
	v->addStretch(1);
	addPage(QStringLiteral("Hotkeys"), hotkeysPage);

	// ===== Advanced =====
	QWidget *advancedPage = makePage(v);
	frameRateModeCombo_ = new QComboBox(this);
	frameRateModeCombo_->addItem(QStringLiteral("Constant (CFR)"), int(FrameRateMode::CFR));
	frameRateModeCombo_->addItem(QStringLiteral("Variable (VFR)"), int(FrameRateMode::VFR));
	frameRateModeCombo_->setCurrentIndex(frameRateModeCombo_->findData(int(preset.frameRateMode)));
	addField(v, QStringLiteral("Frame rate mode"),
		 QStringLiteral("Constant (CFR) writes an even frame every tick — safest for editing. "
				"Variable (VFR) saves space but some editors handle it poorly."),
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

	auto syncBitrate = [this]() {
		bitrateSpin_->setVisible(bitrateCombo_->currentData().toInt() == -1);
	};
	connect(bitrateCombo_, &QComboBox::currentIndexChanged, this, syncBitrate);

	addField(v, QStringLiteral("Bitrate"),
		 QStringLiteral("Higher bitrate = better quality and larger files. Auto picks a sensible "
				"value from the resolution, frame rate, and codec; Custom… lets you type "
				"an exact number."),
		 bitrateCombo_);
	v->addWidget(bitrateSpin_);
	v->addSpacing(12);
	syncBitrate();
	v->addStretch(1);
	addPage(QStringLiteral("Advanced"), advancedPage);

	// ---- Assemble: header on top, nav | pages, buttons at the bottom ----
	connect(nav_, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
	nav_->setCurrentRow(0);

	auto *body = new QHBoxLayout;
	body->addWidget(nav_);
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
	resize(640, 580);
}

void PresetEditorDialog::showPage(const QString &title)
{
	if (!nav_)
		return;
	for (int i = 0; i < nav_->count(); ++i) {
		if (nav_->item(i)->text() == title) {
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

	// The controls are inert while the feature is off, greyed rather than
	// hidden so they can still be read before deciding to turn it on. The
	// preview stays live either way: it is how you decide.
	const bool on = spotCheck_->isChecked();
	spotShortcutEdit_->setEnabled(on);
	spotSizeSlider_->setEnabled(on);
	spotDarkSlider_->setEnabled(on);
	spotRoundSlider_->setEnabled(on);
	spotStartOnCheck_->setEnabled(on);
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

	// GIF quietly overrides half this dialog, so the dialog says so instead:
	// every control GIF ignores is disabled while GIF is chosen, and the notes
	// name each override. A page of live-looking controls that silently do
	// nothing is how "my settings don't work" reports happen.
	const bool isGif = (format == RecordingFormat::GIF);
	codecCombo_->setEnabled(!isGif);
	bitrateCombo_->setEnabled(!isGif);
	bitrateSpin_->setEnabled(!isGif);
	frameRateModeCombo_->setEnabled(!isGif);
	if (desktopAudioCheck_)
		desktopAudioCheck_->setEnabled(!isGif);
	if (audioBitrateCombo_)
		audioBitrateCombo_->setEnabled(!isGif);
	if (audioGifNote_)
		audioGifNote_->setVisible(isGif);

	QString msg;
	if (isGif) {
		msg = QStringLiteral("GIF uses its own palette encoder: codec and bitrate are ignored, "
				     "the frame rate is capped at 15, the image is downscaled to keep "
				     "files small, there is no audio track, and recording can't pause.");
	} else if (format == RecordingFormat::AVI && codec != VideoCodec::H264) {
		msg = QStringLiteral("AVI has poor support for HEVC/AV1 — MP4 or MKV is recommended.");
	} else if (format == RecordingFormat::MP4 && codec == VideoCodec::HEVC) {
		msg = QStringLiteral("Tip: HEVC in MP4 may not play everywhere; MKV is the safest container.");
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

void PresetEditorDialog::accept()
{
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

	result_.name = name.toStdString();
	result_.format = RecordingFormat(formatCombo_->currentData().toInt());
	result_.codec = VideoCodec(codecCombo_->currentData().toInt());
	result_.frameRateMode = FrameRateMode(frameRateModeCombo_->currentData().toInt());
	result_.fps = qMax(1, fpsCombo_->currentText().toInt());
	{
		const int sel = bitrateCombo_->currentData().toInt();
		if (sel == 0)
			result_.videoBitrateKbps = 0; // Auto
		else if (sel == -1)
			result_.videoBitrateKbps = bitrateSpin_->value(); // Custom
		else
			result_.videoBitrateKbps = sel; // a listed preset
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

	result_.outputFolder = folderEdit_->text().trimmed().toStdString();
	// monitorIndex and captureMode are intentionally not edited here — the main
	// window owns both (its display and capture combos write through).
	result_.gpuCompression = gpuCheck_->isChecked();
	result_.idleTimeoutSeconds = idleSpin_->value();
	result_.countdownSeconds = countdownCombo_->currentData().toInt();
	result_.minRecordingSeconds = minLengthSpin_->value();
	result_.showScreenBorder = borderCheck_->isChecked();
	result_.screenBorderColor = borderColor_.name().toStdString();
	result_.screenBorderThickness = borderThicknessSpin_->value();
	result_.regionMoveHandle = regionHandleCheck_->isChecked();
	result_.filenameTemplate = templateEdit_->text().trimmed().toStdString();
	// An empty template would expand to an extension-only (hidden) filename
	// like ".mp4" — restore the default naming instead of saving it.
	if (result_.filenameTemplate.empty())
		result_.filenameTemplate = Preset::makeDefault("").filenameTemplate;

	result_.googleDriveLink = driveLinkEdit_->text().trimmed().toStdString();

	result_.recordDesktopAudio = desktopAudioCheck_->isChecked();
	result_.audioBitrateKbps = audioBitrateCombo_->currentData().toInt();
	result_.micDeviceIds.clear();
	for (int i = 0; i < micChecks_.size(); ++i) {
		if (micChecks_[i]->isChecked())
			result_.micDeviceIds.push_back(micIds_[i].toStdString());
	}

	result_.showMouseCursor = mouseCursorCheck_->isChecked();
	result_.showMouseArea = mouseAreaCheck_->isChecked();
	result_.mouseHighlightColor = highlightColor_.name().toStdString();
	result_.mouseHighlightSize = highlightSizeSlider_->value();
	result_.recordMouseClicks = mouseClicksCheck_->isChecked();
	result_.leftClickColor = leftColor_.name().toStdString();
	result_.rightClickColor = rightColor_.name().toStdString();
	result_.followMouse = followCheck_->isChecked();
	result_.followPaddingPct = followPaddingSlider_->value();
	result_.followSmoothness = followSmoothSlider_->value();
	result_.followAxis = followAxisCombo_->currentIndex();
	result_.followProfile = followProfileCombo_->currentIndex();

	result_.spotlightEnabled = spotCheck_->isChecked();
	result_.spotlightStartOn = spotStartOnCheck_->isChecked();
	result_.spotlightSize = spotSizeSlider_->value();
	result_.spotlightDarkPct = spotDarkSlider_->value();
	result_.spotlightRoundness = spotRoundSlider_->value();
	// Same rule as the zoom key: an empty sequence means "no shortcut", not
	// "put the default back".
	result_.spotlightShortcut = spotShortcutEdit_->keySequence().toString().toStdString();

	result_.zoomEnabled = zoomCheck_->isChecked();
	result_.zoomPercent = zoomPercentSlider_->value();
	result_.zoomAnimMs = zoomAnimSlider_->value();
	result_.zoomFollowSmoothness = zoomFollowSmoothSlider_->value();
	result_.zoomFollowPaddingPct = zoomFollowPadSlider_->value();
	result_.zoomFollowAxis = zoomFollowAxisCombo_->currentIndex();
	// An empty shortcut means "no zoom hotkey" rather than a broken one -- the
	// main window simply registers nothing, and the feature is unreachable
	// until a key is set. Better than silently reinstating the default the user
	// just cleared.
	result_.zoomShortcut = zoomShortcutEdit_->keySequence().toString().toStdString();

	result_.webcamEnabled = webcamCheck_->isChecked();
	result_.webcamDeviceId = webcamDeviceCombo_->currentData().toString().toStdString();
	{
		const QStringList wh = webcamResCombo_->currentData().toString().split(QLatin1Char('x'));
		result_.webcamWidth = wh.value(0).toInt();
		result_.webcamHeight = wh.value(1).toInt();
	}
	result_.webcamFps = qMax(1, webcamFpsCombo_->currentText().toInt());
	result_.webcamUseCustomFolder = webcamCustomFolderCheck_->isChecked();
	result_.webcamFolder = webcamFolderEdit_->text().trimmed().toStdString();

	QDialog::accept();
}

} // namespace harpia
