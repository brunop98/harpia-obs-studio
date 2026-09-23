#include "SubtitleDialog.hpp"

#include "OpenAiTranscriber.hpp"
#include "SecretStore.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace harpia {

namespace {
QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
const QString kGrp = QStringLiteral("subtitles/");
} // namespace

QVector<QPair<QString, QString>> SubtitleDialog::languages()
{
	return {{QStringLiteral("Detect automatically"), QString()},
		{QStringLiteral("Portuguese"), QStringLiteral("pt")},
		{QStringLiteral("English"), QStringLiteral("en")},
		{QStringLiteral("Spanish"), QStringLiteral("es")},
		{QStringLiteral("French"), QStringLiteral("fr")},
		{QStringLiteral("German"), QStringLiteral("de")},
		{QStringLiteral("Italian"), QStringLiteral("it")},
		{QStringLiteral("Japanese"), QStringLiteral("ja")},
		{QStringLiteral("Korean"), QStringLiteral("ko")},
		{QStringLiteral("Chinese"), QStringLiteral("zh")},
		{QStringLiteral("Russian"), QStringLiteral("ru")},
		{QStringLiteral("Arabic"), QStringLiteral("ar")},
		{QStringLiteral("Hindi"), QStringLiteral("hi")},
		{QStringLiteral("Dutch"), QStringLiteral("nl")},
		{QStringLiteral("Polish"), QStringLiteral("pl")},
		{QStringLiteral("Turkish"), QStringLiteral("tr")}};
}

SubtitleDialog::SubtitleDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Subtitles"));
	setModal(false);
	transcriber_ = new OpenAiTranscriber(this);
	buildUi();
	loadSettings();

	connect(transcriber_, &OpenAiTranscriber::progress, this, [this](const QString &s) {
		status_->setText(QStringLiteral("%1  (%2)").arg(s, targetIdx_ >= 0 && targetIdx_ < targets_.size()
									     ? targets_[targetIdx_].name
									     : QString()));
	});
	connect(transcriber_, &OpenAiTranscriber::failed, this, [this](const QString &why) { finishJob(why); });
	connect(transcriber_, &OpenAiTranscriber::finished, this, [this](const Transcript &t) {
		// Shift this chunk's words into source time and keep going.
		const qint64 off = chunks_[chunkIdx_].offsetMs;
		for (ClipWordTime w : t.words) {
			w.startMs += off;
			w.endMs += off;
			words_.append(w);
		}
		++chunkIdx_;
		nextChunk();
	});
}

SubtitleDialog::~SubtitleDialog()
{
	if (!workDir_.isEmpty())
		QDir(workDir_).removeRecursively();
}

