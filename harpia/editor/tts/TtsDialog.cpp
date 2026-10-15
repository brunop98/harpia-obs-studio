#include "TtsDialog.hpp"

#include "TtsAudio.hpp"
#include "TtsSynthesizer.hpp"

#include "../../ui/EditorLog.hpp"
#include "../../ui/InfoHint.hpp"
#include "../ProjectPaths.hpp"
#include "../VoiceoverMixer.hpp"
#include "../subtitles/AudioForSpeech.hpp"
#include "../subtitles/SecretStore.hpp"
#include "../subtitles/SpeechTranscriber.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace harpia {

namespace {
QSettings settings()
{
	return QSettings(QStringLiteral("Harpia"), QStringLiteral("Recorder"));
}
const QString kGrp = QStringLiteral("tts/");
const QString kSubs = QStringLiteral("subtitles/"); // the caption settings, shared
const QString kWhere = QStringLiteral("Text to speech");
constexpr qint64 kPieceGapMs = 250; // a breath between the pieces of a long text

// The key a service uses. OpenAI, ElevenLabs and Deepgram keys are the same
// account keys the Subtitles window keeps, so a key pasted in either window
// works in both; Google's is its own.
const QString kGoogleKey = QStringLiteral("tts/apiKey_google");

QString loadKey(TtsProvider p)
{
	switch (p) {
	case TtsProvider::OpenAI: return SecretStore::loadApiKey(SpeechProvider::OpenAI);
	case TtsProvider::ElevenLabs: return SecretStore::loadApiKey(SpeechProvider::ElevenLabs);
	case TtsProvider::Deepgram: return SecretStore::loadApiKey(SpeechProvider::Deepgram);
	case TtsProvider::Google: return SecretStore::loadNamedKey(kGoogleKey, QStringLiteral("Google Cloud"));
	}
	return QString();
}

bool saveKey(TtsProvider p, const QString &key)
{
	switch (p) {
	case TtsProvider::OpenAI: return SecretStore::saveApiKey(SpeechProvider::OpenAI, key);
	case TtsProvider::ElevenLabs: return SecretStore::saveApiKey(SpeechProvider::ElevenLabs, key);
	case TtsProvider::Deepgram: return SecretStore::saveApiKey(SpeechProvider::Deepgram, key);
	case TtsProvider::Google: return SecretStore::saveNamedKey(kGoogleKey, QStringLiteral("Google Cloud"), key);
	}
	return false;
}

// The services that give audio only are timed by transcribing it back.
SpeechProvider transcriberFor(TtsProvider p)
{
	return p == TtsProvider::Deepgram ? SpeechProvider::Deepgram : SpeechProvider::OpenAI;
}
} // namespace

TtsDialog::TtsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Text to speech"));
	setModal(false);
	synth_ = new TtsSynthesizer(this);
	transcriber_ = new SpeechTranscriber(this);
	buildUi();
	loadSettings();

	connect(synth_, &TtsSynthesizer::synthesized, this, &TtsDialog::onSynthesized);
	connect(synth_, &TtsSynthesizer::voicesListed, this, [this](const QVector<TtsVoice> &v) {
		loadVoices_->setEnabled(true);
		setVoices(v, voice());
		status_->setText(QStringLiteral("%1 voices from your %2 account.").arg(v.size()).arg(ttsProviderName(provider())));
	});
	connect(synth_, &TtsSynthesizer::failed, this, [this](const QString &why) {
		if (!running_ && why == QLatin1String("cancelled"))
			return; // Stop already said so
		if (running_) {
			finishJob(why);
		} else {
			loadVoices_->setEnabled(true);
			status_->setText(QStringLiteral("Could not load voices: %1").arg(why));
		}
	});
	connect(transcriber_, &SpeechTranscriber::finished, this, &TtsDialog::onTranscribed);
	connect(transcriber_, &SpeechTranscriber::failed, this, [this](const QString &why) {
		if (!running_)
			return;
		finishJob(QStringLiteral("timing the words: %1").arg(why));
	});
	connect(transcriber_, &SpeechTranscriber::progress, this, [this](const QString &s) {
		status_->setText(QStringLiteral("Timing the words of part %1 of %2: %3")
					 .arg(pieceIdx_ + 1)
					 .arg(pieces_.size())
					 .arg(s));
	});
}

