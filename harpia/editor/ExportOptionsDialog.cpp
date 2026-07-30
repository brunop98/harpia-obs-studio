#include "ExportOptionsDialog.hpp"

#include "ExportEstimate.hpp"
#include "../ui/UiText.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {
namespace {

// Everything the dialog remembers between exports. Not the file NAME -- that
// belongs to the clip -- but every setting that describes how you like your
// output, because re-picking 720p and 15 fps on every export is most of the
// tedium the old dialog had.
constexpr char kGroup[] = "export/";

// The quality slider runs 0..100 left-to-right (smaller file -> better
// quality) and maps onto CRF backwards, because CRF counts the other way.
// The useful range for x264 is about 30 down to 16; outside it you are either
// looking at mush or paying for bits nobody can see.
constexpr int kCrfWorst = 30;
constexpr int kCrfBest = 16;
int crfForSlider(int v)
{
	const double t = std::clamp(v, 0, 100) / 100.0;
	return int(std::lround(kCrfWorst + t * (kCrfBest - kCrfWorst)));
}
int sliderForCrf(int crf)
{
	const double t = double(kCrfWorst - crf) / double(kCrfWorst - kCrfBest);
	return int(std::lround(std::clamp(t, 0.0, 1.0) * 100.0));
}

QString formatName(ClipExporter::Format f)
{
	switch (f) {
	case ClipExporter::Format::Gif: return QStringLiteral("GIF");
	case ClipExporter::Format::Mkv: return QStringLiteral("MKV");
	case ClipExporter::Format::Mov: return QStringLiteral("MOV");
	case ClipExporter::Format::WebM: return QStringLiteral("WebM");
	default: return QStringLiteral("MP4");
	}
}

} // namespace

