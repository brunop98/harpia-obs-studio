#include "MediaFileDetailsDialog.hpp"

#include "library/ClipLibrary.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QThreadPool>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace harpia {
namespace {

struct ProbeResult {
	QString duration = QStringLiteral("Unknown");
	QString resolution = QStringLiteral("No video track");
	QString codec = QStringLiteral("No video track");
	QString frameRate = QStringLiteral("Not applicable");
	QString audio = QStringLiteral("No audio track");
	QString status;
};

struct ProbeControl {
	std::shared_ptr<std::atomic<bool>> canceled;
	std::chrono::steady_clock::time_point deadline;
};

int interrupted(void *opaque)
{
	const auto *control = static_cast<const ProbeControl *>(opaque);
	return control->canceled->load() || std::chrono::steady_clock::now() >= control->deadline;
}

ProbeResult probe(const QString &path, const std::shared_ptr<std::atomic<bool>> &canceled)
{
	ProbeResult result;
	ProbeControl control{canceled, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
	AVFormatContext *format = avformat_alloc_context();
	if (!format) {
		result.status = QStringLiteral("Could not read media details. File information is still available.");
		return result;
	}
	format->interrupt_callback = {interrupted, &control};
	format->probesize = 4 * 1024 * 1024;
	format->max_analyze_duration = 2 * AV_TIME_BASE;
	AVDictionary *options = nullptr;
	av_dict_set(&options, "protocol_whitelist", "file", 0);
	const QByteArray encodedPath = path.toUtf8();
	const int opened = avformat_open_input(&format, encodedPath.constData(), nullptr, &options);
	av_dict_free(&options);
	if (opened < 0) {
		avformat_close_input(&format);
		result.status = QStringLiteral("Could not read this file's media details. File information is still available.");
		return result;
	}
	const int inspected = avformat_find_stream_info(format, nullptr);
	if (format->duration > 0)
		result.duration = ClipInfo::durationString(format->duration / (AV_TIME_BASE / 1000));
	for (unsigned int i = 0; i < format->nb_streams; ++i) {
		const AVStream *stream = format->streams[i];
		const AVCodecParameters *parameters = stream->codecpar;
		if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && result.resolution == QStringLiteral("No video track")) {
			if (parameters->width > 0 && parameters->height > 0)
				result.resolution = QStringLiteral("%1 × %2").arg(parameters->width).arg(parameters->height);
			else
				result.resolution = QStringLiteral("Unknown");
			result.codec = QString::fromUtf8(avcodec_get_name(parameters->codec_id));
			const double fps = av_q2d(stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate);
			result.frameRate = std::isfinite(fps) && fps > 0.0
				? QStringLiteral("%1 fps").arg(fps, 0, 'f', 3)
				: QStringLiteral("Unknown");
		} else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO && result.audio == QStringLiteral("No audio track")) {
			result.audio = QStringLiteral("%1 · %2 Hz · %3 channel%4")
				.arg(QString::fromUtf8(avcodec_get_name(parameters->codec_id)))
				.arg(parameters->sample_rate).arg(parameters->ch_layout.nb_channels)
				.arg(parameters->ch_layout.nb_channels == 1 ? QString() : QStringLiteral("s"));
		}
	}
	result.status = inspected >= 0 ? QStringLiteral("Media details read.")
		: QStringLiteral("Some media details could not be read. Available information is shown above.");
	avformat_close_input(&format);
	return result;
}

QString dateText(const QDateTime &date)
{
	return date.isValid() ? QLocale().toString(date.toLocalTime(), QLocale::ShortFormat) : QStringLiteral("Unknown");
}

} // namespace