TtsDialog::~TtsDialog()
{
	if (!workDir_.isEmpty())
		QDir(workDir_).removeRecursively();
}

void TtsDialog::buildUi()
{
	auto *outer = new QHBoxLayout(this);
	outer->setContentsMargins(14, 12, 14, 12);
	outer->setSpacing(14);

	// ---- The text, on the left ----
	auto *left = new QVBoxLayout;
	left->setSpacing(6);
	auto *tTitle = new QLabel(QStringLiteral("Text"), this);
	tTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
	left->addWidget(tTitle);
	text_ = new QPlainTextEdit(this);
	text_->setPlaceholderText(QStringLiteral("Paste or type what should be said, or open a text file below.\n\n"
						 "Each word becomes part of a caption, shown while it is spoken."));
	text_->setMinimumSize(320, 300);
	left->addWidget(text_, 1);
	auto *tRow = new QHBoxLayout;
	auto *open = new QPushButton(QStringLiteral("Open text file…"), this);
	open->setToolTip(QStringLiteral("A .txt (or .srt: its numbers and times are left out). It replaces the text above."));
	connect(open, &QPushButton::clicked, this, &TtsDialog::openTextFile);
	tRow->addWidget(open);
	count_ = new QLabel(this);
	count_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	tRow->addWidget(count_, 1);
	left->addLayout(tRow);
	connect(text_, &QPlainTextEdit::textChanged, this, &TtsDialog::refreshCount);
	outer->addLayout(left, 1);

	// ---- Settings, on the right ----
	auto *lay = new QVBoxLayout;
	lay->setSpacing(10);
	outer->addLayout(lay);
	lay->addWidget(infoHint(QStringLiteral(
				   "Turns the text into a narration and captions at the playhead: an audio clip "
				   "and caption clips, linked so they move together. The audio is kept in "
				   "Movies/Harpia/Speech. Ctrl+Z removes them."),
			       this),
		       0, Qt::AlignLeft);

	auto *svc = new QGroupBox(QStringLiteral("Voice"), this);
	auto *sf = new QFormLayout(svc);
	sf->setHorizontalSpacing(10);
	provider_ = new QComboBox(svc);
	for (const TtsProviderInfo &i : ttsProviders())
		provider_->addItem(QString::fromUtf8(i.name), QString::fromLatin1(i.id));
	provider_->setToolTip(QStringLiteral("Which service speaks the text. Each uses its own API key and bills its own account."));
	sf->addRow(QStringLiteral("Service"), provider_);
	about_ = new QLabel(svc);
	about_->setWordWrap(true);
	about_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	sf->addRow(QString(), about_);
	language_ = new QComboBox(svc);
	for (const auto &l : ttsLanguages())
		language_->addItem(l.first, l.second);
	language_->setToolTip(QStringLiteral("The language of the text: Google picks its voices by it, and the word "
					     "timing uses it as a hint."));
	sf->addRow(QStringLiteral("Language"), language_);
	auto *vRow = new QHBoxLayout;
	voice_ = new QComboBox(svc);
	voice_->setEditable(true);
	voice_->setInsertPolicy(QComboBox::NoInsert);
	voice_->setToolTip(QStringLiteral("Pick a voice, or type a voice id the service gave you."));
	vRow->addWidget(voice_, 1);
	loadVoices_ = new QPushButton(QStringLiteral("Load my voices"), svc);
	loadVoices_->setToolTip(QStringLiteral("Ask the service for every voice your account can use."));
	connect(loadVoices_, &QPushButton::clicked, this, &TtsDialog::loadVoices);
	vRow->addWidget(loadVoices_);
	sf->addRow(QStringLiteral("Voice"), vRow);
	style_ = new QLineEdit(svc);
	style_->setPlaceholderText(QStringLiteral("e.g. Calm and warm, like a documentary"));
	style_->setToolTip(QStringLiteral("How the voice should sound: tone, pace, accent, emotion."));
	styleLabel_ = new QLabel(QStringLiteral("Style"), svc);
	sf->addRow(styleLabel_, style_);
	speed_ = new QDoubleSpinBox(svc);
	speed_->setDecimals(2);
	speed_->setSingleStep(0.05);
	speed_->setSuffix(QStringLiteral(" ×"));
	speedLabel_ = new QLabel(QStringLiteral("Speed"), svc);
	sf->addRow(speedLabel_, speed_);
	auto *keyRow = new QHBoxLayout;
	apiKey_ = new QLineEdit(svc);
	apiKey_->setEchoMode(QLineEdit::Password);
	keyRow->addWidget(apiKey_, 1);
	auto *saveKeyBtn = new QPushButton(QStringLiteral("Save key"), svc);
	connect(saveKeyBtn, &QPushButton::clicked, this, [this]() { storeTypedKey(provider()); });
	connect(apiKey_, &QLineEdit::editingFinished, this, [this]() { storeTypedKey(keyFor_); });
	keyRow->addWidget(saveKeyBtn);
	sf->addRow(QStringLiteral("API key"), keyRow);
	getKey_ = new QLabel(svc);
	getKey_->setTextFormat(Qt::RichText);
	getKey_->setOpenExternalLinks(true);
	getKey_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	getKey_->setWordWrap(true);
	sf->addRow(QString(), getKey_);
	keyState_ = new QLabel(svc);
	keyState_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	keyState_->setWordWrap(true);
	sf->addRow(QString(), keyState_);
	connect(provider_, &QComboBox::currentIndexChanged, this, [this]() {
		storeTypedKey(keyFor_); // a key typed for the service being left is ITS key
		apiKey_->clear();
		refreshProvider();
	});
	connect(language_, &QComboBox::currentIndexChanged, this, [this]() {
		// Google's and Deepgram's voices are per language.
		if (provider() == TtsProvider::Google || provider() == TtsProvider::Deepgram)
			setVoices(ttsDefaultVoices(provider(), languageCode()), QString());
	});
	lay->addWidget(svc);

	// ---- Captions: the Subtitles window's settings ----
	auto *cap = new QGroupBox(QStringLiteral("Captions (shared with the Subtitles window)"), this);
	auto *cf = new QFormLayout(cap);
	cf->setHorizontalSpacing(10);
	maxWords_ = new QSpinBox(cap);
	maxWords_->setRange(1, 20);
	maxWords_->setToolTip(QStringLiteral("1 makes a caption per word. 3 to 5 reads like most social captions."));
	cf->addRow(QStringLiteral("At most words"), maxWords_);
	mode_ = new QComboBox(cap);
	mode_->addItem(QStringLiteral("Whole phrase"));
	mode_->addItem(QStringLiteral("One word at a time"));
	mode_->addItem(QStringLiteral("Build up word by word"));
	cf->addRow(QStringLiteral("Show"), mode_);
	position_ = new QComboBox(cap);
	position_->addItem(QStringLiteral("Bottom"));
	position_->addItem(QStringLiteral("Middle"));
	position_->addItem(QStringLiteral("Top"));
	cf->addRow(QStringLiteral("Position"), position_);
	fontPx_ = new QSpinBox(cap);
	fontPx_->setRange(16, 240);
	fontPx_->setSuffix(QStringLiteral(" px"));
	cf->addRow(QStringLiteral("Text size"), fontPx_);
	lay->addWidget(cap);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setStyleSheet(QStringLiteral("color:#9aa0a6;"));
	lay->addWidget(status_);
	lay->addStretch(1);
	auto *btns = new QHBoxLayout;
	btns->addStretch(1);
	cancel_ = new QPushButton(QStringLiteral("Cancel"), this);
	connect(cancel_, &QPushButton::clicked, this, [this]() {
		if (running_) {
			synth_->cancel();
			transcriber_->cancel();
			finishJob(QStringLiteral("cancelled"));
		} else {
			reject();
		}
	});
	btns->addWidget(cancel_);
	go_ = new QPushButton(QStringLiteral("Generate speech"), this);
	go_->setDefault(true);
	go_->setMinimumHeight(32);
	connect(go_, &QPushButton::clicked, this, &TtsDialog::startJob);
	btns->addWidget(go_);
	lay->addLayout(btns);
	setMinimumWidth(820);
}