ExportOptionsDialog::ExportOptionsDialog(const Context &ctx, QWidget *parent)
	: QDialog(parent), ctx_(ctx)
{
	setWindowTitle(QStringLiteral("Export clip"));
	setModal(true);

	auto *root = new QVBoxLayout(this);
	auto *columns = new QHBoxLayout;
	root->addLayout(columns);

	// ---- Left: the choices ------------------------------------------------
	auto *left = new QVBoxLayout;
	columns->addLayout(left, 3);
	auto *form = new QFormLayout;
	form->setLabelAlignment(Qt::AlignLeft);
	left->addLayout(form);

	nameEdit_ = new QLineEdit(ctx_.defaultName, this);
	nameEdit_->setPlaceholderText(QStringLiteral("Output file name"));
	form->addRow(QStringLiteral("File name"), nameEdit_);

	// Where it lands. The old dialog wrote silently beside the SOURCE file,
	// which is not where anyone keeps their exports and was nowhere on screen.
	auto *folderRow = new QWidget(this);
	auto *fl = new QHBoxLayout(folderRow);
	fl->setContentsMargins(0, 0, 0, 0);
	folderEdit_ = new QLineEdit(QDir::toNativeSeparators(ctx_.defaultFolder), folderRow);
	fl->addWidget(folderEdit_, 1);
	auto *browse = new QPushButton(QStringLiteral("Browse…"), folderRow);
	connect(browse, &QPushButton::clicked, this, &ExportOptionsDialog::browseFolder);
	fl->addWidget(browse);
	form->addRow(QStringLiteral("Save to"), folderRow);

	formatCombo_ = new QComboBox(this);
	if (ctx_.allowGif)
		formatCombo_->addItem(QStringLiteral("GIF (animated)"), int(ClipExporter::Format::Gif));
	formatCombo_->addItem(QStringLiteral("MP4 (H.264)"), int(ClipExporter::Format::Mp4));
	formatCombo_->addItem(QStringLiteral("MKV (H.264)"), int(ClipExporter::Format::Mkv));
	formatCombo_->addItem(QStringLiteral("MOV (H.264)"), int(ClipExporter::Format::Mov));
	if (ClipExporter::webmAvailable())
		formatCombo_->addItem(QStringLiteral("WebM (VP9)"), int(ClipExporter::Format::WebM));
	form->addRow(QStringLiteral("Format"), formatCombo_);

	// Resolution presets, built from the SOURCE: offering 1080p for a 720p clip
	// would be an upscale nobody asked for, so anything taller than the source
	// is left out rather than shown and silently ignored.
	resCombo_ = new QComboBox(this);
	const QSize src = ctx_.sourceSize;
	resCombo_->addItem(src.isValid()
				   ? QStringLiteral("Original (%1×%2)").arg(src.width()).arg(src.height())
				   : QStringLiteral("Original"),
			   0);
	if (src.isValid()) {
		for (int h : {1080, 720, 480, 360}) {
			if (h >= src.height())
				continue;
			const int w = int(std::lround(double(src.width()) * h / src.height())) & ~1;
			resCombo_->addItem(QStringLiteral("%1p  (%2×%3)").arg(h).arg(w).arg(h),
					   h);
		}
	}
	resCombo_->addItem(QStringLiteral("Custom…"), -1);
	form->addRow(QStringLiteral("Resolution"), resCombo_);

	customRow_ = new QWidget(this);
	auto *cl = new QHBoxLayout(customRow_);
	cl->setContentsMargins(0, 0, 0, 0);
	customW_ = new QSpinBox(customRow_);
	customW_->setRange(16, 7680);
	customW_->setSingleStep(2);
	customW_->setValue(src.isValid() ? src.width() : 1920);
	customH_ = new QSpinBox(customRow_);
	customH_->setRange(16, 4320);
	customH_->setSingleStep(2);
	customH_->setValue(src.isValid() ? src.height() : 1080);
	cl->addWidget(customW_);
	cl->addWidget(new QLabel(QStringLiteral("×"), customRow_));
	cl->addWidget(customH_);
	cl->addStretch(1);
	form->addRow(QString(), customRow_);

	// ---- Video-only -------------------------------------------------------
	videoRow_ = new QWidget(this);
	auto *vform = new QFormLayout(videoRow_);
	vform->setContentsMargins(0, 0, 0, 0);
	auto *qrow = new QWidget(videoRow_);
	auto *ql = new QHBoxLayout(qrow);
	ql->setContentsMargins(0, 0, 0, 0);
	qualitySlider_ = new QSlider(Qt::Horizontal, qrow);
	qualitySlider_->setRange(0, 100);
	qualitySlider_->setValue(sliderForCrf(23));
	ql->addWidget(qualitySlider_, 1);
	qualityLabel_ = new QLabel(qrow);
	qualityLabel_->setMinimumWidth(70);
	ql->addWidget(qualityLabel_);
	vform->addRow(QStringLiteral("Quality"), qrow);
	auto *hint = new QLabel(QStringLiteral("Smaller file  ←→  Better quality"), videoRow_);
	hint->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	vform->addRow(QString(), hint);
	audioCheck_ = new QCheckBox(QStringLiteral("Keep audio"), videoRow_);
	audioCheck_->setChecked(true);
	audioCheck_->setEnabled(ctx_.canKeepAudio);
	audioCheck_->setToolTip(
		QStringLiteral("Keep the source audio, if the clip has any. WebM is written "
			       "silent by this exporter either way."));
	vform->addRow(QString(), audioCheck_);
	left->addWidget(videoRow_);

	// ---- GIF-only ---------------------------------------------------------
	gifRow_ = new QWidget(this);
	auto *gform = new QFormLayout(gifRow_);
	gform->setContentsMargins(0, 0, 0, 0);
	gifFpsSpin_ = new QSpinBox(gifRow_);
	gifFpsSpin_->setRange(1, 50);
	gifFpsSpin_->setValue(15);
	gifFpsSpin_->setSuffix(QStringLiteral(" fps"));
	gform->addRow(QStringLiteral("Frame rate"), gifFpsSpin_);
	gifColorsCombo_ = new QComboBox(gifRow_);
	for (int c : {256, 128, 64, 32, 16, 8})
		gifColorsCombo_->addItem(QString::number(c), c);
	gifColorsCombo_->setToolTip(QStringLiteral(
		"Palette size. Fewer colours makes a markedly smaller file and shows banding "
		"on gradients."));
	gform->addRow(QStringLiteral("Colors"), gifColorsCombo_);
	gifDitherCheck_ = new QCheckBox(QStringLiteral("Dithering"), gifRow_);
	gifDitherCheck_->setChecked(true);
	gifDitherCheck_->setToolTip(QStringLiteral(
		"Scatters pixels to fake colours the palette lacks. Better on photos and "
		"gradients; off is smaller and cleaner on flat UI."));
	gform->addRow(QString(), gifDitherCheck_);
	gifLoopCheck_ = new QCheckBox(QStringLiteral("Loop forever"), gifRow_);
	gifLoopCheck_->setChecked(true);
	gform->addRow(QString(), gifLoopCheck_);
	left->addWidget(gifRow_);
	left->addStretch(1);

	// ---- Right: what is about to be written -------------------------------
	auto *rightBox = new QGroupBox(QStringLiteral("Output"), this);
	auto *rl = new QVBoxLayout(rightBox);
	if (!ctx_.previewFrame.isNull()) {
		auto *thumb = new QLabel(rightBox);
		thumb->setPixmap(QPixmap::fromImage(ctx_.previewFrame)
					 .scaled(200, 120, Qt::KeepAspectRatio,
						 Qt::SmoothTransformation));
		thumb->setAlignment(Qt::AlignCenter);
		thumb->setFrameShape(QFrame::StyledPanel);
		rl->addWidget(thumb);
	}
	summary_ = new QLabel(rightBox);
	summary_->setTextFormat(Qt::PlainText);
	summary_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	rl->addWidget(summary_);
	rl->addStretch(1);
	auto *sizeCap = new QLabel(QStringLiteral("Estimated size"), rightBox);
	sizeCap->setStyleSheet(QStringLiteral("color:#9a9fa8;"));
	rl->addWidget(sizeCap);
	sizeValue_ = new QLabel(rightBox);
	{
		QFont f = sizeValue_->font();
		f.setPointSizeF(f.pointSizeF() * 1.5);
		f.setBold(true);
		sizeValue_->setFont(f);
	}
	// Said once, here, rather than implied by a number that looks exact. Both
	// encoders are constant-QUALITY: the same settings on a static screen
	// recording and on handheld footage differ by more than 10x.
	sizeValue_->setToolTip(QStringLiteral(
		"An estimate from the resolution, frame rate and quality. Both encoders "
		"spend whatever bits the picture needs, so busy footage comes out larger "
		"than this and a static screen recording much smaller."));
	rl->addWidget(sizeValue_);
	rightBox->setMinimumWidth(220);
	columns->addWidget(rightBox, 2);

	// ---- Buttons ----------------------------------------------------------
	// Cancel on the left, the action on the right, and the action says what it
	// will do -- "Export GIF" rather than a bare "Export" you have to scroll up
	// to interpret.
	auto *buttons = new QDialogButtonBox(this);
	buttons->addButton(QDialogButtonBox::Cancel);
	exportBtn_ = buttons->addButton(QStringLiteral("Export"), QDialogButtonBox::AcceptRole);
	exportBtn_->setDefault(true);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	// Enter and Esc come free with QDialog; Ctrl+S is the one people try.
	auto *save = new QShortcut(QKeySequence::Save, this);
	connect(save, &QShortcut::activated, this, &QDialog::accept);

	loadRemembered();

	connect(formatCombo_, &QComboBox::currentIndexChanged, this,
		&ExportOptionsDialog::onFormatChanged);
	connect(resCombo_, &QComboBox::currentIndexChanged, this, &ExportOptionsDialog::refresh);
	connect(customW_, &QSpinBox::valueChanged, this, &ExportOptionsDialog::refresh);
	connect(customH_, &QSpinBox::valueChanged, this, &ExportOptionsDialog::refresh);
	connect(qualitySlider_, &QSlider::valueChanged, this, &ExportOptionsDialog::refresh);
	connect(audioCheck_, &QCheckBox::toggled, this, &ExportOptionsDialog::refresh);
	connect(gifFpsSpin_, &QSpinBox::valueChanged, this, &ExportOptionsDialog::refresh);
	connect(gifColorsCombo_, &QComboBox::currentIndexChanged, this,
		&ExportOptionsDialog::refresh);
	connect(gifDitherCheck_, &QCheckBox::toggled, this, &ExportOptionsDialog::refresh);
	connect(this, &QDialog::accepted, this, &ExportOptionsDialog::saveRemembered);

	onFormatChanged();
	setMinimumWidth(600);
}