MediaFileDetailsDialog::MediaFileDetailsDialog(const QString &filePath, QWidget *parent)
	: QDialog(parent), canceled_(std::make_shared<std::atomic<bool>>(false))
{
	setObjectName(QStringLiteral("mediaFileDetailsDialog"));
	setWindowTitle(QStringLiteral("Video details"));
	setSizeGripEnabled(true);
	resize(620, 460);
	const QFileInfo file(filePath);
	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout;
	layout->addLayout(form);
	auto field = [this, form](const QString &label, const QString &name, const QString &value) {
		auto *edit = new QLineEdit(value, this);
		edit->setObjectName(name);
		edit->setReadOnly(true);
		edit->setCursorPosition(0);
		form->addRow(label, edit);
		fields_.append({label, edit});
		return edit;
	};
	field(QStringLiteral("Name"), QStringLiteral("detailsFileName"), file.fileName());
	field(QStringLiteral("File path"), QStringLiteral("detailsFilePath"), QDir::toNativeSeparators(file.absoluteFilePath()));
	field(QStringLiteral("Folder"), QStringLiteral("detailsFolderPath"), QDir::toNativeSeparators(file.absolutePath()));
	field(QStringLiteral("File type"), QStringLiteral("detailsFileType"), file.suffix().toUpper());
	field(QStringLiteral("Size"), QStringLiteral("detailsFileSize"), file.isFile()
		? QStringLiteral("%1 (%2 bytes)").arg(QLocale().formattedDataSize(file.size(), 2), QLocale().toString(file.size()))
		: QStringLiteral("Unavailable"));
	field(QStringLiteral("Modified"), QStringLiteral("detailsModified"), dateText(file.lastModified()));
	field(QStringLiteral("Created"), QStringLiteral("detailsCreated"), dateText(file.birthTime()));
	duration_ = field(QStringLiteral("Duration"), QStringLiteral("detailsDuration"), QStringLiteral("Reading…"));
	resolution_ = field(QStringLiteral("Resolution"), QStringLiteral("detailsResolution"), QStringLiteral("Reading…"));
	codec_ = field(QStringLiteral("Video codec"), QStringLiteral("detailsCodec"), QStringLiteral("Reading…"));
	frameRate_ = field(QStringLiteral("Frame rate"), QStringLiteral("detailsFrameRate"), QStringLiteral("Reading…"));
	audio_ = field(QStringLiteral("Audio"), QStringLiteral("detailsAudio"), QStringLiteral("Reading…"));
	status_ = new QLabel(QStringLiteral("Reading media details…"), this);
	status_->setObjectName(QStringLiteral("detailsStatus"));
	status_->setWordWrap(true);
	layout->addWidget(status_);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	auto *copy = buttons->addButton(QStringLiteral("Copy details"), QDialogButtonBox::ActionRole);
	copy->setObjectName(QStringLiteral("copyMediaDetails"));
	connect(copy, &QPushButton::clicked, this, [this]() {
		QStringList lines;
		for (const auto &entry : fields_)
			lines << QStringLiteral("%1: %2").arg(entry.first, entry.second->text());
		QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
	if (!file.isFile()) {
		for (QLineEdit *edit : {duration_, resolution_, codec_, frameRate_, audio_})
			edit->setText(QStringLiteral("Unavailable"));
		status_->setText(QStringLiteral("This file is no longer available at the recorded path."));
		return;
	}
	QPointer<MediaFileDetailsDialog> guard(this);
	const auto canceled = canceled_;
	const QString path = file.absoluteFilePath();
	QThreadPool::globalInstance()->start(QRunnable::create([guard, canceled, path]() {
		if (canceled->load())
			return;
		const ProbeResult result = probe(path, canceled);
		QMetaObject::invokeMethod(qApp, [guard, result]() {
			if (!guard)
				return;
			guard->duration_->setText(result.duration);
			guard->resolution_->setText(result.resolution);
			guard->codec_->setText(result.codec);
			guard->frameRate_->setText(result.frameRate);
			guard->audio_->setText(result.audio);
			guard->status_->setText(result.status);
		}, Qt::QueuedConnection);
	}));
}

MediaFileDetailsDialog::~MediaFileDetailsDialog()
{
	canceled_->store(true);
}

} // namespace harpia