void TtsDialog::loadSettings()
{
	QSettings s = settings();
	const QString lang = s.value(kGrp + QStringLiteral("language"),
				     s.value(kSubs + QStringLiteral("language"), QStringLiteral("pt")))
				     .toString();
	const int li = language_->findData(lang);
	language_->setCurrentIndex(std::max(0, li));
	style_->setText(s.value(kGrp + QStringLiteral("style")).toString());
	maxWords_->setValue(s.value(kSubs + QStringLiteral("maxWords"), 3).toInt());
	mode_->setCurrentIndex(std::clamp(s.value(kSubs + QStringLiteral("mode"), 0).toInt(), 0, 2));
	position_->setCurrentIndex(std::clamp(s.value(kSubs + QStringLiteral("position"), 0).toInt(), 0, 2));
	fontPx_->setValue(s.value(kSubs + QStringLiteral("fontPx"), 64).toInt());
	const int pi = provider_->findData(s.value(kGrp + QStringLiteral("provider"), QStringLiteral("openai")).toString());
	const QSignalBlocker block(provider_);
	provider_->setCurrentIndex(std::max(0, pi));
	refreshProvider();
	refreshCount();
}

void TtsDialog::saveSettings()
{
	QSettings s = settings();
	s.setValue(kGrp + QStringLiteral("provider"), provider_->currentData().toString());
	s.setValue(kGrp + QStringLiteral("language"), languageCode());
	s.setValue(kGrp + QStringLiteral("voice_") + provider_->currentData().toString(), voice());
	s.setValue(kGrp + QStringLiteral("speed_") + provider_->currentData().toString(), speed_->value());
	s.setValue(kGrp + QStringLiteral("style"), style_->text());
	s.setValue(kSubs + QStringLiteral("maxWords"), maxWords_->value());
	s.setValue(kSubs + QStringLiteral("mode"), mode_->currentIndex());
	s.setValue(kSubs + QStringLiteral("position"), position_->currentIndex());
	s.setValue(kSubs + QStringLiteral("fontPx"), fontPx_->value());
}