void ExportOptionsDialog::browseFolder()
{
	const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Export to"),
							    folderEdit_->text());
	if (!d.isEmpty())
		folderEdit_->setText(QDir::toNativeSeparators(d));
}

void ExportOptionsDialog::onFormatChanged()
{
	const bool gif = format() == ClipExporter::Format::Gif;
	gifRow_->setVisible(gif);
	videoRow_->setVisible(!gif);
	exportBtn_->setText(QStringLiteral("Export %1").arg(formatName(format())));
	refresh();
}

double ExportOptionsDialog::effectiveFps() const
{
	return format() == ClipExporter::Format::Gif ? double(gifFpsSpin_->value()) : ctx_.fps;
}

QSize ExportOptionsDialog::outputSize() const
{
	const int data = resCombo_->currentData().toInt();
	if (data == 0)
		return QSize(); // original
	if (data > 0) {
		if (!ctx_.sourceSize.isValid())
			return QSize();
		const int w = int(std::lround(double(ctx_.sourceSize.width()) * data /
					      ctx_.sourceSize.height())) &
			      ~1;
		return QSize(std::max(2, w), data & ~1);
	}
	return QSize(customW_->value() & ~1, customH_->value() & ~1);
}

void ExportOptionsDialog::refresh()
{
	customRow_->setVisible(resCombo_->currentData().toInt() < 0);

	const QSize out = outputSize().isValid() ? outputSize() : ctx_.sourceSize;
	if (qualityLabel_)
		qualityLabel_->setText(QStringLiteral("CRF %1").arg(videoCrf()));

	const bool gif = format() == ClipExporter::Format::Gif;
	const bool audio = !gif && keepAudio() && ctx_.canKeepAudio &&
			   format() != ClipExporter::Format::WebM;
	summary_->setText(
		QStringLiteral("Format:      %1\nResolution:  %2\nFrame rate:  %3 fps\n"
			       "Duration:    %4 s\nAudio:       %5")
			.arg(formatName(format()))
			.arg(out.isValid() ? QStringLiteral("%1×%2").arg(out.width()).arg(out.height())
					   : QStringLiteral("—"))
			.arg(effectiveFps(), 0, 'g', 4)
			.arg(ctx_.seconds, 0, 'f', 1)
			.arg(audio ? QStringLiteral("Yes") : QStringLiteral("No")));

	sizeValue_->setText(QStringLiteral("≈ %1").arg(humanFileSize(estimatedBytes())));
}