void SubtitleDialog::buildUi()
{
	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(14, 12, 14, 12);
	lay->setSpacing(10);

	auto *intro = new QLabel(QStringLiteral(
		"Turns the speech in the selected clips into caption clips on a Subtitles lane, "
		"lined up with the audio. Edit any caption's text in the Inspector afterwards; "
		"Ctrl+Z removes the whole batch."), this);
	intro->setWordWrap(true);
	intro->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(intro);

	// ---- Service ----
	auto *svc = new QGroupBox(QStringLiteral("Speech to text (OpenAI)"), this);
	auto *sf = new QFormLayout(svc);
	sf->setHorizontalSpacing(10);
	language_ = new QComboBox(svc);
	for (const auto &l : languages())
		language_->addItem(l.first, l.second);
	language_->setToolTip(QStringLiteral(
		"Telling the model the language makes it faster and more accurate. Detect "
		"works, but can guess wrong on a short clip or one with music under it."));
	sf->addRow(QStringLiteral("Language"), language_);
	auto *keyRow = new QHBoxLayout;
	apiKey_ = new QLineEdit(svc);
	apiKey_->setEchoMode(QLineEdit::Password);
	apiKey_->setPlaceholderText(QStringLiteral("sk-…  (paste your OpenAI API key)"));
	apiKey_->setToolTip(QStringLiteral(
		"Your own OpenAI API key. Kept on this computer only -- sealed with Windows' "
		"credential protection, never written into a project. Transcription is billed to "
		"your OpenAI account by the minute of audio."));
	keyRow->addWidget(apiKey_, 1);
	auto *saveKey = new QPushButton(QStringLiteral("Save key"), svc);
	connect(saveKey, &QPushButton::clicked, this, [this]() {
		SecretStore::saveApiKey(apiKey_->text());
		apiKey_->clear();
		const QString k = SecretStore::loadApiKey();
		keyState_->setText(k.isEmpty() ? QStringLiteral("No key saved.")
					       : QStringLiteral("Key saved: %1").arg(SecretStore::maskedKey(k)));
	});
	keyRow->addWidget(saveKey);
	sf->addRow(QStringLiteral("API key"), keyRow);
	keyState_ = new QLabel(svc);
	keyState_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	sf->addRow(QString(), keyState_);
	lay->addWidget(svc);

	// ---- Grouping ----
	auto *grp = new QGroupBox(QStringLiteral("Words per caption"), this);
	auto *gf = new QFormLayout(grp);
	gf->setHorizontalSpacing(10);
	maxWords_ = new QSpinBox(grp);
	maxWords_->setRange(1, 20);
	maxWords_->setToolTip(QStringLiteral("1 makes a caption per word. 3 to 5 reads like most social captions."));
	gf->addRow(QStringLiteral("At most words"), maxWords_);
	maxSeconds_ = new QDoubleSpinBox(grp);
	maxSeconds_->setRange(0.5, 15.0);
	maxSeconds_->setDecimals(1);
	maxSeconds_->setSingleStep(0.5);
	maxSeconds_->setSuffix(QStringLiteral(" s"));
	gf->addRow(QStringLiteral("At most on screen"), maxSeconds_);
	pauseMs_ = new QSpinBox(grp);
	pauseMs_->setRange(0, 3000);
	pauseMs_->setSingleStep(50);
	pauseMs_->setSuffix(QStringLiteral(" ms"));
	pauseMs_->setToolTip(QStringLiteral("A silence at least this long starts a new caption. 0 turns it off."));
	gf->addRow(QStringLiteral("Break on a pause of"), pauseMs_);
	maxChars_ = new QSpinBox(grp);
	maxChars_->setRange(6, 120);
	gf->addRow(QStringLiteral("At most characters"), maxChars_);
	lay->addWidget(grp);

	// ---- Look ----
	auto *lk = new QGroupBox(QStringLiteral("Appearance (everything else in the Inspector afterwards)"), this);
	auto *lf = new QFormLayout(lk);
	lf->setHorizontalSpacing(10);
	mode_ = new QComboBox(lk);
	mode_->addItem(QStringLiteral("Whole phrase"));
	mode_->addItem(QStringLiteral("One word at a time"));
	mode_->addItem(QStringLiteral("Build up word by word"));
	lf->addRow(QStringLiteral("Show"), mode_);
	position_ = new QComboBox(lk);
	position_->addItem(QStringLiteral("Bottom"));
	position_->addItem(QStringLiteral("Middle"));
	position_->addItem(QStringLiteral("Top"));
	lf->addRow(QStringLiteral("Position"), position_);
	fontPx_ = new QSpinBox(lk);
	fontPx_->setRange(16, 240);
	fontPx_->setSuffix(QStringLiteral(" px"));
	lf->addRow(QStringLiteral("Text size"), fontPx_);
	bold_ = new QCheckBox(QStringLiteral("Bold"), lk);
	box_ = new QCheckBox(QStringLiteral("Dark box behind the text"), lk);
	auto *flags = new QHBoxLayout;
	flags->addWidget(bold_);
	flags->addWidget(box_);
	flags->addStretch(1);
	lf->addRow(QString(), flags);
	lay->addWidget(lk);

	// ---- Go ----
	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(status_);
	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	connect(cancel_, &QPushButton::clicked, this, [this]() {
		if (transcriber_->busy())
			transcriber_->cancel();
		else
			reject();
	});
	btns->addWidget(cancel_);
	go_ = new QPushButton(QStringLiteral("Generate subtitles"), this);
	go_->setDefault(true);
	go_->setMinimumHeight(32);
	connect(go_, &QPushButton::clicked, this, &SubtitleDialog::startJob);
	btns->addWidget(go_);
	lay->addLayout(btns);
	setMinimumWidth(460);
}