TtsProvider TtsDialog::provider() const
{
	return ttsProviderFromId(provider_->currentData().toString());
}

QString TtsDialog::languageCode() const
{
	return language_->currentData().toString();
}

QString TtsDialog::voice() const
{
	// A picked entry's id; a typed one as typed.
	const int i = voice_->currentIndex();
	if (i >= 0 && voice_->itemText(i) == voice_->currentText())
		return voice_->itemData(i).toString();
	return voice_->currentText().trimmed();
}

void TtsDialog::setVoices(const QVector<TtsVoice> &voices, const QString &select)
{
	voice_->clear();
	for (const TtsVoice &v : voices)
		voice_->addItem(v.name, v.id);
	const int i = voice_->findData(select);
	if (i >= 0)
		voice_->setCurrentIndex(i);
	else if (!select.isEmpty())
		voice_->setEditText(select);
}

void TtsDialog::refreshProvider()
{
	const TtsProviderInfo &i = ttsProviderInfo(provider());
	const QString name = QString::fromUtf8(i.name);
	const QString shortName = name.section(QLatin1Char(' '), 0, 0);
	about_->setText(QString::fromUtf8(i.about));
	QSettings s = settings();
	setVoices(ttsDefaultVoices(provider(), languageCode()),
		  s.value(kGrp + QStringLiteral("voice_") + QString::fromLatin1(i.id)).toString());
	loadVoices_->setVisible(i.listsVoices);
	style_->setVisible(i.hasStyle);
	styleLabel_->setVisible(i.hasStyle);
	speed_->setVisible(i.hasSpeed);
	speedLabel_->setVisible(i.hasSpeed);
	if (provider() == TtsProvider::ElevenLabs)
		speed_->setRange(0.7, 1.2);
	else
		speed_->setRange(0.25, 4.0);
	speed_->setValue(s.value(kGrp + QStringLiteral("speed_") + QString::fromLatin1(i.id), 1.0).toDouble());

	apiKey_->setPlaceholderText(QString::fromUtf8(i.keyPlaceholder));
	apiKey_->setToolTip(QStringLiteral("Your own %1 API key. Kept on this computer only -- sealed with Windows' "
					   "credential protection, never written into a project.")
				    .arg(name));
	const QString url = QString::fromLatin1(i.keyUrl);
	getKey_->setText(QStringLiteral("<a href='%1' style='color:#6ea8fe;'>Get your %2 API key ↗</a>"
					"&nbsp;&nbsp;<span style='color:#7f858e;'>(%3)</span>")
				 .arg(url, shortName.toHtmlEscaped(), QString::fromUtf8(i.keySteps).toHtmlEscaped()));
	keyFor_ = provider();
	const QString k = loadKey(provider());
	const bool shared = provider() != TtsProvider::Google;
	keyState_->setText(k.isEmpty()
				   ? QStringLiteral("No %1 key saved yet. Paste one: it is saved as soon as you leave the box.")
					     .arg(shortName)
				   : QStringLiteral("✓ %1 key saved on this computer: %2%3")
					     .arg(shortName, SecretStore::maskedKey(k),
						  shared ? QStringLiteral(" (the same key the Subtitles window uses).")
							 : QStringLiteral(".")));
	if (!k.isEmpty())
		apiKey_->setPlaceholderText(QStringLiteral("Saved: %1  (paste to replace)").arg(SecretStore::maskedKey(k)));
}