qint64 ExportOptionsDialog::estimatedBytes() const
{
	const QSize out = outputSize().isValid() ? outputSize() : ctx_.sourceSize;
	ExportEstimateInput in;
	in.format = format();
	in.width = out.width();
	in.height = out.height();
	in.fps = effectiveFps();
	in.seconds = ctx_.seconds;
	in.videoCrf = videoCrf();
	in.keepAudio = keepAudio() && ctx_.canKeepAudio;
	in.gifColors = gifColors();
	in.gifDither = gifDither();
	return estimateExportBytes(in);
}

void ExportOptionsDialog::loadRemembered()
{
	QSettings s;
	const QString fld = s.value(QLatin1String(kGroup) + QStringLiteral("folder")).toString();
	if (!fld.isEmpty() && QDir(fld).exists())
		folderEdit_->setText(QDir::toNativeSeparators(fld));

	const int fmt = s.value(QLatin1String(kGroup) + QStringLiteral("format"), -1).toInt();
	const int fi = formatCombo_->findData(fmt);
	if (fi >= 0)
		formatCombo_->setCurrentIndex(fi);

	// The resolution is remembered as a HEIGHT rather than an index: the preset
	// list depends on the source, so index 2 means 480p for one clip and
	// nothing at all for the next.
	const int rh = s.value(QLatin1String(kGroup) + QStringLiteral("resHeight"), 0).toInt();
	const int ri = resCombo_->findData(rh);
	if (ri >= 0)
		resCombo_->setCurrentIndex(ri);

	qualitySlider_->setValue(
		sliderForCrf(s.value(QLatin1String(kGroup) + QStringLiteral("crf"), 23).toInt()));
	gifFpsSpin_->setValue(
		s.value(QLatin1String(kGroup) + QStringLiteral("gifFps"), 15).toInt());
	const int gc = gifColorsCombo_->findData(
		s.value(QLatin1String(kGroup) + QStringLiteral("gifColors"), 256).toInt());
	if (gc >= 0)
		gifColorsCombo_->setCurrentIndex(gc);
	gifDitherCheck_->setChecked(
		s.value(QLatin1String(kGroup) + QStringLiteral("gifDither"), true).toBool());
	gifLoopCheck_->setChecked(
		s.value(QLatin1String(kGroup) + QStringLiteral("gifLoop"), true).toBool());
	if (ctx_.canKeepAudio)
		audioCheck_->setChecked(
			s.value(QLatin1String(kGroup) + QStringLiteral("keepAudio"), true).toBool());
}

