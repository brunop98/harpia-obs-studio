#include "PresetEditorDialog.hpp"

#include "MousePreview.hpp"
#include "core/CaptureManager.hpp"
#include "core/EncoderFactory.hpp"
#include "core/WebcamRecorder.hpp"
#include "core/AudioManager.hpp" // AudioDevice
#include "model/FileNameTemplate.hpp"
#include "platform/CameraAccess.hpp"

#include <map>

#include <algorithm>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
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
	monitorCombo_ = new QComboBox(this);
	const std::vector<MonitorOption> monitors = CaptureManager::enumerateMonitors();
	if (monitors.empty()) {
		monitorCombo_->addItem(QStringLiteral("Primary display"), 0);
	} else {
		int idx = 0;
		for (const MonitorOption &m : monitors)
			monitorCombo_->addItem(QString::fromStdString(m.name), idx++);
	}
	if (preset.monitorIndex >= 0 && preset.monitorIndex < monitorCombo_->count())
		monitorCombo_->setCurrentIndex(preset.monitorIndex);
	addField(v, QStringLiteral("Display"),
		 QStringLiteral("Which monitor to capture when recording the entire screen."),
		 monitorCombo_);

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

	pauseFocusCheck_ = new QCheckBox(QStringLiteral("Auto-pause when the target application loses focus"),
					this);
	pauseFocusCheck_->setChecked(preset.pauseOnFocusLoss);
	addCheck(v, pauseFocusCheck_,
		 QStringLiteral("Remembers the app in the foreground when you hit Record and pauses whenever "
				"it isn't the active window (its dialogs and pickers still count as focused), "
				"resuming when you return. Great for Unity/Photoshop/Blender. Windows only."));

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
	addField(v, QStringLiteral("Border color"), QString(), borderColorBtn_);

	borderThicknessSpin_ = new QSpinBox(this);
	borderThicknessSpin_->setRange(1, 10);
	borderThicknessSpin_->setSuffix(QStringLiteral(" px"));
	borderThicknessSpin_->setValue(preset.screenBorderThickness > 0 ? preset.screenBorderThickness : 4);
	addField(v, QStringLiteral("Border thickness"), QString(), borderThicknessSpin_);

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
	codecCombo_->setCurrentIndex(codecIdx >= 0 ? codecIdx : 0);
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
	v->addStretch(1);
	addPage(QStringLiteral("Audio"), audioPage);

	// ===== Output =====
	QWidget *outputPage = makePage(v);
	folderEdit_ = new QLineEdit(QString::fromStdString(preset.outputFolder), this);
	auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browse, &QPushButton::clicked, this, &PresetEditorDialog::browseFolder);
	addField(v, QStringLiteral("Output folder"),
		 QStringLiteral("Where finished recordings are saved."),
		 folderRowWidget(folderEdit_, browse));

	templateEdit_ = new QLineEdit(QString::fromStdString(preset.filenameTemplate), this);
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

	connect(templateEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(nameEdit_, &QLineEdit::textChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(fpsCombo_, &QComboBox::currentTextChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(codecCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateFilenamePreview);
	connect(formatCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateFilenamePreview);

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
	addField(v, QStringLiteral("Highlight color"), QString(), highlightColorBtn_);

	highlightSizeSlider_ = new QSlider(Qt::Horizontal, this);
	highlightSizeSlider_->setRange(10, 200);
	highlightSizeSlider_->setValue(preset.mouseHighlightSize > 0 ? preset.mouseHighlightSize : 60);
	addField(v, QStringLiteral("Highlight size"),
		 QStringLiteral("Diameter of the cursor highlight ring, in pixels."), highlightSizeSlider_);

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
	addField(v, QStringLiteral("Left click color"), QString(), leftColorBtn_);

	rightColor_ = QColor(QString::fromStdString(preset.rightClickColor));
	if (!rightColor_.isValid())
		rightColor_ = QColor(0xe2, 0x53, 0x4a);
	rightColorBtn_ = new QPushButton(this);
	setButtonColor(rightColorBtn_, rightColor_);
	connect(rightColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(rightColor_, rightColorBtn_); });
	addField(v, QStringLiteral("Right click color"), QString(), rightColorBtn_);

	mousePreview_ = new MousePreview(this);
	addField(v, QStringLiteral("Preview"), QString(), mousePreview_);
	connect(mouseAreaCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(mouseClicksCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(highlightSizeSlider_, &QSlider::valueChanged, this, &PresetEditorDialog::updateMousePreview);
	v->addStretch(1);
	addPage(QStringLiteral("Mouse"), mousePage);

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
		const int di = webcamDeviceCombo_->findData(QString::fromStdString(preset.webcamDeviceId));
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
			"\"C++ ATL for latest v143 build tools\" component and rebuild to enable it.");
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

	// ===== Hotkeys (placeholder — no hotkey engine yet) =====
	QWidget *hotkeysPage = makePage(v);
	auto *hotkeysNote = new QLabel(
		QStringLiteral("Global start/stop/pause hotkeys are coming soon. For now, use the buttons on "
			       "the main window."),
		this);
	hotkeysNote->setWordWrap(true);
	hotkeysNote->setStyleSheet(QStringLiteral("color:#8a8f98;"));
	v->addWidget(hotkeysNote);
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

void PresetEditorDialog::updateMousePreview()
{
	if (!mousePreview_)
		return;
	mousePreview_->configure(mouseAreaCheck_->isChecked(), highlightColor_, highlightSizeSlider_->value(),
				 mouseClicksCheck_->isChecked(), leftColor_);
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
	templatePreview_->setText(
		QStringLiteral("Preview: %1.%2").arg(QString::fromStdString(base), ext));
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

	// GIF ignores codec/bitrate/fps-mode entirely.
	const bool isGif = (format == RecordingFormat::GIF);
	codecCombo_->setEnabled(!isGif);
	bitrateCombo_->setEnabled(!isGif);
	bitrateSpin_->setEnabled(!isGif);
	frameRateModeCombo_->setEnabled(!isGif);

	QString msg;
	if (isGif) {
		msg = QStringLiteral("GIF uses its own palette encoder; codec and bitrate are ignored.");
	} else if (format == RecordingFormat::AVI && codec != VideoCodec::H264) {
		msg = QStringLiteral("AVI has poor support for HEVC/AV1 — MP4 or MKV is recommended.");
	} else if (format == RecordingFormat::MP4 && codec == VideoCodec::HEVC) {
		msg = QStringLiteral("Tip: HEVC in MP4 may not play everywhere; MKV is the safest container.");
	}
	validationLabel_->setText(msg);
	validationLabel_->setVisible(!msg.isEmpty());
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

	// Resolution is always native (screen or region) — no scaling.
	result_.resolutionMode = ResolutionMode::Native;

	result_.outputFolder = folderEdit_->text().trimmed().toStdString();
	result_.monitorIndex = monitorCombo_->currentData().toInt();
	result_.gpuCompression = gpuCheck_->isChecked();
	result_.idleTimeoutSeconds = idleSpin_->value();
	result_.countdownSeconds = countdownCombo_->currentData().toInt();
	result_.minRecordingSeconds = minLengthSpin_->value();
	result_.pauseOnFocusLoss = pauseFocusCheck_->isChecked();
	result_.showScreenBorder = borderCheck_->isChecked();
	result_.screenBorderColor = borderColor_.name().toStdString();
	result_.screenBorderThickness = borderThicknessSpin_->value();
	result_.filenameTemplate = templateEdit_->text().trimmed().toStdString();

	result_.recordDesktopAudio = desktopAudioCheck_->isChecked();
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