bool TtsDialog::storeTypedKey(TtsProvider p)
{
	const QString typed = apiKey_->text().trimmed();
	if (typed.isEmpty())
		return false;
	if (typed == loadKey(p)) {
		apiKey_->clear();
		return true;
	}
	const bool saved = saveKey(p, typed);
	if (saved) {
		apiKey_->clear();
		if (p == provider())
			refreshProvider();
	} else {
		status_->setText(QStringLiteral("The %1 key could not be saved on this computer, so it is only used "
						"until this window closes. Help > Error logs says why.")
					 .arg(ttsProviderName(p)));
	}
	return saved;
}

QString TtsDialog::currentKey() const
{
	const QString typed = apiKey_->text().trimmed();
	return typed.isEmpty() ? loadKey(provider()) : typed;
}

void TtsDialog::setText(const QString &text)
{
	text_->setPlainText(text);
}

QString TtsDialog::text() const
{
	return text_->toPlainText();
}

void TtsDialog::refreshCount()
{
	const int words = int(ttsWords(text()).size());
	// About 150 words a minute, the pace of most narration.
	const int sec = int(std::lround(words / 2.5));
	count_->setText(words == 0 ? QString()
				   : QStringLiteral("%1 words · about %2:%3 of speech")
					     .arg(words)
					     .arg(sec / 60)
					     .arg(sec % 60, 2, 10, QLatin1Char('0')));
}

