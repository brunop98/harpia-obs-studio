#include "PresetEditorDialog.hpp"

#include "MousePreview.hpp"
#include "core/CaptureManager.hpp"
#include "core/EncoderFactory.hpp"
#include "core/WebcamRecorder.hpp"
#include "core/AudioManager.hpp" // AudioDevice

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
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

} // namespace

PresetEditorDialog::PresetEditorDialog(const Preset &preset, QWidget *parent)
	: QDialog(parent), result_(preset)
{
	setWindowTitle(QStringLiteral("Edit Preset"));
	setModal(true);

	auto *form = new QFormLayout;

	nameEdit_ = new QLineEdit(QString::fromStdString(preset.name), this);
	form->addRow(QStringLiteral("Name"), nameEdit_);

	// ---- Video section --------------------------------------------------
	form->addRow(new QLabel(QStringLiteral("<b>Video</b>"), this));

	formatCombo_ = new QComboBox(this);
	formatCombo_->addItem(QStringLiteral("MP4"), int(RecordingFormat::MP4));
	formatCombo_->addItem(QStringLiteral("MKV"), int(RecordingFormat::MKV));
	formatCombo_->addItem(QStringLiteral("MOV"), int(RecordingFormat::MOV));
	formatCombo_->addItem(QStringLiteral("AVI"), int(RecordingFormat::AVI));
	formatCombo_->addItem(QStringLiteral("GIF"), int(RecordingFormat::GIF));
	formatCombo_->setCurrentIndex(formatCombo_->findData(int(preset.format)));
	form->addRow(QStringLiteral("Format"), formatCombo_);

	// Codec list shows only codecs the backend can actually encode.
	codecCombo_ = new QComboBox(this);
	for (VideoCodec c : EncoderFactory::availableCodecs(/*gpuOnly=*/false))
		codecCombo_->addItem(QString::fromUtf8(codecLabel(c)), int(c));
	int codecIdx = codecCombo_->findData(int(preset.codec));
	codecCombo_->setCurrentIndex(codecIdx >= 0 ? codecIdx : 0);
	form->addRow(QStringLiteral("Codec"), codecCombo_);

	// Resolution is always native — the full display, or the selected capture
	// region — so there is no size control here.
	auto *resNote = new QLabel(QStringLiteral("Records at the screen / region resolution"), this);
	resNote->setStyleSheet(QStringLiteral("color: gray;"));
	form->addRow(QStringLiteral("Resolution"), resNote);

	fpsCombo_ = new QComboBox(this);
	fpsCombo_->setEditable(true);
	fpsCombo_->setValidator(new QIntValidator(1, 240, fpsCombo_));
	for (int v : {24, 30, 60, 120})
		fpsCombo_->addItem(QString::number(v));
	fpsCombo_->setCurrentText(QString::number(preset.fps));
	form->addRow(QStringLiteral("Frame rate (fps)"), fpsCombo_);

	frameRateModeCombo_ = new QComboBox(this);
	frameRateModeCombo_->addItem(QStringLiteral("Constant (CFR)"), int(FrameRateMode::CFR));
	frameRateModeCombo_->addItem(QStringLiteral("Variable (VFR)"), int(FrameRateMode::VFR));
	frameRateModeCombo_->setCurrentIndex(frameRateModeCombo_->findData(int(preset.frameRateMode)));
	form->addRow(QStringLiteral("Frame rate mode"), frameRateModeCombo_);

	bitrateSpin_ = new QSpinBox(this);
	bitrateSpin_->setRange(0, 200000);
	bitrateSpin_->setSingleStep(500);
	bitrateSpin_->setSuffix(QStringLiteral(" Kbps"));
	bitrateSpin_->setSpecialValueText(QStringLiteral("Auto"));
	bitrateSpin_->setValue(preset.videoBitrateKbps);
	form->addRow(QStringLiteral("Bitrate"), bitrateSpin_);

	gpuCheck_ = new QCheckBox(QStringLiteral("Use GPU compression while recording (NVENC/AMF/QSV)"), this);
	gpuCheck_->setChecked(preset.gpuCompression);
	form->addRow(QString(), gpuCheck_);

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
	form->addRow(QStringLiteral("Display"), monitorCombo_);

	validationLabel_ = new QLabel(this);
	validationLabel_->setWordWrap(true);
	validationLabel_->setStyleSheet(QStringLiteral("color:#d29922;"));
	form->addRow(QString(), validationLabel_);

	// ---- Output section -------------------------------------------------
	form->addRow(new QLabel(QStringLiteral("<b>Output</b>"), this));

	folderEdit_ = new QLineEdit(QString::fromStdString(preset.outputFolder), this);
	auto *browse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(browse, &QPushButton::clicked, this, &PresetEditorDialog::browseFolder);
	auto *folderRow = new QHBoxLayout;
	folderRow->addWidget(folderEdit_, 1);
	folderRow->addWidget(browse);
	form->addRow(QStringLiteral("Output folder"), folderRow);

	idleSpin_ = new QSpinBox(this);
	idleSpin_->setRange(0, 3600);
	idleSpin_->setSuffix(QStringLiteral(" s"));
	idleSpin_->setSpecialValueText(QStringLiteral("Disabled"));
	idleSpin_->setValue(preset.idleTimeoutSeconds);
	form->addRow(QStringLiteral("Auto-pause after idle"), idleSpin_);

	templateEdit_ = new QLineEdit(QString::fromStdString(preset.filenameTemplate), this);
	form->addRow(QStringLiteral("Filename template"), templateEdit_);
	auto *tokenHelp =
		new QLabel(QStringLiteral("Tokens: {Year} {Month} {Day} {Hour} {Minute} {Second}"), this);
	tokenHelp->setStyleSheet(QStringLiteral("color: gray;"));
	form->addRow(QString(), tokenHelp);

	// ---- Mouse section --------------------------------------------------
	form->addRow(new QLabel(QStringLiteral("<b>Mouse</b>"), this));

	mouseCursorCheck_ = new QCheckBox(QStringLiteral("Show mouse cursor"), this);
	mouseCursorCheck_->setChecked(preset.showMouseCursor);
	form->addRow(QString(), mouseCursorCheck_);

	mouseAreaCheck_ = new QCheckBox(QStringLiteral("Show mouse area (highlight around cursor)"), this);
	mouseAreaCheck_->setChecked(preset.showMouseArea);
	form->addRow(QString(), mouseAreaCheck_);

	highlightColor_ = QColor(QString::fromStdString(preset.mouseHighlightColor));
	if (!highlightColor_.isValid())
		highlightColor_ = QColor(0xff, 0xd5, 0x4a);
	highlightColorBtn_ = new QPushButton(this);
	setButtonColor(highlightColorBtn_, highlightColor_);
	connect(highlightColorBtn_, &QPushButton::clicked, this,
		[this]() { pickColor(highlightColor_, highlightColorBtn_); });
	form->addRow(QStringLiteral("Highlight color"), highlightColorBtn_);

	highlightSizeSlider_ = new QSlider(Qt::Horizontal, this);
	highlightSizeSlider_->setRange(10, 200);
	highlightSizeSlider_->setValue(preset.mouseHighlightSize > 0 ? preset.mouseHighlightSize : 60);
	form->addRow(QStringLiteral("Highlight size"), highlightSizeSlider_);

	mouseClicksCheck_ = new QCheckBox(QStringLiteral("Record mouse clicks (click animations)"), this);
	mouseClicksCheck_->setChecked(preset.recordMouseClicks);
	form->addRow(QString(), mouseClicksCheck_);

	leftColor_ = QColor(QString::fromStdString(preset.leftClickColor));
	if (!leftColor_.isValid())
		leftColor_ = QColor(0x4a, 0x90, 0xe2);
	leftColorBtn_ = new QPushButton(this);
	setButtonColor(leftColorBtn_, leftColor_);
	connect(leftColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(leftColor_, leftColorBtn_); });
	form->addRow(QStringLiteral("Left click color"), leftColorBtn_);

	rightColor_ = QColor(QString::fromStdString(preset.rightClickColor));
	if (!rightColor_.isValid())
		rightColor_ = QColor(0xe2, 0x53, 0x4a);
	rightColorBtn_ = new QPushButton(this);
	setButtonColor(rightColorBtn_, rightColor_);
	connect(rightColorBtn_, &QPushButton::clicked, this, [this]() { pickColor(rightColor_, rightColorBtn_); });
	form->addRow(QStringLiteral("Right click color"), rightColorBtn_);

	mousePreview_ = new MousePreview(this);
	form->addRow(QStringLiteral("Preview"), mousePreview_);

	connect(mouseAreaCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(mouseClicksCheck_, &QCheckBox::toggled, this, &PresetEditorDialog::updateMousePreview);
	connect(highlightSizeSlider_, &QSlider::valueChanged, this, &PresetEditorDialog::updateMousePreview);

	// ---- Webcam section (recorded as a separate synchronized file) ------
	form->addRow(new QLabel(QStringLiteral("<b>Webcam</b>"), this));

	webcamCheck_ = new QCheckBox(QStringLiteral("Record webcam as a separate video file"), this);
	webcamCheck_->setChecked(preset.webcamEnabled);
	form->addRow(QString(), webcamCheck_);

	webcamDeviceCombo_ = new QComboBox(this);
	for (const AudioDevice &cam : WebcamRecorder::cameras())
		webcamDeviceCombo_->addItem(QString::fromStdString(cam.name), QString::fromStdString(cam.id));
	if (webcamDeviceCombo_->count() == 0)
		webcamDeviceCombo_->addItem(QStringLiteral("(no camera detected)"), QString());
	{
		const int di = webcamDeviceCombo_->findData(QString::fromStdString(preset.webcamDeviceId));
		if (di >= 0)
			webcamDeviceCombo_->setCurrentIndex(di);
	}
	form->addRow(QStringLiteral("Camera"), webcamDeviceCombo_);

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
	form->addRow(QStringLiteral("Webcam resolution"), webcamResCombo_);

	webcamFpsCombo_ = new QComboBox(this);
	webcamFpsCombo_->setEditable(true);
	webcamFpsCombo_->setValidator(new QIntValidator(1, 240, webcamFpsCombo_));
	for (int v : {24, 30, 60})
		webcamFpsCombo_->addItem(QString::number(v));
	webcamFpsCombo_->setCurrentText(QString::number(preset.webcamFps > 0 ? preset.webcamFps : 30));
	form->addRow(QStringLiteral("Webcam frame rate"), webcamFpsCombo_);

	webcamCustomFolderCheck_ = new QCheckBox(QStringLiteral("Use a custom folder for the webcam file"), this);
	webcamCustomFolderCheck_->setChecked(preset.webcamUseCustomFolder);
	form->addRow(QString(), webcamCustomFolderCheck_);

	webcamFolderEdit_ = new QLineEdit(QString::fromStdString(preset.webcamFolder), this);
	auto *wcBrowse = new QPushButton(QStringLiteral("Browse…"), this);
	connect(wcBrowse, &QPushButton::clicked, this, [this]() {
		const QString dir =
			QFileDialog::getExistingDirectory(this, QStringLiteral("Webcam output folder"),
							  webcamFolderEdit_->text());
		if (!dir.isEmpty())
			webcamFolderEdit_->setText(dir);
	});
	auto *wcFolderRow = new QHBoxLayout;
	wcFolderRow->addWidget(webcamFolderEdit_, 1);
	wcFolderRow->addWidget(wcBrowse);
	form->addRow(QStringLiteral("Webcam folder"), wcFolderRow);

	auto syncWebcamFolder = [this]() {
		webcamFolderEdit_->setEnabled(webcamCustomFolderCheck_->isChecked());
	};
	connect(webcamCustomFolderCheck_, &QCheckBox::toggled, this, syncWebcamFolder);
	syncWebcamFolder();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &PresetEditorDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);

	connect(formatCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateValidation);
	connect(codecCombo_, &QComboBox::currentIndexChanged, this, &PresetEditorDialog::updateValidation);
	updateValidation();
	updateMousePreview();
	resize(480, sizeHint().height());
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
	result_.videoBitrateKbps = bitrateSpin_->value();

	// Resolution is always native (screen or region) — no scaling.
	result_.resolutionMode = ResolutionMode::Native;

	result_.outputFolder = folderEdit_->text().trimmed().toStdString();
	result_.monitorIndex = monitorCombo_->currentData().toInt();
	result_.gpuCompression = gpuCheck_->isChecked();
	result_.idleTimeoutSeconds = idleSpin_->value();
	result_.filenameTemplate = templateEdit_->text().trimmed().toStdString();

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