void ExportOptionsDialog::saveRemembered() const
{
	QSettings s;
	s.setValue(QLatin1String(kGroup) + QStringLiteral("folder"), folder());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("format"), int(format()));
	s.setValue(QLatin1String(kGroup) + QStringLiteral("resHeight"),
		   resCombo_->currentData().toInt());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("crf"), videoCrf());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("gifFps"), gifFps());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("gifColors"), gifColors());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("gifDither"), gifDither());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("gifLoop"), gifLoop());
	s.setValue(QLatin1String(kGroup) + QStringLiteral("keepAudio"), keepAudio());
}

QString ExportOptionsDialog::fileName() const
{
	return nameEdit_->text().trimmed();
}

QString ExportOptionsDialog::folder() const
{
	const QString f = folderEdit_->text().trimmed();
	return f.isEmpty() ? ctx_.defaultFolder : QDir::fromNativeSeparators(f);
}

QString ExportOptionsDialog::outputPath() const
{
	QString name = fileName();
	if (name.isEmpty())
		name = ctx_.defaultName;
	return folder() + QLatin1Char('/') + name + QLatin1Char('.') +
	       ClipExporter::extensionFor(format());
}

ClipExporter::Format ExportOptionsDialog::format() const
{
	return ClipExporter::Format(formatCombo_->currentData().toInt());
}

int ExportOptionsDialog::gifFps() const
{
	return gifFpsSpin_->value();
}

int ExportOptionsDialog::gifColors() const
{
	return gifColorsCombo_->currentData().toInt();
}

bool ExportOptionsDialog::gifDither() const
{
	return gifDitherCheck_->isChecked();
}

bool ExportOptionsDialog::gifLoop() const
{
	return gifLoopCheck_->isChecked();
}

int ExportOptionsDialog::videoCrf() const
{
	return crfForSlider(qualitySlider_->value());
}

bool ExportOptionsDialog::keepAudio() const
{
	return audioCheck_->isChecked() && ctx_.canKeepAudio;
}

} // namespace harpia