QString TtsDialog::readTextFile(const QString &path, QString *err)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (err)
			*err = QStringLiteral("could not open %1").arg(QFileInfo(path).fileName());
		return QString();
	}
	const QByteArray bytes = f.readAll();
	QStringDecoder utf8(QStringConverter::Utf8);
	QString text = utf8(bytes);
	if (utf8.hasError())
		text = QString::fromLatin1(bytes); // an old ANSI file
	if (path.endsWith(QLatin1String(".srt"), Qt::CaseInsensitive)) {
		QStringList keep;
		static const QRegularExpression number(QStringLiteral("^\\d+$"));
		for (const QString &line : text.split(QLatin1Char('\n'))) {
			const QString t = line.trimmed();
			if (t.isEmpty() || number.match(t).hasMatch() || t.contains(QLatin1String("-->")))
				continue;
			keep << t;
		}
		text = keep.join(QLatin1Char(' '));
	}
	return text.trimmed();
}

void TtsDialog::openTextFile()
{
	QSettings s = settings();
	const QString dir = s.value(kGrp + QStringLiteral("lastFolder"),
				    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
				    .toString();
	const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open a text"), dir,
							  QStringLiteral("Text (*.txt *.md *.srt);;All files (*)"));
	if (path.isEmpty())
		return;
	s.setValue(kGrp + QStringLiteral("lastFolder"), QFileInfo(path).absolutePath());
	QString err;
	const QString t = readTextFile(path, &err);
	if (!err.isEmpty()) {
		status_->setText(err);
		return;
	}
	setText(t);
	status_->setText(QStringLiteral("Read %1.").arg(QFileInfo(path).fileName()));
}

void TtsDialog::loadVoices()
{
	storeTypedKey(provider());
	loadVoices_->setEnabled(false);
	status_->setText(QStringLiteral("Asking %1 for your voices…").arg(ttsProviderName(provider())));
	synth_->listVoices(provider(), currentKey(), languageCode());
}

GroupRule TtsDialog::groupRule() const
{
	QSettings s = settings();
	GroupRule r;
	r.maxWords = maxWords_->value();
	r.maxDurationMs = qint64(s.value(kSubs + QStringLiteral("maxSeconds"), 2.5).toDouble() * 1000.0);
	r.pauseMs = s.value(kSubs + QStringLiteral("pauseMs"), 400).toInt();
	r.maxChars = s.value(kSubs + QStringLiteral("maxChars"), 32).toInt();
	return r;
}

SubtitleLook TtsDialog::look() const
{
	QSettings s = settings();
	SubtitleLook l;
	l.mode = SubtitleMode(mode_->currentIndex());
	l.position = SubtitleLook::Position(position_->currentIndex());
	l.style.fontPx = fontPx_->value();
	l.style.bold = s.value(kSubs + QStringLiteral("bold"), true).toBool();
	l.style.boxEnabled = s.value(kSubs + QStringLiteral("box"), false).toBool();
	return l;
}

QString TtsDialog::speechFolder()
{
	return QDir(harpiaBaseFolder()).filePath(QStringLiteral("Speech"));
}

QString TtsDialog::fileNameFor(const QString &text, const QDateTime &when)
{
	// The first few words, as a file name: "2026-10-15 14-03-22 Ola a todos.wav".
	QStringList words;
	for (const TtsWord &w : ttsWords(text)) {
		QString clean;
		for (const QChar c : w.text)
			if (c.isLetterOrNumber())
				clean += c;
		if (!clean.isEmpty())
			words << clean;
		if (words.size() == 5)
			break;
	}
	QString name = when.toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"));
	if (!words.isEmpty())
		name += QLatin1Char(' ') + words.join(QLatin1Char(' ')).left(48);
	return name + QStringLiteral(".wav");
}