void SubtitleDialog::loadSettings()
{
	QSettings s = settings();
	const QString lang = s.value(kGrp + QStringLiteral("language"), QStringLiteral("pt")).toString();
	for (int i = 0; i < language_->count(); ++i)
		if (language_->itemData(i).toString() == lang)
			language_->setCurrentIndex(i);
	maxWords_->setValue(s.value(kGrp + QStringLiteral("maxWords"), 3).toInt());
	maxSeconds_->setValue(s.value(kGrp + QStringLiteral("maxSeconds"), 2.5).toDouble());
	pauseMs_->setValue(s.value(kGrp + QStringLiteral("pauseMs"), 400).toInt());
	maxChars_->setValue(s.value(kGrp + QStringLiteral("maxChars"), 32).toInt());
	mode_->setCurrentIndex(std::clamp(s.value(kGrp + QStringLiteral("mode"), 0).toInt(), 0, 2));
	position_->setCurrentIndex(std::clamp(s.value(kGrp + QStringLiteral("position"), 0).toInt(), 0, 2));
	fontPx_->setValue(s.value(kGrp + QStringLiteral("fontPx"), 64).toInt());
	bold_->setChecked(s.value(kGrp + QStringLiteral("bold"), true).toBool());
	box_->setChecked(s.value(kGrp + QStringLiteral("box"), false).toBool());
	const QString k = SecretStore::loadApiKey();
	keyState_->setText(k.isEmpty() ? QStringLiteral("No key saved yet. Paste one and press Save key.")
				       : QStringLiteral("Key saved: %1").arg(SecretStore::maskedKey(k)));
}

void SubtitleDialog::saveSettings()
{
	QSettings s = settings();
	s.setValue(kGrp + QStringLiteral("language"), languageCode());
	s.setValue(kGrp + QStringLiteral("maxWords"), maxWords_->value());
	s.setValue(kGrp + QStringLiteral("maxSeconds"), maxSeconds_->value());
	s.setValue(kGrp + QStringLiteral("pauseMs"), pauseMs_->value());
	s.setValue(kGrp + QStringLiteral("maxChars"), maxChars_->value());
	s.setValue(kGrp + QStringLiteral("mode"), mode_->currentIndex());
	s.setValue(kGrp + QStringLiteral("position"), position_->currentIndex());
	s.setValue(kGrp + QStringLiteral("fontPx"), fontPx_->value());
	s.setValue(kGrp + QStringLiteral("bold"), bold_->isChecked());
	s.setValue(kGrp + QStringLiteral("box"), box_->isChecked());
}

void SubtitleDialog::setTargets(const QVector<Target> &targets)
{
	targets_ = targets;
	qint64 ms = 0;
	for (const Target &t : targets_)
		ms += t.media.srcLenMs();
	status_->setText(targets_.isEmpty()
				 ? QStringLiteral("Select one or more video or audio clips on the timeline first.")
				 : QStringLiteral("%1 clip(s), %2 min %3 s of audio to transcribe.")
					   .arg(targets_.size())
					   .arg(ms / 60000)
					   .arg((ms / 1000) % 60));
	go_->setEnabled(!targets_.isEmpty());
}

GroupRule SubtitleDialog::groupRule() const
{
	GroupRule r;
	r.maxWords = maxWords_->value();
	r.maxDurationMs = qint64(maxSeconds_->value() * 1000.0);
	r.pauseMs = pauseMs_->value();
	r.maxChars = maxChars_->value();
	return r;
}