void TtsDialog::setBusy(bool on)
{
	running_ = on;
	go_->setEnabled(!on);
	provider_->setEnabled(!on);
	language_->setEnabled(!on);
	voice_->setEnabled(!on);
	text_->setReadOnly(on);
	cancel_->setText(on ? QStringLiteral("Stop") : QStringLiteral("Cancel"));
}

void TtsDialog::startJob()
{
	if (running_)
		return;
	const QString t = text().trimmed();
	if (ttsWords(t).isEmpty()) {
		status_->setText(QStringLiteral("Type or paste a text first."));
		text_->setFocus();
		return;
	}
	storeTypedKey(provider());
	if (currentKey().isEmpty()) {
		status_->setText(QStringLiteral("Paste your %1 API key first.").arg(ttsProviderName(provider())));
		apiKey_->setFocus();
		return;
	}
	if (voice().isEmpty()) {
		status_->setText(QStringLiteral("Pick a voice first."));
		return;
	}
	saveSettings();
	const TtsProviderInfo &info = ttsProviderInfo(provider());
	pieces_ = provider() == TtsProvider::Google ? ttsChunks(t, info.maxChars, googleSsmlBytes)
						    : ttsChunks(t, info.maxChars);
	pieceIdx_ = 0;
	pieceWavs_.clear();
	pieceWords_.clear();
	if (workDir_.isEmpty())
		workDir_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
			   QStringLiteral("/harpia-tts-%1").arg(QCoreApplication::applicationPid());
	QDir(workDir_).removeRecursively();
	QDir().mkpath(workDir_);
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("%1 words in %2 part(s), %3, voice %4, language %5")
			  .arg(ttsWords(t).size())
			  .arg(pieces_.size())
			  .arg(ttsProviderName(provider()), voice(), languageCode()));
	setBusy(true);
	nextPiece();
}

void TtsDialog::nextPiece()
{
	if (!running_)
		return;
	if (pieceIdx_ >= pieces_.size()) {
		assemble();
		return;
	}
	TtsRequest r;
	r.provider = provider();
	r.apiKey = currentKey();
	r.text = pieces_[pieceIdx_];
	r.voice = voice();
	r.language = languageCode();
	r.style = style_->isVisible() ? style_->text() : QString();
	r.speed = speed_->isVisible() ? speed_->value() : 1.0;
	if (pieceIdx_ > 0)
		r.previousText = pieces_[pieceIdx_ - 1];
	if (pieceIdx_ + 1 < pieces_.size())
		r.nextText = pieces_[pieceIdx_ + 1];
	status_->setText(QStringLiteral("Speaking part %1 of %2 with %3…")
				 .arg(pieceIdx_ + 1)
				 .arg(pieces_.size())
				 .arg(ttsProviderName(provider())));
	synth_->synthesize(r);
}

void TtsDialog::onSynthesized(const TtsReply &reply)
{
	if (!running_)
		return;
	// The piece as it came, then as the editor plays audio: 48 kHz stereo.
	const QString raw = workDir_ + QStringLiteral("/piece_%1.%2").arg(pieceIdx_).arg(reply.ext);
	const QString wav = workDir_ + QStringLiteral("/piece_%1_48k.wav").arg(pieceIdx_);
	QFile f(raw);
	if (!f.open(QIODevice::WriteOnly) || f.write(reply.audio) != reply.audio.size()) {
		finishJob(QStringLiteral("could not write the audio to %1").arg(QDir::toNativeSeparators(workDir_)));
		return;
	}
	f.close();
	tts_audio::Pcm pcm;
	if (!VoiceoverMixer::decodeToWav(raw, wav) || !tts_audio::readWav16(wav, pcm)) {
		finishJob(QStringLiteral("the audio %1 sent could not be read").arg(ttsProviderName(provider())));
		return;
	}
	pieceWavs_.append(wav);
	pieceMs_ = pcm.durationMs();
	if (reply.timed) {
		pieceDone(reply.words);
		return;
	}
	// Audio only: hear it back to find where each word is.
	QString err;
	const QVector<SpeechAudioChunk> chunks = AudioForSpeech::prepare(wav, 0, pieceMs_, workDir_, &err);
	if (chunks.isEmpty()) {
		finishJob(QStringLiteral("preparing the audio for word timing: %1").arg(err));
		return;
	}
	SpeechTranscriber::Job job;
	job.provider = transcriberFor(provider());
	job.wavPath = chunks.first().wavPath;
	job.apiKey = currentKey();
	job.language = languageCode();
	status_->setText(QStringLiteral("Timing the words of part %1 of %2…").arg(pieceIdx_ + 1).arg(pieces_.size()));
	transcriber_->transcribe(job);
}

void TtsDialog::onTranscribed(const Transcript &t)
{
	if (!running_)
		return;
	QVector<bool> matched;
	QVector<ClipWordTime> words = alignToTranscript(ttsWords(pieces_[pieceIdx_]), t.words, &matched);
	const int hits = int(std::count(matched.begin(), matched.end(), true));
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("part %1: %2 of %3 words matched to what was heard")
			  .arg(pieceIdx_ + 1)
			  .arg(hits)
			  .arg(words.size()));
	fillUnmatched(words, matched, pieceMs_);
	pieceDone(words);
}

void TtsDialog::pieceDone(const QVector<ClipWordTime> &words)
{
	pieceWords_.append(words);
	++pieceIdx_;
	nextPiece();
}

void TtsDialog::assemble()
{
	QVector<tts_audio::Pcm> pcms;
	for (const QString &w : pieceWavs_) {
		tts_audio::Pcm p;
		if (!tts_audio::readWav16(w, p)) {
			finishJob(QStringLiteral("a piece of the audio could not be read back"));
			return;
		}
		pcms.append(p);
	}
	QVector<qint64> offsets;
	const tts_audio::Pcm all = tts_audio::join(pcms, kPieceGapMs, &offsets);
	QVector<ClipWordTime> words;
	for (int i = 0; i < pieceWords_.size() && i < offsets.size(); ++i)
		for (ClipWordTime w : pieceWords_[i]) {
			w.startMs += offsets[i];
			w.endMs += offsets[i];
			words.append(w);
		}
	const QString folder = speechFolder();
	QDir().mkpath(folder);
	const QString path = QDir(folder).filePath(fileNameFor(text(), QDateTime::currentDateTime()));
	if (!tts_audio::writeWav16(path, all)) {
		finishJob(QStringLiteral("could not save the narration in %1").arg(QDir::toNativeSeparators(folder)));
		return;
	}
	editorLog(EditorLogLevel::Info, kWhere,
		  QStringLiteral("narration saved: %1 (%2 s, %3 words)")
			  .arg(QDir::toNativeSeparators(path))
			  .arg(all.durationMs() / 1000.0, 0, 'f', 1)
			  .arg(words.size()));
	finishJob(QString());
	status_->setText(QStringLiteral("Done: %1 s of speech, %2 words. It is on the timeline at the playhead; "
					"Ctrl+Z removes it.")
				 .arg(all.durationMs() / 1000.0, 0, 'f', 1)
				 .arg(words.size()));
	emit speechReady(path, all.durationMs(), words, groupRule(), look());
}

void TtsDialog::finishJob(const QString &error)
{
	setBusy(false);
	if (!workDir_.isEmpty())
		QDir(workDir_).removeRecursively();
	if (!error.isEmpty()) {
		status_->setText(QStringLiteral("Stopped: %1").arg(error));
		editorLog(EditorLogLevel::Error, kWhere,
			  QStringLiteral("stopped at part %1 of %2: %3").arg(pieceIdx_ + 1).arg(pieces_.size()).arg(error));
	}
}

} // namespace harpia