SubtitleLook SubtitleDialog::look() const
{
	SubtitleLook l;
	l.mode = SubtitleMode(mode_->currentIndex());
	l.position = SubtitleLook::Position(position_->currentIndex());
	l.style.fontPx = fontPx_->value();
	l.style.bold = bold_->isChecked();
	l.style.boxEnabled = box_->isChecked();
	return l;
}

QString SubtitleDialog::languageCode() const
{
	return language_->currentData().toString();
}

void SubtitleDialog::setBusy(bool on)
{
	go_->setEnabled(!on && !targets_.isEmpty());
	language_->setEnabled(!on);
	cancel_->setText(on ? QStringLiteral("Stop") : QStringLiteral("Cancel"));
}

void SubtitleDialog::startJob()
{
	if (targets_.isEmpty())
		return;
	// A key typed but not saved still works for this run; saved is the norm.
	QString key = apiKey_->text().trimmed();
	if (key.isEmpty())
		key = SecretStore::loadApiKey();
	if (key.isEmpty()) {
		status_->setText(QStringLiteral("Paste your OpenAI API key first."));
		apiKey_->setFocus();
		return;
	}
	saveSettings();
	setBusy(true);
	out_.clear();
	wordsTotal_ = 0;
	targetIdx_ = -1;
	if (workDir_.isEmpty())
		workDir_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
			   QStringLiteral("/harpia-speech-%1").arg(QCoreApplication::applicationPid());
	// Advance to the first target.
	chunkIdx_ = 0;
	chunks_.clear();
	words_.clear();
	nextChunk();
}

void SubtitleDialog::nextChunk()
{
	// Finished the current target's chunks: build its captions, move on.
	if (targetIdx_ >= 0 && chunkIdx_ >= chunks_.size()) {
		const Target &t = targets_[targetIdx_];
		const QVector<CaptionPiece> pieces = groupWords(words_, groupRule());
		const QVector<TlClip> clips = buildSubtitleClips(pieces, t.media, look());
		out_ += clips;
		wordsTotal_ += words_.size();
		for (const SpeechAudioChunk &c : chunks_)
			QFile::remove(c.wavPath);
		chunks_.clear();
		words_.clear();
		chunkIdx_ = 0;
	}
	if (chunks_.isEmpty()) {
		++targetIdx_;
		if (targetIdx_ >= targets_.size()) {
			finishJob(QString());
			return;
		}
		const Target &t = targets_[targetIdx_];
		status_->setText(QStringLiteral("Preparing the audio of %1…").arg(t.name));
		QString err;
		chunks_ = AudioForSpeech::prepare(t.path, t.media.srcStartMs, t.media.srcEndMs, workDir_, &err);
		if (chunks_.isEmpty()) {
			finishJob(QStringLiteral("%1: %2").arg(t.name, err));
			return;
		}
		chunkIdx_ = 0;
	}
	QString key = apiKey_->text().trimmed();
	if (key.isEmpty())
		key = SecretStore::loadApiKey();
	OpenAiTranscriber::Job job;
	job.wavPath = chunks_[chunkIdx_].wavPath;
	job.apiKey = key;
	job.language = languageCode();
	status_->setText(QStringLiteral("Sending %1 (part %2 of %3)…")
				 .arg(targets_[targetIdx_].name)
				 .arg(chunkIdx_ + 1)
				 .arg(chunks_.size()));
	transcriber_->transcribe(job);
}

void SubtitleDialog::finishJob(const QString &error)
{
	for (const SpeechAudioChunk &c : chunks_)
		QFile::remove(c.wavPath);
	chunks_.clear();
	setBusy(false);
	if (!error.isEmpty()) {
		status_->setText(QStringLiteral("Stopped: %1").arg(error));
		return;
	}
	if (out_.isEmpty()) {
		status_->setText(QStringLiteral("No speech was found in the selected clips."));
		return;
	}
	const QString summary = QStringLiteral("%1 captions from %2 words").arg(out_.size()).arg(wordsTotal_);
	status_->setText(QStringLiteral("Done: %1. They are on the Subtitles lane; Ctrl+Z removes them.").arg(summary));
	emit captionsReady(out_, summary);
}

} // namespace harpia
